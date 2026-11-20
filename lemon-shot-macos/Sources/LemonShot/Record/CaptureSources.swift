import AppKit
import AVFoundation
import IOSurface
@_weakLinked import ScreenCaptureKit

// 采集源（spec 8.3、10）：画面 macOS 12.3+ SCStream、11–12.2 CGDisplayStream；系统声音 macOS 13+ SCStream.capturesAudio；麦克风 AVCaptureSession。
// 回调都在调用方给的串行队列上（RecordPipeline.queue），时间戳统一为主机时钟秒。

/// 画面采集参数。
struct ScreenSourceParams {
    var displayID: CGDirectDisplayID
    /// 显示器内的区域（逻辑点，左上原点）。
    var sourceRect: CGRect
    var width: Int
    var height: Int
    var fps: Int
    var pixelFormat: OSType
    var showsCursor: Bool
    /// 是否同时采集系统声音（macOS 13+）。
    var captureAudio: Bool
    /// 只要声音（录音）：画面压到最小。
    var audioOnly: Bool
    /// 自己的这些窗口要进画面（画笔层、点击圈、按键浮层、摄像头）；自己的其他窗口一律排除。
    var includeWindows: [CGWindowID]
}

protocol ScreenSource: AnyObject {
    var onVideo: ((CVPixelBuffer, Double) -> Void)? { get set }
    var onAudio: ((CMSampleBuffer, Double) -> Void)? { get set }
    /// 采集意外中止（权限被收回、显示器断开），主线程回调。
    var onError: ((Error) -> Void)? { get set }
    /// completion 在主线程。
    func start(_ p: ScreenSourceParams, completion: @escaping (Error?) -> Void)
    func stop(completion: @escaping () -> Void)
}

enum ScreenSources {
    static func make(queue: DispatchQueue) -> ScreenSource {
        if #available(macOS 12.3, *) { return SCKScreenSource(queue: queue) }
        return DisplayStreamSource(queue: queue)
    }
}

// MARK: - ScreenCaptureKit

@available(macOS 12.3, *)
final class SCKScreenSource: NSObject, ScreenSource, SCStreamOutput, SCStreamDelegate {
    var onVideo: ((CVPixelBuffer, Double) -> Void)?
    var onAudio: ((CMSampleBuffer, Double) -> Void)?
    var onError: ((Error) -> Void)?
    private let queue: DispatchQueue
    private var stream: SCStream?
    private var stopped = false

    init(queue: DispatchQueue) {
        self.queue = queue
    }

    func start(_ p: ScreenSourceParams, completion: @escaping (Error?) -> Void) {
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: false) { content, error in
            DispatchQueue.main.async {
                guard !self.stopped else { completion(nil); return }
                guard let content = content, let display = content.displays.first(where: { $0.displayID == p.displayID }) else {
                    completion(error ?? RecordError.noDisplay)
                    return
                }
                let me = ProcessInfo.processInfo.processIdentifier
                let keep = content.windows.filter { p.includeWindows.contains($0.windowID) }
                let filter: SCContentFilter
                if let app = content.applications.first(where: { $0.processID == me }) {
                    filter = SCContentFilter(display: display, excludingApplications: [app], exceptingWindows: keep)
                } else {
                    let mine = content.windows.filter { $0.owningApplication?.processID == me && !p.includeWindows.contains($0.windowID) }
                    filter = SCContentFilter(display: display, excludingWindows: mine)
                }
                let cfg = SCStreamConfiguration()
                if p.audioOnly {
                    cfg.width = 2
                    cfg.height = 2
                    cfg.minimumFrameInterval = CMTime(value: 1, timescale: 1)
                    cfg.showsCursor = false
                } else {
                    cfg.sourceRect = p.sourceRect
                    cfg.width = p.width
                    cfg.height = p.height
                    cfg.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(max(1, p.fps)))
                    cfg.showsCursor = p.showsCursor
                    cfg.queueDepth = 8
                    cfg.colorSpaceName = CGColorSpace.sRGB
                    cfg.colorMatrix = CGDisplayStream.yCbCrMatrix_ITU_R_709_2
                }
                cfg.pixelFormat = p.pixelFormat
                if p.captureAudio, #available(macOS 13.0, *) {
                    cfg.capturesAudio = true
                    cfg.sampleRate = Int(AudioMixer.rate)
                    cfg.channelCount = 2
                    cfg.excludesCurrentProcessAudio = true
                }
                let stream = SCStream(filter: filter, configuration: cfg, delegate: self)
                do {
                    try stream.addStreamOutput(self, type: .screen, sampleHandlerQueue: self.queue)
                    if p.captureAudio, #available(macOS 13.0, *) {
                        try stream.addStreamOutput(self, type: .audio, sampleHandlerQueue: self.queue)
                    }
                } catch {
                    completion(error)
                    return
                }
                self.stream = stream
                stream.startCapture { err in
                    DispatchQueue.main.async {
                        if err != nil { self.stream = nil }
                        completion(err)
                    }
                }
            }
        }
    }

    func stop(completion: @escaping () -> Void) {
        stopped = true
        guard let s = stream else { completion(); return }
        stream = nil
        s.stopCapture { _ in DispatchQueue.main.async { completion() } }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard CMSampleBufferIsValid(sb) else { return }
        if type == .screen {
            guard let cb = onVideo, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
            if let arr = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
               let raw = arr.first?[.status] as? Int, let status = SCFrameStatus(rawValue: raw), status != .complete {
                return
            }
            cb(pb, CMSampleBufferGetPresentationTimeStamp(sb).seconds)
        } else if #available(macOS 13.0, *), type == .audio {
            onAudio?(sb, CMSampleBufferGetPresentationTimeStamp(sb).seconds)
        }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        DispatchQueue.main.async {
            guard !self.stopped else { return }
            self.stream = nil
            self.onError?(error)
        }
    }
}

