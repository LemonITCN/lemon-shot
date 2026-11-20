import AppKit
import AVFoundation

/// 录音（spec 10）的入口。
enum AudioFeature {
    /// 全局 recordAudio / 菜单「录音」：打开面板；面板已开时录音中 = 停止，未录音 = 关闭面板。
    static func start() { AudioPanelController.shared.toggle() }
}

// MARK: - 录音

/// 麦克风（AVCaptureSession）+ 系统声音（macOS 13+ SCStream 音频），重采样到 48k 立体声相加，写 m4a（AAC）或 wav。
final class AudioRecorder {
    let queue = DispatchQueue(label: "cn.lemonit.shot.audio", qos: .userInitiated)
    let format: String
    private var pipeline: RecordPipeline?
    private var mic: MicSource?
    private var system: ScreenSource?
    private var tempURL: URL?
    private(set) var startDate = Date()
    /// 电平（0–1 峰值），主线程回调，约 20 次/秒。
    var onLevel: ((Float) -> Void)?
    var onError: ((Error) -> Void)?

    init() {
        format = Settings.shared.string("audio.format") == "wav" ? "wav" : "m4a"
    }

    enum Failure: LocalizedError {
        case noSource, noMicrophone
        var errorDescription: String? { self == .noMicrophone ? L("record.noMicrophone") : L("audio.source") }
    }

    func start(microphone: Bool, systemAudio: Bool, deviceID: String) throws {
        let useSys = systemAudio && RecordConfig.systemAudioSupported
        guard microphone || useSys else { throw Failure.noSource }
        if microphone {
            guard let m = MicSource(deviceID: deviceID, queue: queue) else { throw Failure.noMicrophone }
            mic = m
        }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("lemonshot-audio-\(UUID().uuidString).\(format)")
        tempURL = url
        let writer = try MovieWriter(url: url, fileType: format == "wav" ? .wav : .m4a, video: nil, audio: format == "wav" ? .wav : .aac)
        let count = (mic != nil ? 1 : 0) + (useSys ? 1 : 0)
        let p = RecordPipeline(queue: queue, movie: writer, gif: nil, audioSources: count)
        var lastLevel = 0.0
        var peak: Float = 0
        p.onLevel = { [weak self] v in
            peak = max(peak, v)
            let now = HostClock.now()
            guard now - lastLevel >= 0.05 else { return }
            lastLevel = now
            let out = peak
            peak = 0
            DispatchQueue.main.async { self?.onLevel?(out) }
        }
        pipeline = p
        startDate = Date()
        let h = HostClock.now()
        queue.sync { p.timeline.startNow(at: h) }
        if let m = mic {
            m.onAudio = { [weak p] sb, t in p?.audio(0, sb, host: t) }
            m.start()
        }
        if useSys {
            let index = mic != nil ? 1 : 0
            let src = ScreenSources.make(queue: queue)
            src.onAudio = { [weak p] sb, t in p?.audio(index, sb, host: t) }
            src.onError = { [weak self] e in self?.onError?(e) }
            system = src
            let display = CGMainDisplayID()
            let params = ScreenSourceParams(displayID: display, sourceRect: .zero, width: 2, height: 2, fps: 1, pixelFormat: kCVPixelFormatType_32BGRA,
                                            showsCursor: false, captureAudio: true, audioOnly: true, includeWindows: [])
            src.start(params) { [weak self] err in
                if let e = err { self?.onError?(e) }
            }
        }
    }

    func elapsed() -> Double {
        guard let p = pipeline else { return 0 }
        let now = HostClock.now()
        return queue.sync { p.timeline.elapsed(at: now) }
    }

    /// 停止并写完文件，返回最终保存的位置（audio.folder）。completion 在主线程。
    func stop(completion: @escaping (URL?) -> Void) {
        guard let p = pipeline else { completion(nil); return }
        let h = HostClock.now()
        queue.async { p.timeline.stop(at: h) }
        let group = DispatchGroup()
        if let s = system {
            group.enter()
            s.stop { group.leave() }
        }
        if let m = mic {
            group.enter()
            m.stop { group.leave() }
        }
        let fmt = format
        let date = startDate
        let tmp = tempURL
        group.notify(queue: .main) {
            p.queue.async {
                p.finish { ok, _ in
                    DispatchQueue.main.async {
                        guard ok, let tmp = tmp else { completion(nil); return }
                        let folder = Settings.resolvePath(Settings.shared.string("audio.folder"))
                        do {
                            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                            let out = RecordConfig.outputURL(folder: folder, ext: fmt, date: date)
                            try FileManager.default.moveItem(at: tmp, to: out)
                            completion(out)
                        } catch {
                            try? FileManager.default.removeItem(at: tmp)
                            completion(nil)
                        }
                    }
                }
            }
        }
    }

