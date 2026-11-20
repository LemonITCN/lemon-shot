import AVFoundation
import LemonShotCore

// 录屏/录音的数据通路（spec 8.3、10）：时间线（扣掉暂停）、两路音频重采样混音、H.264/AAC 写文件、GIF 原始帧。
// 这里的类都只在同一个串行队列（RecordPipeline.queue）上使用；采集回调直接投递到这个队列。

/// 录制时间线：主机时间（秒）→ 录制时间（秒，第一帧为 0，暂停的时间扣掉）。
final class RecordTimeline {
    /// 允许开始的主机时间（倒计时结束）；在它之前的样本全部丢弃。
    private(set) var armTime: Double?
    /// 录制时间 0 对应的主机时间：录屏 = 允许开始后的第一帧画面；录音 = 开始时刻。
    private(set) var t0: Double?
    private var pauses: [(Double, Double)] = []
    private(set) var pauseStart: Double?
    private(set) var end: Double?

    func arm(at h: Double) { if armTime == nil { armTime = h } }

    /// 没有画面的录制（录音）：直接以 h 为零点。
    func startNow(at h: Double) {
        armTime = h
        t0 = h
    }

    /// 画面帧：允许开始后的第一帧确定零点。返回录制时间，nil 表示丢弃。
    func videoTime(_ h: Double) -> Double? {
        if t0 == nil {
            guard let a = armTime, h >= a, end == nil, pauseStart == nil else { return nil }
            t0 = h
        }
        return map(h)
    }

    /// 任意样本的录制时间；在零点之前、暂停中、停止之后返回 nil。
    func map(_ h: Double) -> Double? {
        guard let t0 = t0, h >= t0 else { return nil }
        if let e = end, h > e { return nil }
        if let ps = pauseStart, h >= ps { return nil }
        var off = 0.0
        for (s, e) in pauses {
            let s2 = max(s, t0)
            guard e > s2 else { continue }
            if h >= e { off += e - s2 } else if h >= s2 { return nil }
        }
        return h - t0 - off
    }

    func pause(at h: Double) { if pauseStart == nil && end == nil { pauseStart = h } }

    func resume(at h: Double) {
        guard let s = pauseStart else { return }
        pauses.append((s, max(s, h)))
        pauseStart = nil
    }

    func stop(at h: Double) {
        guard end == nil else { return }
        if let ps = pauseStart { resume(at: max(ps, h)) }
        end = h
    }

    /// 到主机时间 h 为止已录制的时长（暂停中停在暂停开始处）。
    func elapsed(at h: Double) -> Double {
        guard let t0 = t0 else { return 0 }
        var hh = h
        if let e = end { hh = min(hh, e) }
        if let ps = pauseStart { hh = min(hh, ps) }
        var off = 0.0
        for (s, e) in pauses {
            let s2 = max(s, t0)
            let e2 = min(e, hh)
            if e2 > s2 { off += e2 - s2 }
        }
        return max(0, hh - t0 - off)
    }

    var endElapsed: Double { elapsed(at: end ?? Double.greatestFiniteMagnitude) }
}

// MARK: - 混音

/// 两路音频各自重采样到 48k 立体声浮点，按录制时间对齐后相加。
final class AudioMixer {
    static let rate: Double = 48000
    /// 某一路落后超过这么多帧时不再等它，缺的部分当静音。
    static let latency: Int64 = 48000 * 3 / 10
    /// 时间戳与预期位置相差在这个范围内视为连续（避免抖动造成咔嗒声），超过则重新对齐。
    static let resyncThreshold: Int64 = 2400

    final class Source {
        var converter: AVAudioConverter?
        var inFormat: AVAudioFormat?
        var nextPos: Int64 = -1
        /// 交错立体声，第 0 帧对应 mixer.basePos。
        var pending: [Float] = []
        var filledTo: Int64 = 0
        var delivered = false
    }

    private(set) var sources: [Source]
    /// 下一次输出的起始帧位置。
    private(set) var basePos: Int64 = 0

