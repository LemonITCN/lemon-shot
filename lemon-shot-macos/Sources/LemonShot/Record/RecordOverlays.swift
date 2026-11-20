import AppKit
import AVFoundation
import QuartzCore

// 进画面的浮层（spec 8.2）：画笔层、点击扩散圆、按键浮层、摄像头画中画。
// 录制开始前就建好并显示（透明、点击穿透），ScreenCaptureKit 的过滤器把它们列为「排除本应用」的例外。

// MARK: - 画笔

/// 区域上的透明画板：颜色取标注调色板第 1 色，线宽 4；主修饰键+Z 撤销一笔，Esc 退出并清除。
final class PenOverlay {
    final class Canvas: NSView {
        var strokes: [[CGPoint]] = []
        var current: [CGPoint]?
        var onEscape: (() -> Void)?
        let color: CGColor = Theme.hex(AnnotStyles.palette().first ?? "#FF3B30")
        static let width: CGFloat = 4

        override var isFlipped: Bool { true }
        override var acceptsFirstResponder: Bool { true }
        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
        override func resetCursorRects() { addCursorRect(bounds, cursor: .crosshair) }

        override func draw(_ dirtyRect: NSRect) {
            guard let ctx = NSGraphicsContext.current?.cgContext else { return }
            ctx.setStrokeColor(color)
            ctx.setLineWidth(Canvas.width)
            ctx.setLineCap(.round)
            ctx.setLineJoin(.round)
            for s in strokes + (current.map { [$0] } ?? []) {
                guard let first = s.first else { continue }
                if s.count == 1 {
                    ctx.setFillColor(color)
                    ctx.fillEllipse(in: CGRect(x: first.x - Canvas.width / 2, y: first.y - Canvas.width / 2, width: Canvas.width, height: Canvas.width))
                    continue
                }
                // 中点二次平滑
                ctx.move(to: first)
                for i in 1..<s.count {
                    let mid = CGPoint(x: (s[i - 1].x + s[i].x) / 2, y: (s[i - 1].y + s[i].y) / 2)
                    ctx.addQuadCurve(to: mid, control: s[i - 1])
                }
                ctx.addLine(to: s[s.count - 1])
                ctx.strokePath()
            }
        }

        override func mouseDown(with event: NSEvent) {
            current = [convert(event.locationInWindow, from: nil)]
            needsDisplay = true
        }

        override func mouseDragged(with event: NSEvent) {
            let p = convert(event.locationInWindow, from: nil)
            if let last = current?.last, hypot(p.x - last.x, p.y - last.y) < 1 { return }
            current?.append(p)
            needsDisplay = true
        }

        override func mouseUp(with event: NSEvent) {
            if let c = current { strokes.append(c) }
            current = nil
            needsDisplay = true
        }

        func undo() {
            guard !strokes.isEmpty else { return }
            strokes.removeLast()
            needsDisplay = true
        }

        func clear() {
            strokes.removeAll()
            current = nil
            needsDisplay = true
        }

        override func keyDown(with event: NSEvent) {
            if event.keyCode == 53 { onEscape?(); return }
            if event.keyCode == 6 && event.modifierFlags.contains(.command) { undo(); return }
            super.keyDown(with: event)
        }

        override func performKeyEquivalent(with event: NSEvent) -> Bool {
            if event.type == .keyDown && event.keyCode == 6 && event.modifierFlags.contains(.command) {
                undo()
                return true
            }
            return super.performKeyEquivalent(with: event)
        }
    }

    let panel: RecordPanel
    let canvas: Canvas
    private(set) var active = false
    var onEscape: (() -> Void)?

    init(region: CGRect) {
        panel = RecordPanel(frame: region, level: RecordPanel.overlayLevel)
        panel.keyable = true
        panel.ignoresMouseEvents = true
        panel.acceptsMouseMovedEvents = true
        canvas = Canvas(frame: CGRect(origin: .zero, size: region.size))
        panel.contentView = canvas
        canvas.onEscape = { [weak self] in self?.onEscape?() }
    }

    var windowID: CGWindowID { CGWindowID(max(0, panel.windowNumber)) }

    func show() { panel.orderFrontRegardless() }