// MARK: - CGDisplayStream（macOS 11–12.2）

/// SDK 已把 CGDisplayStream 标为不可用：运行时用 dlsym 取函数与常量。不能排除窗口，也没有系统声音。
final class DisplayStreamSource: ScreenSource {
    var onVideo: ((CVPixelBuffer, Double) -> Void)?
    var onAudio: ((CMSampleBuffer, Double) -> Void)?
    var onError: ((Error) -> Void)?
    private let queue: DispatchQueue
    private var stream: OpaquePointer?
    private var pool: CVPixelBufferPool?

    typealias FrameHandler = @convention(block) (Int32, UInt64, IOSurfaceRef?, OpaquePointer?) -> Void
    private typealias CreateFn = @convention(c) (CGDirectDisplayID, Int, Int, Int32, CFDictionary?, DispatchQueue, FrameHandler?) -> OpaquePointer?
    private typealias StartStopFn = @convention(c) (OpaquePointer?) -> Int32

    private static func fn<T>(_ name: String, _ type: T.Type) -> T? {
        guard let p = dlsym(UnsafeMutableRawPointer(bitPattern: -2), name) else { return nil }
        return unsafeBitCast(p, to: type)
    }

    private static func constant(_ name: String) -> CFString? {
        guard let p = dlsym(UnsafeMutableRawPointer(bitPattern: -2), name) else { return nil }
        return p.load(as: Unmanaged<CFString>?.self)?.takeUnretainedValue()
    }

    init(queue: DispatchQueue) {
        self.queue = queue
    }