    init(sourceCount: Int) {
        sources = (0..<max(1, sourceCount)).map { _ in Source() }
    }

    /// 把一个采集到的样本缓冲转成 48k 交错立体声浮点。
    func convert(_ index: Int, _ sb: CMSampleBuffer) -> [Float]? {
        guard index < sources.count, let fd = CMSampleBufferGetFormatDescription(sb) else { return nil }
        let n = CMSampleBufferGetNumSamples(sb)
        guard n > 0 else { return nil }
        let inFmt = AVAudioFormat(cmAudioFormatDescription: fd)
        guard inFmt.channelCount > 0, inFmt.sampleRate > 0,
              let inBuf = AVAudioPCMBuffer(pcmFormat: inFmt, frameCapacity: AVAudioFrameCount(n)) else { return nil }
        inBuf.frameLength = AVAudioFrameCount(n)
        guard CMSampleBufferCopyPCMDataIntoAudioBufferList(sb, at: 0, frameCount: Int32(n), into: inBuf.mutableAudioBufferList) == noErr else { return nil }
        return convert(index, inBuf)
    }

    /// 任意 PCM 缓冲 → 48k 交错立体声浮点。单声道复制到左右，多于两声道取前两个。
    func convert(_ index: Int, _ inBuf: AVAudioPCMBuffer) -> [Float]? {
        guard index < sources.count else { return nil }
        let src = sources[index]
        let inFmt = inBuf.format
        let ch = inFmt.channelCount
        var buf = inBuf
        let direct = inFmt.commonFormat == .pcmFormatFloat32 && inFmt.sampleRate == AudioMixer.rate && !inFmt.isInterleaved
        if !direct {
            let outFmt: AVAudioFormat?
            if ch <= 2 {
                outFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: AudioMixer.rate, channels: ch, interleaved: false)
            } else {
                let layout = AVAudioChannelLayout(layoutTag: kAudioChannelLayoutTag_DiscreteInOrder | AudioChannelLayoutTag(ch))
                outFmt = layout.map { AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: AudioMixer.rate, interleaved: false, channelLayout: $0) }
            }
            guard let of = outFmt else { return nil }
            if src.converter == nil || src.inFormat != inFmt {
                src.converter = AVAudioConverter(from: inFmt, to: of)
                src.inFormat = inFmt
            }
            guard let conv = src.converter else { return nil }
            let cap = AVAudioFrameCount(Double(inBuf.frameLength) * AudioMixer.rate / inFmt.sampleRate) + 512
            guard let out = AVAudioPCMBuffer(pcmFormat: conv.outputFormat, frameCapacity: cap) else { return nil }
            var given = false
            var err: NSError?
            let st = conv.convert(to: out, error: &err) { _, status in
                if given {
                    status.pointee = .noDataNow
                    return nil
                }
                given = true
                status.pointee = .haveData
                return inBuf
            }
            if st == .error { return nil }
            buf = out
        }
        let frames = Int(buf.frameLength)
        guard frames > 0, let data = buf.floatChannelData else { return frames == 0 ? [] : nil }
        let l = data[0]
        let r = buf.format.channelCount > 1 ? data[1] : data[0]
        var out = [Float](repeating: 0, count: frames * 2)
        out.withUnsafeMutableBufferPointer { o in
            for i in 0..<frames {
                o[2 * i] = l[i]
                o[2 * i + 1] = r[i]
            }
        }
        return out
    }

    /// 放到录制时间 t（秒）处。frames 为 48k 交错立体声。
    func push(_ index: Int, frames: [Float], at t: Double) {
        guard index < sources.count else { return }
        let s = sources[index]
        let n = Int64(frames.count / 2)
        guard n > 0 else { return }
        var p = Int64((t * AudioMixer.rate).rounded())
        if s.nextPos >= 0 && abs(p - s.nextPos) < AudioMixer.resyncThreshold { p = s.nextPos }
        s.nextPos = p + n
        s.delivered = true
        let skip = max(0, basePos - p)
        guard skip < n else { return }
        let start = Int(p + skip - basePos) * 2
        let count = Int(n - skip) * 2
        let need = start + count
        if s.pending.count < need { s.pending.append(contentsOf: repeatElement(0, count: need - s.pending.count)) }
        frames.withUnsafeBufferPointer { f in
            s.pending.withUnsafeMutableBufferPointer { d in
                let off = Int(skip) * 2
                for k in 0..<count { d[start + k] = f[off + k] }
            }
        }
        s.filledTo = max(s.filledTo, p + n)
    }

    /// 各路都已到达的位置（没送过数据的不算）。
    var readyPosition: Int64 {
        let ready = sources.filter { $0.delivered }.map { $0.filledTo }
        return ready.min() ?? basePos
    }

    /// 取出 [basePos, upTo) 的混音（相加后限幅）。
    func pull(upTo: Int64) -> [Float] {
        let n = Int(upTo - basePos)
        guard n > 0 else { return [] }
        var out = [Float](repeating: 0, count: n * 2)
        for s in sources {
            let m = min(s.pending.count, n * 2)
            if m > 0 {
                out.withUnsafeMutableBufferPointer { o in
                    s.pending.withUnsafeBufferPointer { p in
                        for k in 0..<m { o[k] += p[k] }
                    }
                }
                s.pending.removeFirst(m)
            }
        }
        out.withUnsafeMutableBufferPointer { o in
            for k in 0..<o.count { o[k] = max(-1, min(1, o[k])) }
        }
        basePos = upTo
        return out
    }
}

