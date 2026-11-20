import AppKit
import LemonShotCore

// 录屏的自绘界面（spec 8.1、8.2）：录屏设置面板、控制条、GIF 进度。屏幕上与自测是同一份绘制代码。

enum RecordUI {
    /// 录屏模式截图层左下角提示。
    static func tipsLines(_ st: ls_session_state) -> [String] {
        st.has_selection != 0 && st.phase == Int32(LS_PHASE_SELECTED.rawValue) ? [L("record.tip.adjust")] : [L("record.selectRegion")]
    }

    /// 录屏模式的截图层：只保留 F 全屏、D 上次区域、V 开始、Tab 显隐面板，其余单键（标注工具、OCR、复制颜色等）解绑。
    static func configure(_ s: Session) {
        let keep: Set<String> = ["fullscreen", "lastArea", "record", "toggleToolbar"]
        for id in ShortcutTable.captureIDs where !keep.contains(id) {
            if let b = ShortcutTable.binding(id) { ls_session_bind_key(s.ptr, b, 0) }
        }
    }

    /// 偏好设置里的「系统声音」开关：macOS 13 以前禁用并提示 record.systemAudioUnsupported。
    static func systemAudioSwitch(_ key: String) -> SwitchW {
        let w = Bind.toggle(key)
        if !RecordConfig.systemAudioSupported {
            w.isOn = false
            w.isEnabled = false
            w.tooltip = L("record.systemAudioUnsupported")
        }
        return w
    }

    static var buttonSize: CGFloat { Theme.f("toolbar.button") }
    static var iconSize: CGFloat { Theme.f("toolbar.iconSize") }

    /// 设备选择菜单（按住开关 0.5 秒）。
    static func showDeviceMenu(audio: Bool, anchor: Widget) {
        guard let r = anchor.screenRect() else { return }
        let key = audio ? "record.microphoneDevice" : "record.cameraDevice"
        let list = Devices.list(audio: audio)
        let opts: [Option] = list.isEmpty ? [Option("", L(audio ? "record.noMicrophone" : "record.noCamera"), enabled: false)]
            : [Option("", L("record.default"))] + list.map { Option($0.0, $0.1) }
        showMenu(opts, selected: Settings.shared.string(key), anchor: r, host: anchor.host) { v in Settings.shared.set(key, v) }
    }

    static func showMenu(_ options: [Option], selected: String, anchor: CGRect, host: WidgetHost?, onClose: (() -> Void)? = nil, onPick: @escaping (String) -> Void) {
        let list = MenuListWidget(options: options, selected: selected, minWidth: 120)
        let panel = PopupPanel(content: list, size: list.contentSize(), interactive: true)
        let win = host?.window
        list.onPick = { [weak panel] v in
            panel?.close()
            onPick(v)
        }
        // 菜单收起后把键盘还给截图层（Esc、回车继续可用）
        panel.onClose = { [weak win] in
            onClose?()
            win?.makeKey()
        }
        panel.show(below: anchor, gap: 6)
    }
}

// MARK: - 设置面板开关

/// 录屏设置面板上的图标开关：开 = surfaceSelected 底 + iconActive 图标；按住 0.5 秒弹设备选择。
final class RecordToggle: Control {
    let key: String
    let onIcon: String
    let offIcon: String
    var isOn: Bool { didSet { if oldValue != isOn { setNeedsDisplay() } } }
    var onToggle: ((Bool) -> Void)?
    var onLongPress: (() -> Void)?
    private var timer: Timer?
    private var longFired = false

    init(key: String, onIcon: String, offIcon: String? = nil, tooltip: String) {
        self.key = key
        self.onIcon = onIcon
        self.offIcon = offIcon ?? onIcon
        isOn = Settings.shared.bool(key)
        super.init()
        self.tooltip = tooltip
        frame.size = CGSize(width: RecordUI.buttonSize, height: RecordUI.buttonSize)
    }

