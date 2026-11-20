import AppKit

/// 弹出层窗口：下拉菜单、调色板气泡、说明气泡、提示气泡。四周留出投影边距。
final class PopupPanel: NSPanel {
    static let shadowMargin: CGFloat = 18
    let host: WidgetHost
    private var monitors: [Any] = []
    var onClose: (() -> Void)?
    private static var open: [PopupPanel] = []

    init(content: Widget, size: CGSize, interactive: Bool) {
        let m = PopupPanel.shadowMargin
        let full = CGRect(x: 0, y: 0, width: size.width + 2 * m, height: size.height + 2 * m)
        content.frame = CGRect(x: m, y: m, width: size.width, height: size.height)
        content.layout()
        host = WidgetHost(frame: full, root: content)
        super.init(contentRect: full, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 2)
        isReleasedWhenClosed = false
        hidesOnDeactivate = false
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .transient]
        ignoresMouseEvents = !interactive
        appearance = Theme.appearance
        contentView = host
    }

    override var canBecomeKey: Bool { !ignoresMouseEvents }

    /// 在锚点（屏幕坐标）下方显示，放不下改到上方；水平按 align 对齐并夹在屏幕内。
    func show(below anchor: CGRect, gap: CGFloat = 4, alignLeft: Bool = true, dismissOnOutsideClick: Bool = true) {
        let m = PopupPanel.shadowMargin
        let size = frame.size
        let screen = NSScreen.screens.first { $0.frame.intersects(anchor) } ?? NSScreen.main
        let vis = screen?.visibleFrame ?? CGRect(x: 0, y: 0, width: 1440, height: 900)
        var x = alignLeft ? anchor.minX - m : anchor.midX - size.width / 2
        var y = anchor.minY - gap - size.height + m
        if y < vis.minY { y = anchor.maxY + gap - m }
        x = max(vis.minX - m, min(x, vis.maxX - size.width + m))
        y = max(vis.minY - m, min(y, vis.maxY - size.height + m))
        setFrameOrigin(CGPoint(x: x, y: y))
        PopupPanel.open.append(self)
        orderFrontRegardless()
        if !ignoresMouseEvents { makeKey() }
        if dismissOnOutsideClick { installMonitors() }
    }

    private func installMonitors() {
        if let g = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown], handler: { [weak self] _ in self?.close() }) { monitors.append(g) }
        if let l = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown, .keyDown], handler: { [weak self] e in
            guard let self = self else { return e }
            if e.type == .keyDown {
                if e.keyCode == 53 { self.close(); return nil }
                return e
            }
            if e.window !== self { self.close() }
            return e
        }) { monitors.append(l) }
    }

    override func close() {
        for m in monitors { NSEvent.removeMonitor(m) }
        monitors.removeAll()
        host.hideTooltip()
        PopupPanel.open.removeAll { $0 === self }
        super.close()
        let cb = onClose
        onClose = nil
        cb?()
    }

    static func closeAll() { for p in open { p.close() } }
}

/// 气泡内容：深色提示（tipBg）或浅色说明（popoverBg）。
final class BubbleWidget: Widget {
    enum Style { case tooltip, info }
    let text: String
    let style: Style
    static let maxWidth: CGFloat = 280

    init(text: String, style: Style) {
        self.text = text
        self.style = style
    }

    var font: NSFont { Fonts.ui(style == .tooltip ? Fonts.caption + 1 : Fonts.body - 1) }
    var padX: CGFloat { style == .tooltip ? 8 : 12 }
    var padY: CGFloat { style == .tooltip ? 5 : 10 }

    func contentSize() -> CGSize {
        let s = Draw.wrappedSize(text, font: font, maxWidth: BubbleWidget.maxWidth - 2 * padX, lineSpacing: style == .tooltip ? 0 : 3)
        return CGSize(width: ceil(s.width + 2 * padX), height: ceil(s.height + 2 * padY))
    }

    override func draw(_ ctx: CGContext) {
        let r = frame
        switch style {
        case .tooltip:
            Draw.fill(ctx, r, Theme.cap("tipBg"), radius: Theme.f("capture.tipRadius"))
            Draw.wrapped(ctx, text, font: font, color: Theme.cap("tipText"), x: r.minX + padX, y: r.minY + padY, width: r.width - 2 * padX, lineSpacing: 0)
        case .info:
            Draw.panel(ctx, r, radius: Theme.f("popover.radius"), fill: Theme.color("popoverBg"), border: Theme.color("border"),
                       shadow: Theme.color("shadow"), blur: Theme.f("toolbar.shadowBlur"), offsetY: Theme.f("toolbar.shadowOffsetY"))
            Draw.wrapped(ctx, text, font: font, color: Theme.color("text"), x: r.minX + padX, y: r.minY + padY, width: r.width - 2 * padX)
        }
    }
}

enum Bubble {
    @discardableResult
    static func showTooltip(_ text: String, anchor: CGRect) -> PopupPanel {
        let w = BubbleWidget(text: text, style: .tooltip)
        let p = PopupPanel(content: w, size: w.contentSize(), interactive: false)
        p.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 2)
        p.show(below: anchor, gap: 6, alignLeft: false, dismissOnOutsideClick: false)
        return p
    }

    @discardableResult
    static func showInfo(_ text: String, anchor: CGRect) -> PopupPanel {
        let w = BubbleWidget(text: text, style: .info)
        let p = PopupPanel(content: w, size: w.contentSize(), interactive: false)
        p.show(below: anchor, gap: 6, alignLeft: false, dismissOnOutsideClick: true)
        return p
    }
}