// MARK: - MP4 / 音频文件

/// AVAssetWriter：H.264 视频 + AAC（或 WAV 的 LPCM）音频，会话从 0 开始。
final class MovieWriter {
    let writer: AVAssetWriter
    private let video: AVAssetWriterInput?
    private let adaptor: AVAssetWriterInputPixelBufferAdaptor?
    private let audio: AVAssetWriterInput?
    private let keepAll: Bool
    private var lastPixelBuffer: CVPixelBuffer?
    private var pendingAudio: [CMSampleBuffer] = []
    private(set) var videoFrames = 0
    private(set) var lastVideoTime = -1.0
    var hasVideo: Bool { video != nil }

    struct VideoSpec { var width: Int; var height: Int; var fps: Int; var bitrate: Int }

    static let timescale: CMTimeScale = 6000

    enum AudioKind { case aac, wav }

    /// keepAll：编码器忙时不丢样本（自测用；实时录制时为假，忙时丢最旧的画面）。
    init(url: URL, fileType: AVFileType, video spec: VideoSpec?, audio kind: AudioKind?, keepAll: Bool = false) throws {
        try? FileManager.default.removeItem(at: url)
        writer = try AVAssetWriter(outputURL: url, fileType: fileType)
        self.keepAll = keepAll
        let realTime = true
        if let v = spec {
            let props: [String: Any] = [
                AVVideoAverageBitRateKey: v.bitrate,
                AVVideoExpectedSourceFrameRateKey: v.fps,
                AVVideoMaxKeyFrameIntervalKey: max(1, v.fps * 2),
                AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
                AVVideoAllowFrameReorderingKey: false,
            ]
            let settings: [String: Any] = [
                AVVideoCodecKey: AVVideoCodecType.h264,
                AVVideoWidthKey: v.width,
                AVVideoHeightKey: v.height,
                AVVideoCompressionPropertiesKey: props,
                AVVideoColorPropertiesKey: [
                    AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                    AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                    AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2,
                ],
            ]
            let input = AVAssetWriterInput(mediaType: .video, outputSettings: settings)
            input.expectsMediaDataInRealTime = realTime
            input.mediaTimeScale = MovieWriter.timescale
            guard writer.canAdd(input) else { throw RecordError.encoder("video") }
            writer.add(input)
            video = input
            adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: nil)
        } else {
            video = nil
            adaptor = nil
        }
        if let k = kind {
            var settings: [String: Any] = [AVSampleRateKey: AudioMixer.rate, AVNumberOfChannelsKey: 2]
            switch k {
            case .aac:
                settings[AVFormatIDKey] = kAudioFormatMPEG4AAC
                settings[AVEncoderBitRateKey] = 192_000
            case .wav:
                settings[AVFormatIDKey] = kAudioFormatLinearPCM
                settings[AVLinearPCMBitDepthKey] = 16
                settings[AVLinearPCMIsFloatKey] = false
                settings[AVLinearPCMIsBigEndianKey] = false
                settings[AVLinearPCMIsNonInterleaved] = false
            }
            let input = AVAssetWriterInput(mediaType: .audio, outputSettings: settings)
            input.expectsMediaDataInRealTime = realTime
            guard writer.canAdd(input) else { throw RecordError.encoder("audio") }
            writer.add(input)
            audio = input
        } else {
            audio = nil
        }
        guard writer.startWriting() else { throw writer.error ?? RecordError.encoder("start") }
        writer.startSession(atSourceTime: .zero)
    }

    /// 两路各自排队、交替送进编码器：AVAssetWriter 要求音视频交错，任何一路都不能阻塞等待另一路。
    /// 录制时队列很短（编码器跟不上就丢最旧的画面）；自测（keepAll）不丢，收尾时等编码器收完。
    private var pendingVideo: [(CVPixelBuffer, CMTime)] = []
    private var lastQueuedVideo = CMTime.invalid
    private var maxPendingVideo: Int { keepAll ? Int.max : 4 }

    private func flush() {
        guard writer.status == .writing else {
            pendingVideo.removeAll()
            pendingAudio.removeAll()
            return
        }
        var progressed = true
        while progressed {
            progressed = false
            if let input = video, let ad = adaptor, let f = pendingVideo.first, input.isReadyForMoreMediaData {
                pendingVideo.removeFirst()
                if ad.append(f.0, withPresentationTime: f.1) { videoFrames += 1 }
                progressed = true
            }
            if let input = audio, let sb = pendingAudio.first, input.isReadyForMoreMediaData {
                pendingAudio.removeFirst()
                _ = input.append(sb)
                progressed = true
            }
        }
        if pendingVideo.count > maxPendingVideo { pendingVideo.removeFirst(pendingVideo.count - maxPendingVideo) }
        // 编码器长时间跟不上时丢掉最旧的音频，避免无限堆积（实时约 4 秒）
        if !keepAll && pendingAudio.count > 400 { pendingAudio.removeFirst(pendingAudio.count - 400) }
    }

    func appendVideo(_ pb: CVPixelBuffer, time t: Double) {
        guard video != nil, writer.status == .writing else { return }
        let pts = CMTime(value: CMTimeValue((t * Double(MovieWriter.timescale)).rounded()), timescale: MovieWriter.timescale)
        // 时间戳必须严格递增
        if lastQueuedVideo.isValid && pts <= lastQueuedVideo { return }
        lastQueuedVideo = pts
        lastVideoTime = t
        lastPixelBuffer = pb
        queuedVideoFrames += 1
        pendingVideo.append((pb, pts))
        flush()
    }

    /// 已接收（排队或已送编码器）的画面帧数。
    private(set) var queuedVideoFrames = 0

    /// 画面长时间不变时（屏幕静止，采集不出新帧）重复上一帧，保持音视频交错写入。
    func repeatLastFrame(at t: Double) {
        guard let pb = lastPixelBuffer, t - lastVideoTime >= 1.0 else { return }
        appendVideo(pb, time: t)
    }

    static let lpcmFormat: CMAudioFormatDescription? = {
        var asbd = AudioStreamBasicDescription(mSampleRate: AudioMixer.rate, mFormatID: kAudioFormatLinearPCM,
                                               mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                               mBytesPerPacket: 8, mFramesPerPacket: 1, mBytesPerFrame: 8, mChannelsPerFrame: 2, mBitsPerChannel: 32, mReserved: 0)
        var fd: CMAudioFormatDescription?
        CMAudioFormatDescriptionCreate(allocator: nil, asbd: &asbd, layoutSize: 0, layout: nil, magicCookieSize: 0, magicCookie: nil, extensions: nil, formatDescriptionOut: &fd)
        return fd
    }()

    /// 48k 交错立体声浮点 → CMSampleBuffer，时间戳为帧位置 pos。
    static func audioBuffer(_ frames: [Float], pos: Int64) -> CMSampleBuffer? {
        guard let fd = lpcmFormat, !frames.isEmpty else { return nil }
        let n = frames.count / 2
        let bytes = n * 8
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: bytes, blockAllocator: nil, customBlockSource: nil,
                                                 offsetToData: 0, dataLength: bytes, flags: kCMBlockBufferAssureMemoryNowFlag, blockBufferOut: &block) == noErr,
              let bb = block else { return nil }
        let st = frames.withUnsafeBytes { raw -> OSStatus in
            guard let base = raw.baseAddress else { return -1 }
            return CMBlockBufferReplaceDataBytes(with: base, blockBuffer: bb, offsetIntoDestination: 0, dataLength: bytes)
        }
        guard st == noErr else { return nil }
        var sb: CMSampleBuffer?
        var timing = CMSampleTimingInfo(duration: CMTime(value: 1, timescale: CMTimeScale(AudioMixer.rate)),
                                        presentationTimeStamp: CMTime(value: pos, timescale: CMTimeScale(AudioMixer.rate)), decodeTimeStamp: .invalid)
        var size = 8
        guard CMSampleBufferCreate(allocator: nil, dataBuffer: bb, dataReady: true, makeDataReadyCallback: nil, refcon: nil, formatDescription: fd,
                                   sampleCount: n, sampleTimingEntryCount: 1, sampleTimingArray: &timing, sampleSizeEntryCount: 1, sampleSizeArray: &size,
                                   sampleBufferOut: &sb) == noErr else { return nil }
        return sb
    }

    func appendAudio(_ frames: [Float], pos: Int64) {
        guard audio != nil, let sb = MovieWriter.audioBuffer(frames, pos: pos) else { return }
        pendingAudio.append(sb)
        flush()
    }

    /// 结束：会话结束时间为 end（录制时长）。没写进任何画面的录屏视为失败。
    func finish(end: Double, completion: @escaping (Bool) -> Void) {
        // 排队中的样本等编码器收完（最多约 3 秒）
        for _ in 0..<1500 where !(pendingVideo.isEmpty && pendingAudio.isEmpty) && writer.status == .writing {
            flush()
            if !(pendingVideo.isEmpty && pendingAudio.isEmpty) { usleep(2000) }
        }
        pendingVideo.removeAll()
        pendingAudio.removeAll()
        guard writer.status == .writing, video == nil || videoFrames > 0 else {
            writer.cancelWriting()
            completion(false)
            return
        }
        audio?.markAsFinished()
        video?.markAsFinished()
        writer.endSession(atSourceTime: CMTime(value: CMTimeValue((max(0, end) * AudioMixer.rate).rounded()), timescale: CMTimeScale(AudioMixer.rate)))
        let w = writer
        w.finishWriting { completion(w.status == .completed) }
    }

    func cancel() {
        if writer.status == .writing { writer.cancelWriting() }
        try? FileManager.default.removeItem(at: writer.outputURL)
    }
}

