import AppKit
import AVFoundation
import LemonShotCore

/// 自测（spec 1.1）：录屏设置面板、控制条、按键浮层、录音面板四个状态；外加编码链路的离屏检查
/// （程序生成的画面 + 合成正弦波，走与录制相同的时间线/混音/写文件代码，读回校验）。不需要任何权限、不碰屏幕和声音设备。
enum RecordSelfTest {
    static func render(_ screen: FrozenImage, write: (String, CGImage?) -> Void) {
        write("record-setup", recordSetup(screen))
        write("record-bar", recordBar(screen))
        write("record-keys", recordKeys(screen))
        write("audio", audioPanel(screen))
    }

    static var scale: CGFloat { SelfTest.scale }

    // MARK: 录屏设置面板

    /// 录屏模式截图层：选区 (200,150,640,400)，下方录屏设置面板，左下角提示。
    static func recordSetup(_ screen: FrozenImage) -> CGImage? {
        guard let s = SelfTest.makeSession(screen) else { return nil }
        RecordUI.configure(s)
        let mouse = CGPoint(x: 520, y: 360)
        _ = s.mouseMove(CGPoint(x: 400, y: 300), mods: 0)
        _ = s.setSelection(SelfTest.sampleSelection)
        _ = s.mouseMove(mouse, mods: 0)
        let bounds = CGRect(origin: .zero, size: SelfTest.screenSize)
        let st = s.state
        let panel = RecordSetupPanel()
        let l = CaptureLayout.compute(session: s, state: st, mouse: mouse, mouseInView: true, mode: .record, flags: CaptureLayout.Flags(),
                                      bounds: bounds, toolbarSize: panel.frame.size)
        if let r = l.toolbar {
            panel.frame = r
            panel.layout()
        }
        return Offscreen.render(size: SelfTest.screenSize, scale: scale) { ctx in
            ctx.interpolationQuality = .none
            Draw.image(ctx, s.frozen.cgImage, in: bounds)
            ctx.interpolationQuality = .high
            CaptureRender.drawMain(ctx, session: s, state: st, bounds: bounds, dirty: bounds)
            if let p = l.crosshair { CaptureRender.drawCrosshair(ctx, at: p, bounds: bounds) }
            if let r = l.sizeTip { CaptureRender.drawSizeTip(ctx, r, l.sizeTipText) }
            if let r = l.tips { CaptureRender.drawTips(ctx, r, l.tipsLines, color: Theme.hex(Settings.shared.string("capture.tipsColor"))) }
            if l.toolbar != nil { panel.draw(ctx) }
        }
    }

    // MARK: 控制条

    /// 录制中控制条（计时 00:12），连同区域下边缘的红色虚线边框。
    static func recordBar(_ screen: FrozenImage) -> CGImage? {
        let w = RecordBarWidget()
        w.clock = "00:12"
        let size = w.contentSize()
        let pad: CGFloat = 24
        let canvas = CGSize(width: size.width + 2 * pad, height: size.height + 2 * pad + 20)
        w.frame = CGRect(x: pad, y: pad + 20, width: size.width, height: size.height)
        w.layout()
        let bg = SelfTest.crop(of: screen, CGRect(x: 300, y: 330, width: canvas.width, height: canvas.height))
        return Offscreen.render(size: canvas, scale: scale) { ctx in
            if let bg = bg { Draw.image(ctx, bg, in: CGRect(origin: .zero, size: canvas)) }
            // 区域下边缘与边框（示意控制条在区域下方）
            let bw = Theme.f("recordBar.regionBorderWidth")
            ctx.saveGState()
            ctx.setStrokeColor(Theme.tokenColor("recordBar.regionBorder"))
            ctx.setLineWidth(bw)
            ctx.setLineDash(phase: 0, lengths: [6, 4])
            ctx.move(to: CGPoint(x: 0, y: 12 + bw / 2))
            ctx.addLine(to: CGPoint(x: canvas.width, y: 12 + bw / 2))
            ctx.strokePath()
            ctx.restoreGState()
            w.draw(ctx)
        }
    }

    // MARK: 按键浮层