    override func draw(_ ctx: CGContext) {
        let r = frame
        let radius = Theme.f("toolbar.buttonRadius")
        ctx.saveGState()
        ctx.setAlpha(isEnabled ? 1 : 0.45)
        if isOn {
            Draw.fill(ctx, r, Theme.color("surfaceSelected"), radius: radius)
        } else if isEnabled && pressed {
            Draw.fill(ctx, r, Theme.color("surfacePressed"), radius: radius)
        } else if isEnabled && hovered {
            Draw.fill(ctx, r, Theme.color("surfaceHover"), radius: radius)
        }
        let s = RecordUI.iconSize
        let ir = CGRect(x: r.midX - s / 2, y: r.midY - s / 2, width: s, height: s)
        Icons.draw(ctx, isOn ? onIcon : offIcon, in: ir, color: isOn ? Theme.color("iconActive") : Theme.color("icon"))
        ctx.restoreGState()
    }

    override func mouseDown(_ p: CGPoint, event: NSEvent?) {
        longFired = false
        timer?.invalidate()
        guard onLongPress != nil else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: false) { [weak self] _ in
            guard let self = self, self.pressed else { return }
            self.longFired = true
            self.onLongPress?()
        }
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        timer?.invalidate()
        timer = nil
        guard inside, !longFired else { return }
        isOn.toggle()
        Settings.shared.set(key, isOn)
        onToggle?(isOn)
    }
}

// MARK: - 设置面板下拉

/// 紧凑下拉：当前值 + chevron，点开自绘菜单。宽度按所有选项里最宽的定，切换时面板不变宽。
final class RecordChip: Control {
    let options: [Option]
    var value: String { didSet { setNeedsDisplay() } }
    var onChange: ((String) -> Void)?
    private var open = false

    static var font: NSFont { Fonts.ui(Fonts.body) }

    init(_ options: [Option], value: String, tooltip: String) {
        self.options = options
        self.value = value
        super.init()
        self.tooltip = tooltip
        let tw = options.map { Draw.textWidth($0.label, RecordChip.font) }.max() ?? 40
        frame.size = CGSize(width: ceil(tw + 8 + 4 + 12 + 6), height: RecordUI.buttonSize)
    }

    var label: String { options.first { $0.value == value }?.label ?? value }

    override func draw(_ ctx: CGContext) {
        let r = frame
        ctx.saveGState()
        ctx.setAlpha(isEnabled ? 1 : 0.45)
        let radius = Theme.f("toolbar.buttonRadius")
        if open || (isEnabled && pressed) {
            Draw.fill(ctx, r, Theme.color("surfacePressed"), radius: radius)
        } else if isEnabled && hovered {
            Draw.fill(ctx, r, Theme.color("surfaceHover"), radius: radius)
        }
        Draw.text(ctx, label, font: RecordChip.font, color: Theme.color("text"), in: CGRect(x: r.minX + 8, y: r.minY, width: r.width - 30, height: r.height))
        Icons.draw(ctx, "chevron-down", in: CGRect(x: r.maxX - 18, y: r.midY - 6, width: 12, height: 12), color: Theme.color("textSecondary"), lineWidth: 1.6)
        ctx.restoreGState()
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside, !open, let anchor = screenRect() else { return }
        open = true
        setNeedsDisplay()
        host?.hideTooltip()
        RecordUI.showMenu(options, selected: value, anchor: anchor, host: host, onClose: { [weak self] in
            self?.open = false
            self?.setNeedsDisplay()
        }) { [weak self] v in
            guard let self = self else { return }
            if v != self.value {
                self.value = v
                self.onChange?(v)
            }
        }
    }
}

// MARK: - 设置面板

/// 录屏设置面板（spec 8.1）：麦克风 · 系统声音 · 摄像头 · 光标 · 点击 · 按键 | 格式 | 清晰度 | 帧率 | 开始录制。
/// 高 toolbar.height，外观与截图工具栏一致。改动立即写回 record.*。
final class RecordSetupPanel: Widget {
    var onStart: (() -> Void)?
    private(set) var toggles: [RecordToggle] = []
    private(set) var chips: [RecordChip] = []
    let startButton: ButtonW
    private var separatorXs: [CGFloat] = []
    private var mic: RecordToggle!
    private var system: RecordToggle!