    /// 进入画笔：接收鼠标与键盘（需要激活本应用）。
    func activate() {
        active = true
        panel.ignoresMouseEvents = false
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
        panel.makeFirstResponder(canvas)
        panel.invalidateCursorRects(for: canvas)
    }

    /// 退出画笔并清除笔迹。
    func deactivate() {
        active = false
        canvas.clear()
        panel.ignoresMouseEvents = true
        panel.resignKey()
    }

    func close() {
        panel.orderOut(nil)
        panel.close()
    }
}

// MARK: - 点击扩散圆

/// 每次按下在光标处画一个扩散圆（clickOverlay：左键蓝、右键橙，durationMs）。覆盖整块屏，点击穿透。
final class ClickOverlay {
    let panel: RecordPanel
    private let root = CALayer()
    private var monitors: [Any] = []

    init(screen: NSScreen) {
        panel = RecordPanel(frame: screen.frame, level: RecordPanel.overlayLevel)
        panel.ignoresMouseEvents = true
        let v = NSView(frame: CGRect(origin: .zero, size: screen.frame.size))
        v.wantsLayer = true
        v.layer?.addSublayer(root)
        root.frame = v.bounds
        panel.contentView = v
    }

    var windowID: CGWindowID { CGWindowID(max(0, panel.windowNumber)) }

    func show() {
        panel.orderFrontRegardless()
        let types: NSEvent.EventTypeMask = [.leftMouseDown, .rightMouseDown]
        if let g = NSEvent.addGlobalMonitorForEvents(matching: types, handler: { [weak self] e in self?.ripple(right: e.type == .rightMouseDown) }) { monitors.append(g) }
        if let l = NSEvent.addLocalMonitorForEvents(matching: types, handler: { [weak self] e in
            self?.ripple(right: e.type == .rightMouseDown)
            return e
        }) { monitors.append(l) }
    }

    private func ripple(right: Bool) {
        let m = NSEvent.mouseLocation
        let f = panel.frame
        guard f.contains(m) else { return }
        // 图层坐标：左下原点（未翻转），与 Cocoa 屏幕坐标同向
        let p = CGPoint(x: m.x - f.minX, y: m.y - f.minY)
        let r = Theme.f("clickOverlay.radius")
        let layer = CAShapeLayer()
        layer.frame = CGRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r)
        layer.path = CGPath(ellipseIn: CGRect(x: 0, y: 0, width: 2 * r, height: 2 * r), transform: nil)
        layer.fillColor = Theme.tokenColor(right ? "clickOverlay.rightColor" : "clickOverlay.color")
        layer.contentsScale = panel.backingScaleFactor
        let dur = Theme.f("clickOverlay.durationMs") / 1000
        CATransaction.begin()
        CATransaction.setCompletionBlock { layer.removeFromSuperlayer() }
        let scale = CABasicAnimation(keyPath: "transform.scale")
        scale.fromValue = 0.3
        scale.toValue = 1.0
        let fade = CABasicAnimation(keyPath: "opacity")
        fade.fromValue = 1.0
        fade.toValue = 0.0
        let group = CAAnimationGroup()
        group.animations = [scale, fade]
        group.duration = CFTimeInterval(dur)
        group.timingFunction = CAMediaTimingFunction(name: .easeOut)
        group.fillMode = .forwards
        group.isRemovedOnCompletion = false
        layer.add(group, forKey: "ripple")
        root.addSublayer(layer)
        CATransaction.commit()
    }

    func close() {
        for m in monitors { NSEvent.removeMonitor(m) }
        monitors.removeAll()
        panel.orderOut(nil)
        panel.close()
    }
}

// MARK: - 按键浮层

/// 一组按键（如 ⌘ ⇧ 4）的显示数据。
struct KeyCombo: Equatable {
    var text: String
    var hasModifier: Bool
}

/// 按键浮层的绘制：最近的若干组合排成一行，keysOverlay 样式。屏幕上与自测共用。
final class KeysOverlayWidget: Widget {
    struct Item { var combo: KeyCombo; var shownAt: Double; var alpha: CGFloat }
    var items: [Item] = []
    var position = "bottomCenter"
    var sizeName = "medium"
    var dark = true