enum RecordError: LocalizedError {
    case encoder(String)
    case noDisplay
    case noFrames
    case capture(String)

    var errorDescription: String? {
        switch self {
        case .encoder(let s): return "encoder \(s)"
        case .noDisplay: return "display"
        case .noFrames: return "no frames"
        case .capture(let s): return s
        }
    }
}

// MARK: - GIF

/// GIF 录制：按 gifFps 抓到的帧原样（BGRA）追加进临时文件，停止后后台用内核 ls_gif_* 编码。
final class GifFrameStore {
    let width: Int
    let height: Int
    let url: URL
    private var handle: FileHandle?
    private(set) var times: [Double] = []
    private var row: [UInt8]

    init?(width: Int, height: Int) {
        self.width = width
        self.height = height
        url = FileManager.default.temporaryDirectory.appendingPathComponent("lemonshot-gif-\(UUID().uuidString).raw")
        row = [UInt8](repeating: 0, count: width * 4)
        guard FileManager.default.createFile(atPath: url.path, contents: nil), let h = try? FileHandle(forWritingTo: url) else { return nil }
        handle = h
    }

    var frameCount: Int { times.count }
    var frameBytes: Int { width * height * 4 }

    /// 追加一帧（BGRA）。尺寸不同时按左上角对齐裁剪/补黑。
    func append(_ pb: CVPixelBuffer, time t: Double) {
        guard let h = handle, CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_32BGRA else { return }
        if let last = times.last, t <= last { return }
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb) else { return }
        append(base: base, stride: CVPixelBufferGetBytesPerRow(pb), width: CVPixelBufferGetWidth(pb), height: CVPixelBufferGetHeight(pb), handle: h)
        times.append(t)
    }

    private func append(base: UnsafeMutableRawPointer, stride: Int, width w: Int, height hh: Int, handle h: FileHandle) {
        let copyW = min(w, width) * 4
        var data = Data(capacity: frameBytes)
        for y in 0..<height {
            if y < hh {
                let src = base.advanced(by: y * stride)
                row.withUnsafeMutableBytes { r in
                    r.copyMemory(from: UnsafeRawBufferPointer(start: src, count: copyW))
                    if copyW < width * 4 { memset(r.baseAddress!.advanced(by: copyW), 0, width * 4 - copyW) }
                }
            } else {
                for i in 0..<row.count { row[i] = i % 4 == 3 ? 255 : 0 }
            }
            data.append(contentsOf: row)
        }
        h.write(data)
    }

    func close() {
        handle?.closeFile()
        handle = nil
    }

    func discard() {
        close()
        try? FileManager.default.removeItem(at: url)
    }

    /// 编码成 GIF（调用方在后台线程调用）。每帧时长 = 到下一帧的录制时间差（百分之一秒，累计取整不漂移，最少 2）。
    func encode(to out: URL, end: Double, progress: (Double) -> Void) -> Bool {
        close()
        guard !times.isEmpty, let rh = FileHandle(forReadingAtPath: url.path) else { return false }
        defer { rh.closeFile() }
        try? FileManager.default.removeItem(at: out)
        guard let gif = out.path.withCString({ ls_gif_create($0, Int32(width), Int32(height), 0) }) else { return false }
        defer { ls_gif_destroy(gif) }
        var emittedCs = Int((times[0] * 100).rounded())
        for i in 0..<times.count {
            let data = rh.readData(ofLength: frameBytes)
            guard data.count == frameBytes else { break }
            // 最后一帧显示到结束时刻
            let next = i + 1 < times.count ? times[i + 1] : (end > times[i] + 0.02 ? end : times[i] + 0.1)
            let target = Int((next * 100).rounded())
            let delay = max(2, target - emittedCs)
            emittedCs += delay
            let ok = data.withUnsafeBytes { raw -> Bool in
                guard let p = raw.baseAddress else { return false }
                var img = ls_image(data: UnsafeMutablePointer(mutating: p.assumingMemoryBound(to: UInt8.self)), width: Int32(width), height: Int32(height), stride: Int32(width * 4))
                return ls_gif_add(gif, &img, Int32(delay)) == 0
            }
            if !ok { return false }
            progress(Double(i + 1) / Double(times.count))
        }
        return ls_gif_finish(gif) == 0
    }
}