    func start(_ p: ScreenSourceParams, completion: @escaping (Error?) -> Void) {
        guard !p.audioOnly else { completion(RecordError.capture("audio")); return }
        guard let create = DisplayStreamSource.fn("CGDisplayStreamCreateWithDispatchQueue", CreateFn.self),
              let startFn = DisplayStreamSource.fn("CGDisplayStreamStart", StartStopFn.self) else {
            completion(RecordError.capture("CGDisplayStream"))
            return
        }
        var props: [CFString: Any] = [:]
        if let k = DisplayStreamSource.constant("kCGDisplayStreamSourceRect") { props[k] = p.sourceRect.dictionaryRepresentation }
        if let k = DisplayStreamSource.constant("kCGDisplayStreamMinimumFrameTime") { props[k] = 1.0 / Double(max(1, p.fps)) }
        if let k = DisplayStreamSource.constant("kCGDisplayStreamShowCursor") { props[k] = p.showsCursor ? kCFBooleanTrue : kCFBooleanFalse }
        if let k = DisplayStreamSource.constant("kCGDisplayStreamQueueDepth") { props[k] = 6 }
        let attrs: [String: Any] = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
                                    kCVPixelBufferWidthKey as String: p.width, kCVPixelBufferHeightKey as String: p.height,
                                    kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any]()]
        var newPool: CVPixelBufferPool?
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &newPool)
        pool = newPool
        let handler: FrameHandler = { [weak self] status, displayTime, surface, _ in
            // 0 = kCGDisplayStreamFrameStatusFrameComplete
            guard status == 0, let self = self, let surface = surface, let cb = self.onVideo, let pb = self.copy(surface) else { return }
            cb(pb, CMClockMakeHostTimeFromSystemUnits(displayTime).seconds)
        }
        // 统一用 BGRA（老系统少见 4K60，复制开销可以接受；GIF 也要 BGRA）
        let fmt = Int32(bitPattern: kCVPixelFormatType_32BGRA)
        guard let s = create(p.displayID, p.width, p.height, fmt, props as CFDictionary, queue, handler) else {
            completion(RecordError.capture("CGDisplayStream"))
            return
        }
        stream = s
        let err = startFn(s)
        completion(err == 0 ? nil : RecordError.capture("CGDisplayStream \(err)"))
    }

    /// IOSurface 在回调返回后会被系统复用：复制进自己的像素缓冲再交给编码器。
    private func copy(_ surface: IOSurfaceRef) -> CVPixelBuffer? {
        guard let pool = pool else { return nil }
        var out: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out) == kCVReturnSuccess, let pb = out else { return nil }
        IOSurfaceLock(surface, .readOnly, nil)
        CVPixelBufferLockBaseAddress(pb, [])
        defer {
            CVPixelBufferUnlockBaseAddress(pb, [])
            IOSurfaceUnlock(surface, .readOnly, nil)
        }
        let src = IOSurfaceGetBaseAddress(surface)
        let srcStride = IOSurfaceGetBytesPerRow(surface)
        guard let dst = CVPixelBufferGetBaseAddress(pb) else { return nil }
        let dstStride = CVPixelBufferGetBytesPerRow(pb)
        let rows = min(IOSurfaceGetHeight(surface), CVPixelBufferGetHeight(pb))
        let bytes = min(srcStride, dstStride, min(IOSurfaceGetWidth(surface), CVPixelBufferGetWidth(pb)) * 4)
        for y in 0..<rows { memcpy(dst.advanced(by: y * dstStride), src.advanced(by: y * srcStride), bytes) }
        return pb
    }

    func stop(completion: @escaping () -> Void) {
        if let s = stream {
            stream = nil
            if let stopFn = DisplayStreamSource.fn("CGDisplayStreamStop", StartStopFn.self) { _ = stopFn(s) }
            // 等已排队的回调跑完再释放
            queue.async {
                Unmanaged<AnyObject>.fromOpaque(UnsafeRawPointer(s)).release()
                DispatchQueue.main.async { completion() }
            }
            return
        }
        completion()
    }
}

// MARK: - 麦克风

/// 麦克风：AVCaptureSession 音频，时间戳从会话时钟换算到主机时钟。
final class MicSource: NSObject, AVCaptureAudioDataOutputSampleBufferDelegate {
    private let session = AVCaptureSession()
    private let output = AVCaptureAudioDataOutput()
    private static let controlQueue = DispatchQueue(label: "cn.lemonit.shot.mic-control")
    var onAudio: ((CMSampleBuffer, Double) -> Void)?

    /// 设备不存在时退回默认麦克风；一个都没有返回 nil。
    init?(deviceID: String, queue: DispatchQueue) {
        let dev = (deviceID.isEmpty ? nil : AVCaptureDevice(uniqueID: deviceID)) ?? AVCaptureDevice.default(for: .audio)
        guard let device = dev, let input = try? AVCaptureDeviceInput(device: device) else { return nil }
        super.init()
        session.beginConfiguration()
        guard session.canAddInput(input), session.canAddOutput(output) else {
            session.commitConfiguration()
            return nil
        }
        session.addInput(input)
        session.addOutput(output)
        session.commitConfiguration()
        output.setSampleBufferDelegate(self, queue: queue)
    }

    func start() {
        let s = session
        MicSource.controlQueue.async { s.startRunning() }
    }

    func stop(completion: (() -> Void)? = nil) {
        let s = session
        MicSource.controlQueue.async {
            s.stopRunning()
            DispatchQueue.main.async { completion?() }
        }
    }

    private var clock: CMClock? {
        if #available(macOS 12.3, *) { return session.synchronizationClock }
        return session.masterClock
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sb: CMSampleBuffer, from connection: AVCaptureConnection) {
        var pts = CMSampleBufferGetPresentationTimeStamp(sb)
        if let c = clock { pts = CMSyncConvertTime(pts, from: c, to: CMClockGetHostTimeClock()) }
        onAudio?(sb, pts.seconds)
    }
}

/// 麦克风、摄像头权限：未决定时先请求，回调在主线程。
enum CapturePermission {
    static func ensure(_ type: AVMediaType, completion: @escaping (Bool) -> Void) {
        switch AVCaptureDevice.authorizationStatus(for: type) {
        case .authorized: completion(true)
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: type) { ok in DispatchQueue.main.async { completion(ok) } }
        default: completion(false)
        }
    }
}