    func cancel() {
        system?.stop {}
        mic?.stop()
        if let p = pipeline { queue.async { p.cancel() } }
        pipeline = nil
    }
}

// MARK: - 面板

/// 录音面板内容（300×120，圆角 12，popoverBg）：标题 + 计时 + 关闭；来源开关（麦克风/系统声音）+ 格式；电平条 + 开始/停止。
final class AudioPanelWidget: Widget {
    static let size = CGSize(width: 300, height: 120)
    static let pad: CGFloat = 14

    let close: IconButton
    let mic: AudioSourceChip
    let system: AudioSourceChip
    let action: ButtonW
    var recording = false { didSet { updateState() } }
    var clock = "00:00" { didSet { if oldValue != clock { setNeedsDisplay() } } }
    /// 电平 0–1（已换算成表盘刻度）。
    var level: CGFloat = 0 { didSet { if abs(oldValue - level) > 0.003 { setNeedsDisplay() } } }
    var formatLabel = "M4A"
    var onDrag: ((CGPoint) -> Void)?
    private var dragStart: CGPoint?

    override var isInteractive: Bool { true }

    override init() {
        close = IconButton(icon: "x", size: 24, iconSize: 14, radius: 6)
        mic = AudioSourceChip(icon: "mic", offIcon: "mic-off", title: L("record.microphone"))
        system = AudioSourceChip(icon: "volume-2", offIcon: "volume-x", title: L("record.systemAudio"))
        action = ButtonW(L("audio.start"), kind: .primary)
        super.init()
        close.tooltip = L("common.close")
        if !RecordConfig.systemAudioSupported {
            system.isEnabled = false
            system.isOn = false
            system.tooltip = L("record.systemAudioUnsupported")
        }
        for w in [close, mic, system, action] as [Widget] { add(w) }
        frame.size = AudioPanelWidget.size
    }

    private func updateState() {
        action.title = L(recording ? "audio.stop" : "audio.start")
        action.kind = recording ? .danger : .primary
        mic.isEnabled = !recording
        system.isEnabled = !recording && RecordConfig.systemAudioSupported
        layout()
        setNeedsDisplay()
    }

    override func layout() {
        let p = AudioPanelWidget.pad
        let r = frame
        close.frame = CGRect(x: r.maxX - p - 24 + 6, y: r.minY + 12, width: 24, height: 24)
        let ms = mic.preferredSize()
        mic.frame = CGRect(x: r.minX + p, y: r.minY + 46, width: ms.width, height: ms.height)
        let ss = system.preferredSize()
        system.frame = CGRect(x: mic.frame.maxX + 8, y: r.minY + 46, width: ss.width, height: ss.height)
        let bs = action.preferredSize()
        let bw = max(bs.width, 84)
        action.frame = CGRect(x: r.maxX - p - bw, y: r.minY + 80, width: bw, height: bs.height)
    }

    var meterRect: CGRect {
        let p = AudioPanelWidget.pad
        return CGRect(x: frame.minX + p, y: action.frame.midY - 3, width: action.frame.minX - 14 - frame.minX - p, height: 6)
    }

    override func draw(_ ctx: CGContext) {
        let r = frame
        Draw.panel(ctx, r, radius: 12, fill: Theme.color("popoverBg"), border: Theme.color("border"),
                   shadow: Theme.color("shadow"), blur: Theme.f("toolbar.shadowBlur"), offsetY: Theme.f("toolbar.shadowOffsetY"))
        let p = AudioPanelWidget.pad
        // 标题行
        let titleRect = CGRect(x: r.minX + p, y: r.minY + 12, width: 150, height: 24)
        Icons.draw(ctx, "audio-lines", in: CGRect(x: titleRect.minX, y: titleRect.midY - 8, width: 16, height: 16), color: Theme.color("icon"), lineWidth: 1.6)
        Draw.text(ctx, L("audio.title"), font: Fonts.semibold(Fonts.body), color: Theme.color("text"), in: titleRect.offsetBy(dx: 22, dy: 0).insetBy(dx: 0, dy: 0))
        let clockFont = Fonts.digits(Fonts.body, .medium)
        let cw = ceil(Draw.textWidth(clock, clockFont)) + 2
        let clockRect = CGRect(x: close.frame.minX - 8 - cw, y: r.minY + 12, width: cw, height: 24)
        Draw.text(ctx, clock, font: clockFont, color: Theme.color(recording ? "text" : "textSecondary"), in: clockRect, truncate: false)
        if recording {
            let d: CGFloat = 8
            Draw.ellipse(ctx, CGRect(x: clockRect.minX - 6 - d, y: clockRect.midY - d / 2, width: d, height: d), fill: Theme.tokenColor("recordBar.dot"))
        }
        // 格式
        Draw.text(ctx, formatLabel, font: Fonts.ui(Fonts.caption), color: Theme.color("textTertiary"),
                  in: CGRect(x: r.maxX - p - 60, y: mic.frame.minY, width: 60, height: mic.frame.height), align: .right)
        // 电平条
        let m = meterRect
        Draw.fill(ctx, m, Theme.color("controlTrack"), radius: m.height / 2)
        let lv = max(0, min(1, level))
        if lv > 0.001 {
            let fill = CGRect(x: m.minX, y: m.minY, width: max(m.height, m.width * lv), height: m.height)
            Draw.fill(ctx, fill, lv > 0.92 ? Theme.color("warning") : Theme.color("success"), radius: m.height / 2)
        }
        drawChildren(ctx)
    }