// MARK: - 通路

/// 把画面帧、各路音频按时间线写进文件。所有方法都在 queue 上调用。
final class RecordPipeline {
    let queue: DispatchQueue
    let timeline = RecordTimeline()
    let mixer: AudioMixer?
    let movie: MovieWriter?
    let gif: GifFrameStore?
    /// 当前时间（主机秒）。自测时换成模拟时钟。
    var now: () -> Double = HostClock.now
    /// 混音输出电平（0–1 峰值），在 queue 上回调。
    var onLevel: ((Float) -> Void)?
    private(set) var finished = false
    private(set) var audioFramesWritten: Int64 = 0

    init(queue: DispatchQueue, movie: MovieWriter?, gif: GifFrameStore?, audioSources: Int) {
        self.queue = queue
        self.movie = movie
        self.gif = gif
        mixer = audioSources > 0 ? AudioMixer(sourceCount: audioSources) : nil
    }

    /// 倒计时期间收到的最后一帧：屏幕静止时采集不出新帧，开始时用它作为第 0 帧。
    private var preArmFrame: CVPixelBuffer?

    func video(_ pb: CVPixelBuffer, host h: Double) {
        guard !finished else { return }
        if timeline.armTime == nil {
            preArmFrame = pb
            return
        }
        guard let t = timeline.videoTime(h) else { return }
        preArmFrame = nil
        write(pb, at: t)
    }

