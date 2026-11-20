import AppKit
import CoreMedia
@_weakLinked import ScreenCaptureKit

/// 按区域抓屏，排除自己的窗口（长截图每 100ms 一帧；文字识别「固定区域」抓一次）。
/// macOS 14+ 用 SCScreenshotManager；12.3–13 用 SCStream（每 100ms 一帧，取最新一帧）；
/// 11–12.2 用 CGWindowListCreateImageFromArray（SDK 已标为不可用，运行时 dlsym 取），只合成别人的窗口。
final class RegionGrabber {
    /// Cocoa 全局坐标。
    let region: CGRect
    let scale: CGFloat
    let pixelWidth: Int
    let pixelHeight: Int
    private let displayID: CGDirectDisplayID
    /// 显示器内坐标（左上原点，点）。
    private let localRect: CGRect
    /// CG 全局坐标（主屏左上原点，y 向下）。
    private let cgRect: CGRect
    /// SC 实现对象（类型只在新系统存在，存成 AnyObject）。
    private var impl: AnyObject?

    init(region: CGRect, screen: NSScreen) {
        self.region = region
        scale = screen.backingScaleFactor
        pixelWidth = max(1, Int((region.width * scale).rounded()))
        pixelHeight = max(1, Int((region.height * scale).rounded()))
        displayID = ScreenCapture.displayID(screen)
        let sf = screen.frame
        localRect = CGRect(x: region.minX - sf.minX, y: sf.maxY - region.maxY, width: region.width, height: region.height)
        let primaryH = NSScreen.screens.first?.frame.height ?? sf.height
        cgRect = CGRect(x: region.minX, y: primaryH - region.maxY, width: region.width, height: region.height)
    }

    /// 准备（取可共享内容、建过滤器、开流）。回调在主线程；失败时退回老接口，仍然返回真。
    func start(_ ready: @escaping (Bool) -> Void) {
        guard #available(macOS 12.3, *) else {
            ready(RegionGrabber.fromArray != nil || RegionGrabber.legacyCreate != nil)
            return
        }
        let did = displayID, rect = localRect, pw = pixelWidth, ph = pixelHeight
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { [weak self] content, _ in
            DispatchQueue.main.async {
                guard let self = self else { return }
                guard let content = content, let display = content.displays.first(where: { $0.displayID == did }) else {
                    ready(true)
                    return
                }
                let me = content.applications.filter { $0.processID == ProcessInfo.processInfo.processIdentifier }
                let filter = SCContentFilter(display: display, excludingApplications: me, exceptingWindows: [])
                let cfg = SCStreamConfiguration()
                cfg.sourceRect = rect
                cfg.width = pw
                cfg.height = ph
                cfg.showsCursor = false
                cfg.pixelFormat = kCVPixelFormatType_32BGRA
                if #available(macOS 14.0, *) {
                    cfg.captureResolution = .best
                    self.impl = ShotGrabber(filter: filter, config: cfg)
                    ready(true)
                } else {
                    cfg.minimumFrameInterval = CMTime(value: 1, timescale: 10)
                    cfg.queueDepth = 3
                    let s = StreamGrabber()
                    self.impl = s
                    s.start(filter: filter, config: cfg) { ok in
                        if !ok { self.impl = nil }
                        ready(true)
                    }
                }
            }
        }
    }

    /// 抓一帧。回调线程不定。
    func grab(_ completion: @escaping (CGImage?) -> Void) {
        if #available(macOS 14.0, *), let s = impl as? ShotGrabber {
            s.grab { img in completion(img ?? self.legacyGrab()) }
            return
        }
        if #available(macOS 12.3, *), let s = impl as? StreamGrabber {
            s.grab(completion)
            return
        }
        completion(legacyGrab())
    }

    func stop() {
        if #available(macOS 12.3, *), let s = impl as? StreamGrabber { s.stop() }
        impl = nil
    }

    /// 抓一次就结束（文字识别固定区域）。回调在主线程。
    static func grabOnce(region: CGRect, screen: NSScreen, completion: @escaping (CGImage?) -> Void) {
        let g = RegionGrabber(region: region, screen: screen)
        g.start { _ in
            g.grab { img in
                DispatchQueue.main.async {
                    g.stop()
                    completion(img)
                }
            }
        }
    }

    // MARK: 老系统

    private typealias FromArrayFn = @convention(c) (CGRect, CFArray, UInt32) -> Unmanaged<CGImage>?
    private typealias CreateImageFn = @convention(c) (CGRect, UInt32, UInt32, UInt32) -> Unmanaged<CGImage>?

    private static let fromArray: FromArrayFn? = {
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImageFromArray") else { return nil }
        return unsafeBitCast(sym, to: FromArrayFn.self)
    }()

    private static let legacyCreate: CreateImageFn? = {
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage") else { return nil }
        return unsafeBitCast(sym, to: CreateImageFn.self)
    }()

    /// 屏上除自己以外的全部窗口按 z 序合成区域图（kCGWindowImageBestResolution = 8）。
    private func legacyGrab() -> CGImage? {
        let me = ProcessInfo.processInfo.processIdentifier
        if let fn = RegionGrabber.fromArray, let arr = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] {
            var ptrs: [UnsafeRawPointer?] = []
            for d in arr {
                guard let num = d[kCGWindowNumber as String] as? NSNumber else { continue }
                if let pid = (d[kCGWindowOwnerPID as String] as? NSNumber)?.int32Value, pid == me { continue }
                ptrs.append(UnsafeRawPointer(bitPattern: UInt(num.uint32Value)))
            }
            if let cf = ptrs.withUnsafeMutableBufferPointer({ CFArrayCreate(nil, $0.baseAddress, $0.count, nil) }),
               let img = fn(cgRect, cf, 8)?.takeRetainedValue() {
                return img
            }
        }
        // kCGWindowListOptionOnScreenOnly = 1
        return RegionGrabber.legacyCreate?(cgRect, 1, 0, 8)?.takeRetainedValue()
    }
}