    var fontSize: CGFloat { Theme.f("keysOverlay.\(sizeName).fontSize").nonZero(22) }
    var padding: CGFloat { Theme.f("keysOverlay.\(sizeName).padding").nonZero(10) }
    var radius: CGFloat { Theme.f("keysOverlay.\(sizeName).radius").nonZero(10) }
    var font: NSFont { Fonts.ui(fontSize, .medium) }

    func pillSize(_ text: String) -> CGSize {
        let h = ceil(Draw.lineHeight(font) + 2 * padding)
        return CGSize(width: ceil(max(h, Draw.textWidth(text, font) + 2 * padding * 1.4)), height: h)
    }

    /// 各组合的位置（区域坐标，左上原点）。放不下时只保留最近的。
    func layoutPills(in r: CGRect) -> [(Int, CGRect)] {
        let gap = Theme.f("keysOverlay.gap"), margin = Theme.f("keysOverlay.margin")
        var sizes = items.map { pillSize($0.combo.text) }
        var start = 0
        let avail = max(0, r.width - 2 * min(margin, r.width / 6))
        func total(_ from: Int) -> CGFloat { sizes[from...].reduce(0) { $0 + $1.width } + gap * CGFloat(max(0, sizes.count - from - 1)) }
        while start < sizes.count - 1 && total(start) > avail { start += 1 }
        sizes = Array(sizes[start...])
        let w = sizes.reduce(0) { $0 + $1.width } + gap * CGFloat(max(0, sizes.count - 1))
        let h = sizes.map { $0.height }.max() ?? 0
        let m = min(margin, max(8, (r.height - h) / 2))
        var x: CGFloat
        switch position {
        case "bottomLeft": x = r.minX + m
        case "bottomRight": x = r.maxX - m - w
        default: x = r.midX - w / 2
        }
        let y = position == "topCenter" ? r.minY + m : r.maxY - m - h
        var out: [(Int, CGRect)] = []
        for (i, s) in sizes.enumerated() {
            out.append((start + i, CGRect(x: x, y: y + (h - s.height) / 2, width: s.width, height: s.height).integral))
            x += s.width + gap
        }
        return out
    }

    override func draw(_ ctx: CGContext) {
        let bg = Theme.tokenColor(dark ? "keysOverlay.darkBg" : "keysOverlay.lightBg")
        let fg = Theme.tokenColor(dark ? "keysOverlay.darkText" : "keysOverlay.lightText")
        for (i, r) in layoutPills(in: frame) {
            let a = items[i].alpha
            guard a > 0.01 else { continue }
            ctx.saveGState()
            ctx.setAlpha(a)
            Draw.setShadow(ctx, offsetY: 2, blur: 8, color: CGColor(gray: 0, alpha: 0.25))
            Draw.fill(ctx, r, bg, radius: radius)
            ctx.restoreGState()
            ctx.saveGState()
            ctx.setAlpha(a)
            Draw.text(ctx, items[i].combo.text, font: font, color: fg, in: r, align: .center, truncate: false)
            ctx.restoreGState()
        }
    }

    // MARK: 按键 → 文字（Mac 用 ⌘⌥⌃⇧ 符号）

    static let specialKeys: [UInt16: String] = [
        36: "↩", 76: "⌤", 48: "⇥", 49: "␣", 51: "⌫", 117: "⌦", 53: "⎋",
        123: "←", 124: "→", 125: "↓", 126: "↑", 115: "↖", 119: "↘", 116: "⇞", 121: "⇟", 57: "⇪",
    ]

    static func combo(keyCode: UInt16, flags: NSEvent.ModifierFlags, characters: String?) -> KeyCombo {
        var parts: [String] = []
        if flags.contains(.command) { parts.append("⌘") }
        if flags.contains(.option) { parts.append("⌥") }
        if flags.contains(.control) { parts.append("⌃") }
        if flags.contains(.shift) { parts.append("⇧") }
        let key: String
        if let s = specialKeys[keyCode] {
            key = s
        } else if let n = KeyMap.name(forKeyCode: keyCode), n.hasPrefix("F"), n.count > 1, Int(n.dropFirst()) != nil {
            key = n
        } else if let c = characters?.trimmingCharacters(in: .controlCharacters), !c.isEmpty {
            key = c.uppercased()
        } else {
            key = KeyMap.name(forKeyCode: keyCode) ?? "?"
        }
        parts.append(key)
        let printable = specialKeys[keyCode] == nil && !(key.hasPrefix("F") && key.count > 1)
        // 只按 Shift 加可打印字符不算快捷键
        let hasMod = flags.contains(.command) || flags.contains(.option) || flags.contains(.control) || (flags.contains(.shift) && !printable)
        return KeyCombo(text: parts.joined(separator: " "), hasModifier: hasMod)
    }
}