    override var isInteractive: Bool { true }

    override init() {
        startButton = ButtonW(L("record.start"), kind: .primary)
        super.init()
        mic = RecordToggle(key: "record.microphone", onIcon: "mic", offIcon: "mic-off", tooltip: L("record.microphone") + " · " + L("record.holdToChooseDevice"))
        mic.onLongPress = { [weak self] in if let m = self?.mic { RecordUI.showDeviceMenu(audio: true, anchor: m) } }
        system = RecordToggle(key: "record.systemAudio", onIcon: "volume-2", offIcon: "volume-x", tooltip: L("record.systemAudio"))
        if !RecordConfig.systemAudioSupported {
            system.isEnabled = false
            system.isOn = false
            system.tooltip = L("record.systemAudioUnsupported")
        }
        let camera = RecordToggle(key: "record.camera", onIcon: "webcam", tooltip: L("record.camera") + " · " + L("record.holdToChooseDevice"))
        camera.onLongPress = { [weak camera] in if let c = camera { RecordUI.showDeviceMenu(audio: false, anchor: c) } }
        let cursor = RecordToggle(key: "record.showCursor", onIcon: "mouse-pointer-2", tooltip: L("record.showCursor"))
        let clicks = RecordToggle(key: "record.showClicks", onIcon: "mouse-pointer-click", tooltip: L("record.showClicks"))
        let keys = RecordToggle(key: "record.showKeys", onIcon: "keyboard", tooltip: L("record.showKeys"))
        toggles = [mic, system, camera, cursor, clicks, keys]
        for t in toggles { add(t) }

        let s = Settings.shared
        let format = RecordChip([Option("mp4", "MP4"), Option("gif", "GIF")], value: s.string("record.format") == "gif" ? "gif" : "mp4", tooltip: L("record.format"))
        format.onChange = { [weak self] v in
            Settings.shared.set("record.format", v)
            self?.refresh()
        }
        let quality = RecordChip(["ultra", "high", "standard"].map { Option($0, L("record.quality.\($0)")) }, value: s.string("record.quality"), tooltip: L("record.quality"))
        quality.onChange = { v in Settings.shared.set("record.quality", v) }
        let fps = RecordChip([15, 24, 30, 60].map { Option(String($0), L("common.fps", ["n": String($0)])) }, value: String(RecordConfig.clampFps(s.int("record.fps"))), tooltip: L("record.fps"))
        fps.onChange = { v in Settings.shared.set("record.fps", Double(Int(v) ?? 30)) }
        chips = [format, quality, fps]
        for c in chips { add(c) }

        startButton.action = { [weak self] in self?.onStart?() }
        add(startButton)
        frame.size = RecordSetupPanel.size(toggles: toggles.count, chips: chips, start: startButton)
        refresh()
    }

    private static var sepSpace: CGFloat { 2 * Theme.f("toolbar.separatorMargin") + Theme.f("toolbar.separatorWidth") }

    private static func size(toggles n: Int, chips: [RecordChip], start: ButtonW) -> CGSize {
        let bs = RecordUI.buttonSize, sp = Theme.f("toolbar.spacing")
        var w = 2 * Theme.f("toolbar.paddingX")
        w += CGFloat(n) * bs + CGFloat(n - 1) * sp
        for c in chips { w += sepSpace + c.frame.width }
        w += sepSpace + start.preferredSize().width + 2
        return CGSize(width: ceil(w), height: Theme.f("toolbar.height"))
    }

    /// 按设置刷新开关状态；GIF 不录声音，两个声音开关禁用。
    func refresh() {
        let gif = Settings.shared.string("record.format") == "gif"
        for t in toggles where t !== system { t.isOn = Settings.shared.bool(t.key) }
        mic.isEnabled = !gif
        if RecordConfig.systemAudioSupported {
            system.isOn = Settings.shared.bool("record.systemAudio")
            system.isEnabled = !gif
        }
        setNeedsDisplay()
    }