    /// 按键浮层：⌘ ⇧ 4（真实按键映射：键码 21 + Command + Shift），底部居中、中号、外观跟随主题。
    static func recordKeys(_ screen: FrozenImage) -> CGImage? {
        let size = CGSize(width: 520, height: 200)
        let w = KeysOverlayWidget()
        w.position = "bottomCenter"
        w.sizeName = "medium"
        w.dark = Theme.isDark
        w.frame = CGRect(origin: .zero, size: size)
        let c = KeysOverlayWidget.combo(keyCode: 21, flags: [.command, .shift], characters: "4")
        w.items = [KeysOverlayWidget.Item(combo: c, shownAt: 0, alpha: 1)]
        let bg = SelfTest.crop(of: screen, CGRect(x: 260, y: 200, width: size.width, height: size.height))
        return Offscreen.render(size: size, scale: scale) { ctx in
            if let bg = bg { Draw.image(ctx, bg, in: CGRect(origin: .zero, size: size)) }
            w.draw(ctx)
        }
    }

    // MARK: 录音面板

    /// 录音面板（录音中：计时 00:07、电平约 60%）。
    static func audioPanel(_ screen: FrozenImage) -> CGImage? {
        let w = AudioPanelWidget()
        let pad: CGFloat = 24
        let size = CGSize(width: AudioPanelWidget.size.width + 2 * pad, height: AudioPanelWidget.size.height + 2 * pad)
        w.frame = CGRect(x: pad, y: pad, width: AudioPanelWidget.size.width, height: AudioPanelWidget.size.height)
        w.mic.isOn = Settings.shared.bool("audio.microphone")
        w.system.isOn = true
        w.recording = true
        w.clock = "00:07"
        w.level = AudioPanelWidget.meter(0.18)
        w.layout()
        let bg = SelfTest.crop(of: screen, CGRect(x: 840, y: 60, width: size.width, height: size.height))
        return Offscreen.render(size: size, scale: scale) { ctx in
            if let bg = bg { Draw.image(ctx, bg, in: CGRect(origin: .zero, size: size)) }
            w.draw(ctx)
        }
    }

    // MARK: - 编码链路离屏检查

    /// 写 <dir>/record-check/ 下的 check.mp4、check.gif、check.m4a、check.wav 与 record-check.txt。返回是否全部通过。
    @discardableResult
    static func encodeCheck(dir: URL) -> Bool {
        let out = dir.appendingPathComponent("record-check", isDirectory: true)
        try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
        var lines: [String] = []
        var allOK = true
        func check(_ name: String, _ ok: Bool, _ detail: String) {
            lines.append("[\(ok ? "通过" : "失败")] \(name)：\(detail)")
            if !ok { allOK = false }
        }
        func timed(_ name: String, _ f: () -> Void) {
            let t = Date()
            f()
            lines.append("　　\(name) 用时 \(fmt(Date().timeIntervalSince(t))) 秒")
        }
        timed("MP4") { mp4Check(out.appendingPathComponent("check.mp4"), check) }
        timed("GIF") { gifCheck(out.appendingPathComponent("check.gif"), check) }
        timed("M4A") { audioFileCheck(out.appendingPathComponent("check.m4a"), kind: .aac, check) }
        timed("WAV") { audioFileCheck(out.appendingPathComponent("check.wav"), kind: .wav, check) }
        let text = lines.joined(separator: "\n") + "\n"
        try? text.write(to: out.appendingPathComponent("record-check.txt"), atomically: true, encoding: .utf8)
        print("selftest: 录屏编码检查\(allOK ? "全部通过" : "有失败项")，见 \(out.appendingPathComponent("record-check.txt").path)")
        return allOK
    }

    /// 模拟时钟：主机时间从 1000 秒开始。
    final class SimClock { var now = 1000.0 }