private extension CGFloat {
    func nonZero(_ d: CGFloat) -> CGFloat { self > 0 ? self : d }
}

/// 按键浮层：CGEventTap 只监听（需要「输入监控」权限），在区域底部（record.keys.position）显示最近按键组合，fadeMs 后淡出。
final class KeysOverlay {
    let panel: RecordPanel
    private let widget = KeysOverlayWidget()
    private let host: WidgetHost
    private var tap: CFMachPort?
    private var source: CFRunLoopSource?
    private var timer: Timer?
    private let maxKeys: Int
    private let fade: Double
    private let onlyShortcuts: Bool
    private let hideRepeats: Bool

    static var hasPermission: Bool {
        if #available(macOS 10.15, *) { return IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted }
        return true
    }

    init(region: CGRect) {
        let s = Settings.shared
        maxKeys = max(1, s.int("record.keys.maxKeys"))
        fade = max(0.2, s.double("record.keys.fadeMs") / 1000)
        onlyShortcuts = s.bool("record.keys.onlyShortcuts")
        hideRepeats = s.bool("record.keys.hideRepeats")
        widget.position = s.string("record.keys.position")
        widget.sizeName = ["small", "medium", "large"].contains(s.string("record.keys.size")) ? s.string("record.keys.size") : "medium"
        switch s.string("record.keys.theme") {
        case "light": widget.dark = false
        case "dark": widget.dark = true
        default: widget.dark = Theme.isDark
        }
        let local = CGRect(origin: .zero, size: region.size)
        widget.frame = local
        host = WidgetHost(frame: local, root: widget)
        panel = RecordPanel(frame: region, level: RecordPanel.overlayLevel)
        panel.ignoresMouseEvents = true
        panel.contentView = host
    }

    var windowID: CGWindowID { CGWindowID(max(0, panel.windowNumber)) }

    /// 显示并开始监听。没有输入监控权限时返回假（不会触发系统授权弹窗）。
    @discardableResult
    func show() -> Bool {
        panel.orderFrontRegardless()
        guard KeysOverlay.hasPermission else { return false }
        let mask = (CGEventMask(1) << CGEventType.keyDown.rawValue)
        let me = Unmanaged.passUnretained(self).toOpaque()
        guard let t = CGEvent.tapCreate(tap: .cgSessionEventTap, place: .headInsertEventTap, options: .listenOnly, eventsOfInterest: mask,
                                        callback: KeysOverlay.callback, userInfo: me) else { return false }
        tap = t
        source = CFMachPortCreateRunLoopSource(nil, t, 0)
        CFRunLoopAddSource(CFRunLoopGetMain(), source, .commonModes)
        CGEvent.tapEnable(tap: t, enable: true)
        return true
    }