    override func layout() {
        let bs = RecordUI.buttonSize, sp = Theme.f("toolbar.spacing")
        let sepM = Theme.f("toolbar.separatorMargin"), sepW = Theme.f("toolbar.separatorWidth")
        var x = frame.minX + Theme.f("toolbar.paddingX")
        let y = frame.midY - bs / 2
        separatorXs.removeAll()
        for (i, t) in toggles.enumerated() {
            if i > 0 { x += sp }
            t.frame = CGRect(x: x, y: y, width: bs, height: bs)
            x += bs
        }
        for c in chips {
            x += sepM
            separatorXs.append(x)
            x += sepW + sepM
            c.frame.origin = CGPoint(x: x, y: y)
            x += c.frame.width
        }
        x += sepM
        separatorXs.append(x)
        x += sepW + sepM
        let bsz = startButton.preferredSize()
        startButton.frame = CGRect(x: x, y: frame.midY - bsz.height / 2, width: bsz.width, height: bsz.height)
    }

    override func draw(_ ctx: CGContext) {
        Draw.panel(ctx, frame, radius: Theme.f("toolbar.radius"), fill: Theme.color("toolbarBg"), border: Theme.color("toolbarBorder"),
                   shadow: Theme.color("shadow"), blur: Theme.f("toolbar.shadowBlur"), offsetY: Theme.f("toolbar.shadowOffsetY"))
        let sh = Theme.f("toolbar.separatorHeight")
        for x in separatorXs {
            Draw.fill(ctx, CGRect(x: x, y: frame.midY - sh / 2, width: Theme.f("toolbar.separatorWidth"), height: sh), Theme.color("divider"))
        }
        drawChildren(ctx)
    }
}

// MARK: - 控制条

/// 录制中控制条（spec 8.2）：recordBar.height 胶囊，红点 + 计时 · 暂停/继续 · 画笔 · 停止 · 取消。
final class RecordBarWidget: Widget {
    var clock = "00:00" { didSet { if oldValue != clock { setNeedsDisplay() } } }
    var paused = false { didSet { if oldValue != paused { updateButtons(); setNeedsDisplay() } } }
    var penOn = false { didSet { pen.selected = penOn } }
    let pause: IconButton
    let pen: IconButton
    let stop: IconButton
    let cancel: IconButton
    var onPause: (() -> Void)?
    var onPen: (() -> Void)?
    var onStop: (() -> Void)?
    var onCancel: (() -> Void)?

    override var isInteractive: Bool { true }

    static var height: CGFloat { Theme.f("recordBar.height") }
    static var clockFont: NSFont { Fonts.digits(Fonts.body, .medium) }
    static let padLeft: CGFloat = 14
    static let dot: CGFloat = 8

    override init() {
        let bs = RecordUI.buttonSize, isz = RecordUI.iconSize
        pause = IconButton(icon: "pause", size: bs, iconSize: isz, radius: bs / 2)
        pen = IconButton(icon: "pencil", size: bs, iconSize: isz, radius: bs / 2)
        stop = IconButton(icon: "square", size: bs, iconSize: isz - 2, radius: bs / 2)
        cancel = IconButton(icon: "x", size: bs, iconSize: isz, radius: bs / 2)
        super.init()
        stop.iconColor = Theme.tokenColor("recordBar.dot")
        stop.fillIcon = true
        pause.tooltip = L("record.pause")
        pen.tooltip = L("record.pen")
        stop.tooltip = L("record.stop")
        cancel.tooltip = L("record.cancel")
        pause.action = { [weak self] in self?.onPause?() }
        pen.action = { [weak self] in self?.onPen?() }
        stop.action = { [weak self] in self?.onStop?() }
        cancel.action = { [weak self] in self?.onCancel?() }
        for b in [pause, pen, stop, cancel] { add(b) }
        frame.size = contentSize()
    }

    private func updateButtons() {
        pause.icon = paused ? "play" : "pause"
        pause.tooltip = L(paused ? "record.resume" : "record.pause")
    }