    private func write(_ pb: CVPixelBuffer, at t: Double) {
        if let m = movie, m.hasVideo {
            m.appendVideo(pb, time: t)
        } else if let g = gif {
            g.append(pb, time: t)
        }
        drainAudio()
    }

    /// 倒计时结束：从 h 开始录。手里有倒计时期间的画面就以它为第 0 帧，不必等屏幕变化。
    func arm(at h: Double) {
        timeline.arm(at: h)
        if let pb = preArmFrame, let t = timeline.videoTime(h) { write(pb, at: t) }
        preArmFrame = nil
    }

    func audio(_ index: Int, _ sb: CMSampleBuffer, host h: Double) {
        guard !finished, let mixer = mixer, let t = timeline.map(h), let frames = mixer.convert(index, sb) else { return }
        mixer.push(index, frames: frames, at: t)
        drainAudio()
    }

    /// 已经是 PCM 缓冲的音频（自测用）。
    func audio(_ index: Int, _ buf: AVAudioPCMBuffer, host h: Double) {
        guard !finished, let mixer = mixer, let t = timeline.map(h), let frames = mixer.convert(index, buf) else { return }
        mixer.push(index, frames: frames, at: t)
        drainAudio()
    }

    /// 输出各路都已到达（或落后超过 latency）的混音。
    func drainAudio(final: Bool = false) {
        guard let mixer = mixer, timeline.t0 != nil else { return }
        let nowT = final ? timeline.endElapsed : timeline.elapsed(at: now())
        let nowPos = Int64((nowT * AudioMixer.rate).rounded())
        var upTo = max(mixer.readyPosition, nowPos - AudioMixer.latency)
        upTo = final ? nowPos : min(upTo, nowPos)
        // 停止之后不再输出超过结束时刻的声音（wav 没有编辑表，多写的会留在文件里）
        if timeline.end != nil { upTo = min(upTo, Int64((timeline.endElapsed * AudioMixer.rate).rounded())) }
        if upTo > mixer.basePos {
            let pos = mixer.basePos
            let frames = mixer.pull(upTo: upTo)
            movie?.appendAudio(frames, pos: pos)
            audioFramesWritten += Int64(frames.count / 2)
            if let cb = onLevel {
                var peak: Float = 0
                for v in frames { peak = max(peak, abs(v)) }
                cb(peak)
            }
        }
        if !final, timeline.end == nil, let m = movie, m.hasVideo, m.queuedVideoFrames > 0 {
            m.repeatLastFrame(at: max(0, nowT - 0.2))
        }
    }

    /// 停止后调用（timeline.stop 已设置）：写完剩余音频并收尾。completion 在任意线程。
    func finish(completion: @escaping (Bool, Double) -> Void) {
        guard !finished else { return }
        if timeline.end == nil { timeline.stop(at: now()) }
        let end = timeline.endElapsed
        drainAudio(final: true)
        finished = true
        if let m = movie {
            m.finish(end: end) { ok in completion(ok, end) }
        } else if let g = gif {
            g.close()
            completion(g.frameCount > 0, end)
        } else {
            completion(false, end)
        }
    }

    func cancel() {
        finished = true
        movie?.cancel()
        gif?.discard()
    }
}