    override func mouseDown(_ p: CGPoint, event: NSEvent?) { dragStart = NSEvent.mouseLocation }

    override func mouseDragged(_ p: CGPoint, event: NSEvent?) {
        guard let s = dragStart else { return }
        let now = NSEvent.mouseLocation
        onDrag?(CGPoint(x: now.x - s.x, y: now.y - s.y))
        dragStart = now
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) { dragStart = nil }

    /// 峰值 → 表盘刻度（-50 dB 到 0 dB）。
    static func meter(_ peak: Float) -> CGFloat {
        guard peak > 0.000_01 else { return 0 }
        let db = 20 * log10(Double(peak))
        return CGFloat(max(0, min(1, (db + 50) / 50)))
    }
}

/// 来源开关：图标 + 文字，开 = surfaceSelected 底、强调色。
final class AudioSourceChip: Control {
    let icon: String
    let offIcon: String
    let title: String
    var isOn = true { didSet { if oldValue != isOn { setNeedsDisplay() } } }
    var onToggle: ((Bool) -> Void)?

    init(icon: String, offIcon: String, title: String) {
        self.icon = icon
        self.offIcon = offIcon
        self.title = title
        super.init()
        frame.size = preferredSize()
    }

    var font: NSFont { Fonts.ui(Fonts.body - 1) }

    override func preferredSize() -> CGSize {
        CGSize(width: ceil(10 + 16 + 6 + Draw.textWidth(title, font) + 12), height: controlHeight)
    }

    override func draw(_ ctx: CGContext) {
        let r = frame
        ctx.saveGState()
        ctx.setAlpha(alpha)
        let bg = isOn ? Theme.color("surfaceSelected") : (hovered && isEnabled ? Theme.color("surfaceHover") : Theme.color("controlTrack").alpha(0.6))
        Draw.fill(ctx, r, bg, radius: controlRadius)
        let fg = isOn ? Theme.color("iconActive") : Theme.color("textSecondary")
        Icons.draw(ctx, isOn ? icon : offIcon, in: CGRect(x: r.minX + 10, y: r.midY - 8, width: 16, height: 16), color: fg, lineWidth: 1.6)
        Draw.text(ctx, title, font: font, color: isOn ? Theme.color("text") : Theme.color("textSecondary"),
                  in: CGRect(x: r.minX + 32, y: r.minY, width: r.width - 38, height: r.height))
        ctx.restoreGState()
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside else { return }
        isOn.toggle()
        onToggle?(isOn)
    }
}

/// 录音浮动面板：屏幕右上角，不抢焦点、不进截图/录屏画面，可拖动。
final class AudioPanelController {
    static let shared = AudioPanelController()
    static let margin: CGFloat = 18

    private var panel: RecordPanel?
    private var widget: AudioPanelWidget?
    private var host: WidgetHost?
    private var recorder: AudioRecorder?
    private var tick: Timer?
    private var shownLevel: CGFloat = 0
    private var stopping = false

    var isRecording: Bool { recorder != nil }

    func toggle() {
        if panel == nil { show() } else if recorder != nil { stop() } else { close() }
    }