    var clockWidth: CGFloat { ceil(Draw.textWidth(clock.count > 5 ? "0:00:00" : "00:00", RecordBarWidget.clockFont)) + 2 }

    func contentSize() -> CGSize {
        let bs = RecordUI.buttonSize, sp = Theme.f("toolbar.spacing")
        let sep = 2 * Theme.f("toolbar.separatorMargin") + Theme.f("toolbar.separatorWidth")
        let w = RecordBarWidget.padLeft + RecordBarWidget.dot + 8 + clockWidth + sep + 4 * bs + 3 * sp + (RecordBarWidget.height - bs) / 2
        return CGSize(width: ceil(w), height: RecordBarWidget.height)
    }

    private var separatorX: CGFloat {
        frame.minX + RecordBarWidget.padLeft + RecordBarWidget.dot + 8 + clockWidth + Theme.f("toolbar.separatorMargin")
    }

    override func layout() {
        let bs = RecordUI.buttonSize, sp = Theme.f("toolbar.spacing")
        var x = separatorX + Theme.f("toolbar.separatorWidth") + Theme.f("toolbar.separatorMargin")
        let y = frame.midY - bs / 2
        for b in [pause, pen, stop, cancel] {
            b.frame = CGRect(x: x, y: y, width: bs, height: bs)
            x += bs + sp
        }
    }

    override func draw(_ ctx: CGContext) {
        let r = frame
        Draw.panel(ctx, r, radius: Theme.f("recordBar.radius"), fill: Theme.color("toolbarBg"), border: Theme.color("toolbarBorder"),
                   shadow: Theme.color("shadow"), blur: Theme.f("toolbar.shadowBlur"), offsetY: Theme.f("toolbar.shadowOffsetY"))
        let d = RecordBarWidget.dot
        let dr = CGRect(x: r.minX + RecordBarWidget.padLeft, y: r.midY - d / 2, width: d, height: d)
        Draw.ellipse(ctx, dr, fill: paused ? Theme.color("textTertiary") : Theme.tokenColor("recordBar.dot"))
        let cx = dr.maxX + 8
        Draw.text(ctx, clock, font: RecordBarWidget.clockFont, color: Theme.color(paused ? "textSecondary" : "text"),
                  in: CGRect(x: cx, y: r.minY, width: clockWidth + 4, height: r.height), truncate: false)
        let sh = Theme.f("toolbar.separatorHeight")
        Draw.fill(ctx, CGRect(x: separatorX, y: r.midY - sh / 2, width: Theme.f("toolbar.separatorWidth"), height: sh), Theme.color("divider"))
        drawChildren(ctx)
    }
}

// MARK: - GIF 进度

/// 停止后生成 GIF 时的进度（record.encodingGif + 百分比 + 进度条），tipBg 底。
final class RecordProgressWidget: Widget {
    var text: String
    var progress: Double = 0 { didSet { setNeedsDisplay() } }
    static let size = CGSize(width: 260, height: 52)

    init(text: String) { self.text = text }

    override func draw(_ ctx: CGContext) {
        let r = frame
        Draw.fill(ctx, r, Theme.cap("tipBg"), radius: 12)
        let white = Theme.cap("tipText")
        let font = Fonts.ui(13)
        let pct = L("common.percent", ["n": String(Int((progress * 100).rounded(.down)))])
        Draw.text(ctx, text, font: font, color: white, in: CGRect(x: r.minX + 16, y: r.minY + 8, width: r.width - 80, height: 20))
        Draw.text(ctx, pct, font: Fonts.digits(13), color: white.alpha(0.8), in: CGRect(x: r.maxX - 64, y: r.minY + 8, width: 48, height: 20), align: .right)
        let track = CGRect(x: r.minX + 16, y: r.maxY - 16, width: r.width - 32, height: 4)
        Draw.fill(ctx, track, CGColor(gray: 1, alpha: 0.22), radius: 2)
        Draw.fill(ctx, CGRect(x: track.minX, y: track.minY, width: track.width * CGFloat(max(0, min(1, progress))), height: track.height), Theme.color("accent"), radius: 2)
    }
}