// MARK: - macOS 14+：SCScreenshotManager

@available(macOS 14.0, *)
private final class ShotGrabber {
    let filter: SCContentFilter
    let config: SCStreamConfiguration
    init(filter: SCContentFilter, config: SCStreamConfiguration) {
        self.filter = filter
        self.config = config
    }

    func grab(_ completion: @escaping (CGImage?) -> Void) {
        SCScreenshotManager.captureImage(contentFilter: filter, configuration: config) { img, _ in completion(img) }
    }
}

// MARK: - macOS 12.3–13：SCStream

@available(macOS 12.3, *)
private final class StreamGrabber: NSObject, SCStreamOutput, SCStreamDelegate {
    private var stream: SCStream?
    private let queue = DispatchQueue(label: "cn.lemonit.shot.region-stream")
    private let lock = NSLock()
    private var latest: CGImage?
    private var waiters: [(CGImage?) -> Void] = []
    private var stopped = false

    func start(filter: SCContentFilter, config: SCStreamConfiguration, ready: @escaping (Bool) -> Void) {
        let s = SCStream(filter: filter, configuration: config, delegate: self)
        do {
            try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: queue)
        } catch {
            ready(false)
            return
        }
        stream = s
        s.startCapture { err in
            DispatchQueue.main.async { ready(err == nil) }
        }
    }

    func grab(_ completion: @escaping (CGImage?) -> Void) {
        lock.lock()
        if let l = latest {
            lock.unlock()
            completion(l)
            return
        }
        if stopped {
            lock.unlock()
            completion(nil)
            return
        }
        waiters.append(completion)
        lock.unlock()
        // 一秒内没有新画面就放弃这一帧
        queue.asyncAfter(deadline: .now() + 1) { [weak self] in self?.flushWaiters(nil) }
    }

    private func flushWaiters(_ img: CGImage?) {
        lock.lock()
        let ws = waiters
        waiters.removeAll()
        lock.unlock()
        for w in ws { w(img) }
    }

    func stop() {
        lock.lock()
        stopped = true
        lock.unlock()
        flushWaiters(nil)
        stream?.stopCapture(completionHandler: nil)
        stream = nil
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, CMSampleBufferIsValid(sampleBuffer),
              let atts = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let raw = atts.first?[.status] as? Int, let status = SCFrameStatus(rawValue: raw), status == .complete,
              let pb = CMSampleBufferGetImageBuffer(sampleBuffer), let img = StreamGrabber.image(pb) else { return }
        lock.lock()
        latest = img
        lock.unlock()
        flushWaiters(img)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        lock.lock()
        stopped = true
        lock.unlock()
        flushWaiters(nil)
    }

    /// BGRA 像素缓冲复制成 CGImage。
    static func image(_ pb: CVPixelBuffer) -> CGImage? {
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb) else { return nil }
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb)
        let data = Data(bytes: base, count: bpr * h)
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        let space = CVImageBufferGetColorSpace(pb)?.takeUnretainedValue() ?? Theme.sRGB
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: bpr, space: space,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}