    private func show() {
        let m = AudioPanelController.margin
        let size = AudioPanelWidget.size
        let w = AudioPanelWidget()
        w.frame = CGRect(x: m, y: m, width: size.width, height: size.height)
        w.formatLabel = Settings.shared.string("audio.format") == "wav" ? "WAV" : "M4A"
        w.mic.isOn = Settings.shared.bool("audio.microphone")
        if RecordConfig.systemAudioSupported { w.system.isOn = Settings.shared.bool("audio.systemAudio") }
        w.layout()
        w.mic.onToggle = { Settings.shared.set("audio.microphone", $0) }
        w.system.onToggle = { Settings.shared.set("audio.systemAudio", $0) }
        w.close.action = { [weak self] in self?.closeOrStop() }
        w.action.action = { [weak self] in
            guard let self = self else { return }
            if self.recorder != nil { self.stop() } else { self.begin() }
        }
        let full = CGRect(x: 0, y: 0, width: size.width + 2 * m, height: size.height + 2 * m)
        let h = WidgetHost(frame: full, root: w)
        h.tooltipDelay = 0.5
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { NSMouseInRect(mouse, $0.frame, false) } ?? NSScreen.main
        let vis = screen?.visibleFrame ?? CGRect(x: 0, y: 0, width: 1440, height: 900)
        let p = RecordPanel(frame: CGRect(x: vis.maxX - 16 - size.width - m, y: vis.maxY - 16 - size.height - m, width: full.width, height: full.height), level: .floating)
        p.markPrivate()
        p.appearance = Theme.appearance
        p.contentView = h
        w.onDrag = { [weak p] d in
            guard let p = p else { return }
            p.setFrameOrigin(CGPoint(x: p.frame.minX + d.x, y: p.frame.minY + d.y))
        }
        panel = p
        widget = w
        host = h
        p.orderFrontRegardless()
    }

    private func closeOrStop() {
        if recorder != nil { stop() } else { close() }
    }

    private func close() {
        tick?.invalidate()
        tick = nil
        host?.hideTooltip()
        panel?.orderOut(nil)
        panel?.close()
        panel = nil
        widget = nil
        host = nil
    }

    private func begin() {
        guard recorder == nil, !stopping, let w = widget else { return }
        let s = Settings.shared
        let wantMic = w.mic.isOn
        let wantSys = w.system.isOn && RecordConfig.systemAudioSupported
        guard wantMic || wantSys else { return }
        if wantSys && !ScreenCapture.hasPermission {
            Toast.show(L("audio.needScreenPermission"), icon: "triangle-alert") { PrefsWindowController.show(page: "permissions") }
            return
        }
        let go = { [weak self] (micOK: Bool) in
            guard let self = self, self.recorder == nil, self.widget != nil else { return }
            if wantMic && !micOK {
                Toast.show(L("record.needMicPermission"), icon: "triangle-alert") { PrefsWindowController.show(page: "permissions") }
                if !wantSys { return }
            }
            let r = AudioRecorder()
            r.onLevel = { [weak self] v in
                guard let self = self else { return }
                self.shownLevel = max(AudioPanelWidget.meter(v), self.shownLevel)
            }
            r.onError = { [weak self] e in
                Toast.show(L("audio.failed", ["error": e.localizedDescription]), icon: "triangle-alert")
                self?.stop()
            }
            do {
                try r.start(microphone: wantMic && micOK, systemAudio: wantSys, deviceID: s.string("audio.microphoneDevice"))
            } catch {
                r.cancel()
                Toast.show(L("audio.failed", ["error": error.localizedDescription]), icon: "triangle-alert")
                return
            }
            self.recorder = r
            self.widget?.recording = true
            self.widget?.clock = RecordConfig.clock(0)
            self.host?.needsDisplay = true
            let t = Timer(timeInterval: 1.0 / 20, repeats: true) { [weak self] _ in self?.update() }
            RunLoop.main.add(t, forMode: .common)
            self.tick = t
        }
        if wantMic { CapturePermission.ensure(.audio, completion: go) } else { go(false) }
    }

    private func update() {
        guard let r = recorder, let w = widget else { return }
        w.clock = RecordConfig.clock(r.elapsed())
        w.level = shownLevel
        shownLevel *= 0.82
        host?.needsDisplay = true
    }

    private func stop() {
        guard let r = recorder, !stopping else { return }
        stopping = true
        tick?.invalidate()
        tick = nil
        recorder = nil
        widget?.recording = false
        widget?.level = 0
        host?.needsDisplay = true
        r.stop { [weak self] url in
            self?.stopping = false
            if let url = url {
                Toast.show(L("audio.saved")) { NSWorkspace.shared.activateFileViewerSelecting([url]) }
            } else {
                Toast.show(L("audio.failed", ["error": RecordError.noFrames.localizedDescription]), icon: "triangle-alert")
            }
        }
    }
}