    private static let callback: CGEventTapCallBack = { _, type, event, info in
        guard let info = info else { return Unmanaged.passUnretained(event) }
        let me = Unmanaged<KeysOverlay>.fromOpaque(info).takeUnretainedValue()
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let t = me.tap { CGEvent.tapEnable(tap: t, enable: true) }
        } else if type == .keyDown {
            me.handle(event)
        }
        return Unmanaged.passUnretained(event)
    }

    private func handle(_ event: CGEvent) {
        if event.getIntegerValueField(.keyboardEventAutorepeat) != 0 { return }
        let code = UInt16(event.getIntegerValueField(.keyboardEventKeycode))
        let ns = NSEvent(cgEvent: event)
        let flags = ns?.modifierFlags ?? []
        let chars = ns?.characters(byApplyingModifiers: [])
        let c = KeysOverlayWidget.combo(keyCode: code, flags: flags, characters: chars)
        if onlyShortcuts && !c.hasModifier { return }
        let now = ProcessInfo.processInfo.systemUptime
        if hideRepeats, let last = widget.items.last, last.combo == c {
            widget.items[widget.items.count - 1].shownAt = now
            widget.items[widget.items.count - 1].alpha = 1
        } else {
            widget.items.append(KeysOverlayWidget.Item(combo: c, shownAt: now, alpha: 1))
            if widget.items.count > maxKeys { widget.items.removeFirst(widget.items.count - maxKeys) }
        }
        host.needsDisplay = true
        startTimer()
    }

    private func startTimer() {
        guard timer == nil else { return }
        let t = Timer(timeInterval: 1.0 / 30, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func tick() {
        let now = ProcessInfo.processInfo.systemUptime
        let fadeOut = Theme.f("animation.normalMs") / 1000
        var changed = false
        for i in widget.items.indices {
            let age = now - widget.items[i].shownAt
            let a: CGFloat = age <= fade ? 1 : CGFloat(max(0, 1 - (age - fade) / Double(fadeOut)))
            if a != widget.items[i].alpha { widget.items[i].alpha = a; changed = true }
        }
        let before = widget.items.count
        widget.items.removeAll { $0.alpha <= 0 }
        if changed || before != widget.items.count { host.needsDisplay = true }
        if widget.items.isEmpty {
            timer?.invalidate()
            timer = nil
        }
    }

    func close() {
        timer?.invalidate()
        timer = nil
        if let t = tap { CGEvent.tapEnable(tap: t, enable: false) }
        if let s = source { CFRunLoopRemoveSource(CFRunLoopGetMain(), s, .commonModes) }
        tap = nil
        source = nil
        panel.orderOut(nil)
        panel.close()
    }
}

// MARK: - 摄像头画中画

/// 区域右下角画中画（圆形或圆角矩形，直径为区域短边 22%），可拖动。
final class CameraOverlay {
    final class DragView: NSView {
        override var mouseDownCanMoveWindow: Bool { true }
        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    }

    let panel: RecordPanel
    private let session = AVCaptureSession()
    private static let controlQueue = DispatchQueue(label: "cn.lemonit.shot.camera-control")

    init?(deviceID: String, region: CGRect, shape: String) {
        let dev = (deviceID.isEmpty ? nil : AVCaptureDevice(uniqueID: deviceID)) ?? AVCaptureDevice.default(for: .video)
        guard let device = dev, let input = try? AVCaptureDeviceInput(device: device), session.canAddInput(input) else { return nil }
        session.addInput(input)
        if session.canSetSessionPreset(.high) { session.sessionPreset = .high }
        let d = max(64, (min(region.width, region.height) * 0.22).rounded())
        let margin = max(8, (d * 0.12).rounded())
        let frame = CGRect(x: region.maxX - margin - d, y: region.minY + margin, width: d, height: d)
        panel = RecordPanel(frame: frame, level: RecordPanel.overlayLevel)
        panel.isMovableByWindowBackground = true
        panel.isMovable = true
        let v = DragView(frame: CGRect(origin: .zero, size: frame.size))
        v.wantsLayer = true
        let radius = shape == "rounded" ? d * 0.18 : d / 2
        v.layer?.cornerRadius = radius
        v.layer?.masksToBounds = true
        v.layer?.backgroundColor = CGColor(gray: 0, alpha: 1)
        let preview = AVCaptureVideoPreviewLayer(session: session)
        preview.videoGravity = .resizeAspectFill
        preview.frame = v.bounds
        if let c = preview.connection, c.isVideoMirroringSupported {
            c.automaticallyAdjustsVideoMirroring = false
            c.isVideoMirrored = true
        }
        v.layer?.addSublayer(preview)
        let ring = CAShapeLayer()
        ring.path = CGPath(roundedRect: v.bounds.insetBy(dx: 1, dy: 1), cornerWidth: max(0, radius - 1), cornerHeight: max(0, radius - 1), transform: nil)
        ring.fillColor = nil
        ring.strokeColor = CGColor(gray: 1, alpha: 0.9)
        ring.lineWidth = 2
        v.layer?.addSublayer(ring)
        panel.contentView = v
    }

    var windowID: CGWindowID { CGWindowID(max(0, panel.windowNumber)) }

    func show() {
        panel.orderFrontRegardless()
        let s = session
        CameraOverlay.controlQueue.async { s.startRunning() }
    }

    func close() {
        let s = session
        CameraOverlay.controlQueue.async { s.stopRunning() }
        panel.orderOut(nil)
        panel.close()
    }
}