    /// 测试画面：随时间移动的色块 + 背景色渐变（BGRA）。
    static func frame(_ w: Int, _ h: Int, t: Double) -> CVPixelBuffer? {
        var pb: CVPixelBuffer?
        let attrs: [String: Any] = [kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any]()]
        guard CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_32BGRA, attrs as CFDictionary, &pb) == kCVReturnSuccess, let p = pb else { return nil }
        CVPixelBufferLockBaseAddress(p, [])
        defer { CVPixelBufferUnlockBaseAddress(p, []) }
        guard let ctx = CGContext(data: CVPixelBufferGetBaseAddress(p), width: w, height: h, bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(p),
                                  space: Theme.sRGB, bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        let v = CGFloat((sin(t * 2) + 1) / 2)
        ctx.setFillColor(CGColor(srgbRed: 0.2 + 0.5 * v, green: 0.4, blue: 0.9 - 0.5 * v, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.setFillColor(CGColor(srgbRed: 1, green: 0.8, blue: 0, alpha: 1))
        let bx = CGFloat(t.truncatingRemainder(dividingBy: 3) / 3) * CGFloat(w - h / 3)
        ctx.fill(CGRect(x: bx, y: CGFloat(h) / 3, width: CGFloat(h) / 3, height: CGFloat(h) / 3))
        return p
    }

    /// 正弦波 PCM 缓冲。interleaved 为假时是非交错（与 SCStream 音频相同）。
    static func sine(rate: Double, channels: AVAudioChannelCount, interleaved: Bool, freq: Double, amp: Float, startFrame: Int, frames: Int) -> AVAudioPCMBuffer? {
        guard let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: rate, channels: channels, interleaved: interleaved),
              let buf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: AVAudioFrameCount(frames)) else { return nil }
        buf.frameLength = AVAudioFrameCount(frames)
        let abl = UnsafeMutableAudioBufferListPointer(buf.mutableAudioBufferList)
        for i in 0..<frames {
            let v = amp * Float(sin(2 * Double.pi * freq * Double(startFrame + i) / rate))
            if interleaved {
                let p = abl[0].mData!.assumingMemoryBound(to: Float.self)
                for c in 0..<Int(channels) { p[i * Int(channels) + c] = v }
            } else {
                for c in 0..<Int(channels) { abl[c].mData!.assumingMemoryBound(to: Float.self)[i] = v }
            }
        }
        return buf
    }

    /// 与采集回调同样形态的 CMSampleBuffer。
    static func sampleBuffer(_ pcm: AVAudioPCMBuffer, pts: CMTime) -> CMSampleBuffer? {
        var sb: CMSampleBuffer?
        guard CMAudioSampleBufferCreateWithPacketDescriptions(allocator: nil, dataBuffer: nil, dataReady: false, makeDataReadyCallback: nil, refcon: nil,
                                                              formatDescription: pcm.format.formatDescription, sampleCount: CMItemCount(pcm.frameLength),
                                                              presentationTimeStamp: pts, packetDescriptions: nil, sampleBufferOut: &sb) == noErr,
              let s = sb,
              CMSampleBufferSetDataBufferFromAudioBufferList(s, blockBufferAllocator: nil, blockBufferMemoryAllocator: nil, flags: 0, bufferList: pcm.audioBufferList) == noErr
        else { return nil }
        return s
    }

    private enum Event { case video(Double), mic(Int), sys(Int), pause, resume }

    /// 4 秒模拟录制，1.49–2.49 秒暂停：期望 3.0 秒、90 帧、首帧时间 0、两路声音（440/660 Hz）都在。
    static func mp4Check(_ url: URL, _ check: (String, Bool, String) -> Void) {
        let clock = SimClock()
        let q = DispatchQueue(label: "selftest.record")
        let w = 640, h = 360, fps = 30
        let movie: MovieWriter
        do {
            movie = try MovieWriter(url: url, fileType: .mp4, video: .init(width: w, height: h, fps: fps, bitrate: RecordConfig.bitrate(width: w, height: h, fps: fps)), audio: .aac, keepAll: true)
        } catch {
            check("MP4 写入器", false, error.localizedDescription)
            return
        }
        let p = RecordPipeline(queue: q, movie: movie, gif: nil, audioSources: 2)
        p.now = { clock.now }
        let h0 = 1000.0
        // 倒计时期间（开始前）送来的一帧：开始时作为第 0 帧（屏幕静止也能立即开始）
        clock.now = h0 - 0.5
        if let pb = frame(w, h, t: 0) { p.video(pb, host: h0 - 0.5) }
        p.arm(at: h0)
        // 事件按主机时间排序：画面 30 帧/秒（第 0 帧由上面那帧代替）；麦克风 44.1k 单声道每块 1024；系统声音 48k 立体声非交错每块 960（从 h0 前 0.1 秒开始，验证零点前丢弃）
        var events: [(Double, Event)] = []
        for i in 1..<(4 * fps) { events.append((h0 + Double(i) / Double(fps), .video(h0 + Double(i) / Double(fps)))) }
        var n = 0
        while true {
            let t = h0 - 0.1 + Double(n * 1024) / 44100
            if t > h0 + 4.05 { break }
            events.append((t, .mic(n)))
            n += 1
        }
        n = 0
        while true {
            let t = h0 - 0.1 + Double(n * 960) / 48000
            if t > h0 + 4.05 { break }
            events.append((t, .sys(n)))
            n += 1
        }
        events.append((h0 + 1.49, .pause))
        events.append((h0 + 2.49, .resume))
        events.sort { $0.0 < $1.0 }
        for (t, e) in events {
            // 样本送达比采集时刻晚一点（模拟采集延迟）
            clock.now = min(t + 0.02, h0 + 4.0)
            switch e {
            case .video(let ht):
                if let pb = frame(w, h, t: ht - h0) { p.video(pb, host: ht) }
            case .mic(let k):
                if let b = sine(rate: 44100, channels: 1, interleaved: true, freq: 440, amp: 0.3, startFrame: k * 1024, frames: 1024),
                   let sb = sampleBuffer(b, pts: CMTime(seconds: t, preferredTimescale: 44100)) { p.audio(0, sb, host: t) }
            case .sys(let k):
                if let b = sine(rate: 48000, channels: 2, interleaved: false, freq: 660, amp: 0.3, startFrame: k * 960, frames: 960),
                   let sb = sampleBuffer(b, pts: CMTime(seconds: t, preferredTimescale: 48000)) { p.audio(1, sb, host: t) }
            case .pause: p.timeline.pause(at: t)
            case .resume: p.timeline.resume(at: t)
            }
        }
        p.timeline.stop(at: h0 + 4.0)
        clock.now = h0 + 4.05
        let sem = DispatchSemaphore(value: 0)
        var ok = false, end = 0.0
        p.finish { o, e in
            ok = o
            end = e
            sem.signal()
        }
        sem.wait()
        check("MP4 写文件", ok, "录制时长 \(fmt(end)) 秒（期望 3.000），写入画面 \(movie.videoFrames) 帧，音频 \(p.audioFramesWritten) 帧")
        guard ok else { return }

        let asset = AVURLAsset(url: url)
        let dur = asset.duration.seconds
        check("MP4 时长", abs(dur - 3.0) < 0.05, "\(fmt(dur)) 秒")
        let vt = asset.tracks(withMediaType: .video)
        let at = asset.tracks(withMediaType: .audio)
        check("MP4 轨道", vt.count == 1 && at.count == 1, "视频 \(vt.count) 条，音频 \(at.count) 条")
        if let v = vt.first {
            let sub = CMFormatDescriptionGetMediaSubType(v.formatDescriptions.first as! CMFormatDescription)
            let codec = String(bytes: [24, 16, 8, 0].map { UInt8((sub >> $0) & 0xFF) }, encoding: .ascii) ?? "?"
            check("视频轨", Int(v.naturalSize.width) == w && Int(v.naturalSize.height) == h,
                  "\(Int(v.naturalSize.width))×\(Int(v.naturalSize.height))，编码 \(codec)，名义帧率 \(fmt(Double(v.nominalFrameRate)))，轨道时长 \(fmt(v.timeRange.duration.seconds)) 秒")
            if let reader = try? AVAssetReader(asset: asset) {
                let o = AVAssetReaderTrackOutput(track: v, outputSettings: nil)
                reader.add(o)
                reader.startReading()
                var pts: [Double] = []
                while let sb = o.copyNextSampleBuffer() {
                    if CMSampleBufferGetNumSamples(sb) > 0 { pts.append(CMSampleBufferGetPresentationTimeStamp(sb).seconds) }
                }
                pts.sort()
                var maxGap = 0.0
                for i in 1..<max(1, pts.count) { maxGap = max(maxGap, pts[i] - pts[i - 1]) }
                let first = pts.first ?? -1
                check("视频时间戳", pts.count == 90 && abs(first) < 0.001 && maxGap < 0.05,
                      "\(pts.count) 帧（期望 90），首帧 \(fmt(first))，末帧 \(fmt(pts.last ?? -1))，最大间隔 \(fmt(maxGap)) 秒（暂停已扣除）")
            }
        }
        if let a = at.first {
            let fd = a.formatDescriptions.first as! CMAudioFormatDescription
            let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fd)?.pointee
            let rate = asbd?.mSampleRate ?? 0, ch = asbd?.mChannelsPerFrame ?? 0
            check("音频轨", rate == 48000 && ch == 2 && asbd?.mFormatID == kAudioFormatMPEG4AAC,
                  "AAC \(Int(rate)) Hz \(ch) 声道，轨道时长 \(fmt(a.timeRange.duration.seconds)) 秒")
            if let pcm = decodeAudio(asset: asset, track: a) {
                func amp(_ from: Double, _ to: Double, _ f: Double) -> Double {
                    let s = Int(from * 48000), e = min(pcm.count, Int(to * 48000))
                    guard e > s else { return 0 }
                    return goertzel(pcm[s..<e], freq: f, rate: 48000)
                }
                let a1 = amp(0.3, 0.8, 440), b1 = amp(0.3, 0.8, 660)
                let a2 = amp(1.8, 2.3, 440), b2 = amp(1.8, 2.3, 660)
                let across = rms(pcm, 1.45, 1.55)
                let peak = pcm.reduce(Float(0)) { max($0, abs($1)) }
                let good = [a1, b1, a2, b2].allSatisfy { $0 > 0.2 && $0 < 0.4 } && across > 0.25 && peak <= 1.0
                check("混音", good, "暂停前 440Hz \(fmt(a1)) / 660Hz \(fmt(b1))，暂停后 440Hz \(fmt(a2)) / 660Hz \(fmt(b2))（各约 0.3），暂停接缝 1.45–1.55 秒 RMS \(fmt(across))，峰值 \(fmt(Double(peak)))")
            } else {
                check("混音", false, "读回音频失败")
            }
        }
    }

    static func decodeAudio(asset: AVAsset, track: AVAssetTrack) -> [Float]? {
        guard let reader = try? AVAssetReader(asset: asset) else { return nil }
        let settings: [String: Any] = [AVFormatIDKey: kAudioFormatLinearPCM, AVLinearPCMIsFloatKey: true, AVLinearPCMBitDepthKey: 32,
                                       AVLinearPCMIsNonInterleaved: false, AVLinearPCMIsBigEndianKey: false, AVSampleRateKey: 48000, AVNumberOfChannelsKey: 2]
        let o = AVAssetReaderTrackOutput(track: track, outputSettings: settings)
        reader.add(o)
        guard reader.startReading() else { return nil }
        var left: [Float] = []
        while let sb = o.copyNextSampleBuffer() {
            guard let bb = CMSampleBufferGetDataBuffer(sb) else { continue }
            var len = 0
            var ptr: UnsafeMutablePointer<CChar>?
            guard CMBlockBufferGetDataPointer(bb, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len, dataPointerOut: &ptr) == noErr, let p = ptr else { continue }
            let n = len / 8
            p.withMemoryRebound(to: Float.self, capacity: n * 2) { f in
                for i in 0..<n { left.append(f[2 * i]) }
            }
        }
        return left
    }

    static func goertzel(_ x: ArraySlice<Float>, freq: Double, rate: Double) -> Double {
        let k = 2 * cos(2 * Double.pi * freq / rate)
        var s1 = 0.0, s2 = 0.0
        for v in x {
            let s0 = Double(v) + k * s1 - s2
            s2 = s1
            s1 = s0
        }
        let power = max(0, s1 * s1 + s2 * s2 - k * s1 * s2)
        return 2 * sqrt(power) / Double(x.count)
    }

    static func rms(_ x: [Float], _ from: Double, _ to: Double) -> Double {
        let s = Int(from * 48000), e = min(x.count, Int(to * 48000))
        guard e > s else { return 0 }
        var sum = 0.0
        for i in s..<e { sum += Double(x[i]) * Double(x[i]) }
        return sqrt(sum / Double(e - s))
    }

    static func fmt(_ v: Double) -> String { String(format: "%.3f", v) }

    /// GIF：12 帧/秒、3 秒（1–1.5 秒暂停 0.5 秒），期望 30 帧、总时长约 2.5 秒。
    static func gifCheck(_ url: URL, _ check: (String, Bool, String) -> Void) {
        let clock = SimClock()
        let w = 320, h = 180
        guard let store = GifFrameStore(width: w, height: h) else { check("GIF 临时帧", false, "无法创建临时文件"); return }
        let p = RecordPipeline(queue: DispatchQueue(label: "selftest.gif"), movie: nil, gif: store, audioSources: 0)
        p.now = { clock.now }
        let h0 = 1000.0
        p.arm(at: h0)
        for i in 0..<36 {
            let t = h0 + Double(i) / 12
            if abs(t - (h0 + 1.0)) < 1e-9 { p.timeline.pause(at: t) }
            if abs(t - (h0 + 1.5)) < 1e-9 { p.timeline.resume(at: t) }
            clock.now = t
            if let pb = frame(w, h, t: Double(i) / 12) { p.video(pb, host: t) }
        }
        p.timeline.stop(at: h0 + 3.0)
        let sem = DispatchSemaphore(value: 0)
        var ok = false, end = 0.0
        p.finish { o, e in ok = o; end = e; sem.signal() }
        sem.wait()
        var progressCalls = 0
        let encoded = ok && store.encode(to: url, end: end) { _ in progressCalls += 1 }
        store.discard()
        check("GIF 编码", encoded, "录制时长 \(fmt(end)) 秒（期望 2.500），原始帧 \(store.frameCount)，进度回调 \(progressCalls) 次")
        guard encoded, let src = CGImageSourceCreateWithURL(url as CFURL, nil) else { return }
        let count = CGImageSourceGetCount(src)
        var total = 0.0
        for i in 0..<count {
            let props = CGImageSourceCopyPropertiesAtIndex(src, i, nil) as? [CFString: Any]
            let gif = props?[kCGImagePropertyGIFDictionary] as? [CFString: Any]
            total += (gif?[kCGImagePropertyGIFUnclampedDelayTime] as? Double) ?? (gif?[kCGImagePropertyGIFDelayTime] as? Double) ?? 0
        }
        let img = CGImageSourceCreateImageAtIndex(src, 0, nil)
        let size = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? Int ?? 0
        check("GIF 读回", count == 30 && abs(total - 2.5) < 0.03 && img?.width == w && img?.height == h,
              "\(count) 帧（期望 30），总时长 \(fmt(total)) 秒，\(img?.width ?? 0)×\(img?.height ?? 0)，\(size / 1024) KB")
    }

    /// 录音文件：2 秒 44.1k 单声道 440Hz → 48k 立体声 m4a/wav。
    static func audioFileCheck(_ url: URL, kind: MovieWriter.AudioKind, _ check: (String, Bool, String) -> Void) {
        let name = kind == .aac ? "M4A" : "WAV"
        let clock = SimClock()
        let movie: MovieWriter
        do {
            movie = try MovieWriter(url: url, fileType: kind == .aac ? .m4a : .wav, video: nil, audio: kind, keepAll: true)
        } catch {
            check(name, false, error.localizedDescription)
            return
        }
        let p = RecordPipeline(queue: DispatchQueue(label: "selftest.audio"), movie: movie, gif: nil, audioSources: 1)
        p.now = { clock.now }
        let h0 = 1000.0
        p.timeline.startNow(at: h0)
        var levels = 0
        p.onLevel = { _ in levels += 1 }
        var k = 0
        while Double(k * 1024) / 44100 < 2.0 {
            let t = h0 + Double(k * 1024) / 44100
            clock.now = min(t + 0.03, h0 + 2.0)
            if let b = sine(rate: 44100, channels: 1, interleaved: true, freq: 440, amp: 0.5, startFrame: k * 1024, frames: 1024),
               let sb = sampleBuffer(b, pts: CMTime(seconds: t, preferredTimescale: 44100)) { p.audio(0, sb, host: t) }
            k += 1
        }
        p.timeline.stop(at: h0 + 2.0)
        let sem = DispatchSemaphore(value: 0)
        var ok = false
        p.finish { o, _ in ok = o; sem.signal() }
        sem.wait()
        guard ok else { check(name, false, "写文件失败"); return }
        let asset = AVURLAsset(url: url)
        let dur = asset.duration.seconds
        guard let a = asset.tracks(withMediaType: .audio).first else { check(name, false, "没有音频轨"); return }
        let fd = a.formatDescriptions.first as! CMAudioFormatDescription
        let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fd)?.pointee
        let want = kind == .aac ? kAudioFormatMPEG4AAC : kAudioFormatLinearPCM
        var amp = 0.0
        if let pcm = decodeAudio(asset: asset, track: a), pcm.count > 48000 { amp = goertzel(pcm[24000..<72000], freq: 440, rate: 48000) }
        // AVAsset 读到的 AAC 时长含编码器补齐的最后一包（≤ 2 包 ≈ 43 ms）
        check(name, abs(dur - 2.0) < 0.05 && asbd?.mSampleRate == 48000 && asbd?.mChannelsPerFrame == 2 && asbd?.mFormatID == want && amp > 0.4 && levels > 0,
              "时长 \(fmt(dur)) 秒（期望 2.000），\(Int(asbd?.mSampleRate ?? 0)) Hz \(asbd?.mChannelsPerFrame ?? 0) 声道，位深 \(asbd?.mBitsPerChannel ?? 0)，440Hz 幅度 \(fmt(amp))（约 0.5），电平回调 \(levels) 次")
    }
}
