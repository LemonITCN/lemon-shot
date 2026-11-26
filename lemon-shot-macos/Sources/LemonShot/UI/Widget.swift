import AppKit

/// 自绘控件树的节点。坐标为宿主视图坐标（左上原点）。
/// 屏幕上由 WidgetHost 承载并转发事件；自测时直接 draw 进离屏位图，两边是同一份绘制代码。
class Widget {
    var frame: CGRect = .zero
    var isHidden = false
    var isEnabled = true { didSet { if oldValue != isEnabled { setNeedsDisplay() } } }
    weak var parent: Widget?
    private(set) var children: [Widget] = []
    weak var ownHost: WidgetHost?
    var hovered = false
    var pressed = false
    /// 悬停一段时间后显示的提示气泡文字。
    var tooltip: String?
    /// 悬停时的光标；nil 表示箭头。
    var hoverCursor: NSCursor?

    init() {}

    var host: WidgetHost? { ownHost ?? parent?.host }

    func add(_ w: Widget) {
        w.parent = self
        children.append(w)
    }

    func removeAllChildren() {
        for c in children { c.parent = nil }
        children.removeAll()
    }

    /// 偏好的尺寸（布局用）。
    func preferredSize() -> CGSize { frame.size }

    func layout() { for c in children { c.layout() } }

    func draw(_ ctx: CGContext) { drawChildren(ctx) }

    func drawChildren(_ ctx: CGContext) {
        for c in children where !c.isHidden {
            c.draw(ctx)
        }
    }

    func hitTest(_ p: CGPoint) -> Widget? {
        guard !isHidden, frame.contains(p) || frame.isEmpty else { return nil }
        for c in children.reversed() where !c.isHidden {
            if let h = c.hitTest(p) { return h }
        }
        return frame.contains(p) && isInteractive ? self : nil
    }

    /// 能接收鼠标事件（标签之类的纯展示节点返回假）。
    var isInteractive: Bool { false }
    var acceptsFocus: Bool { false }
    var isFocused: Bool { host?.focused === self }

    // 事件：返回真表示处理了
    func mouseDown(_ p: CGPoint, event: NSEvent?) {}
    func mouseDragged(_ p: CGPoint, event: NSEvent?) {}
    func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {}
    func mouseMoved(_ p: CGPoint) {}
    func mouseEntered() { hovered = true; setNeedsDisplay() }
    func mouseExited() { hovered = false; setNeedsDisplay() }
    func scrollWheel(_ event: NSEvent) -> Bool { false }
    func keyDown(_ event: NSEvent) -> Bool { false }
    func focusChanged() { setNeedsDisplay() }

    func setNeedsDisplay() {
        host?.setNeedsDisplay(frame.insetBy(dx: -6, dy: -6))
    }

    /// 宿主视图坐标 → 屏幕坐标（弹出菜单、气泡定位用）。
    func screenRect(_ r: CGRect? = nil) -> CGRect? {
        guard let h = host, let win = h.window else { return nil }
        let inWin = h.convert(r ?? frame, to: nil)
        return win.convertToScreen(inWin)
    }

    func descendants() -> [Widget] {
        var out: [Widget] = []
        for c in children { out.append(c); out.append(contentsOf: c.descendants()) }
        return out
    }
}

/// 承载自绘控件树的视图。
class WidgetHost: NSView {
    var root: Widget { didSet { root.ownHost = self; needsDisplay = true } }
    /// 背景绘制（在控件树之前）。
    var background: ((CGContext, CGRect) -> Void)?
    private(set) weak var focused: Widget?
    private weak var hoverWidget: Widget?
    private weak var pressedWidget: Widget?
    private var trackingArea: NSTrackingArea?
    private var tooltipTimer: Timer?
    private var tooltipBubble: PopupPanel?
    var tooltipDelay: TimeInterval = 0.5

    init(frame: CGRect, root: Widget) {
        self.root = root
        super.init(frame: frame)
        root.ownHost = self
        wantsLayer = true
        layerContentsRedrawPolicy = .onSetNeedsDisplay
    }

    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { false }
    /// 点击时是否抢第一响应者。截图层的工具栏、样式面板设为假：编辑文字时点它们不能结束编辑。
    var takesFocus = true
    /// 截图层的工具栏、样式面板：宿主四周留了画阴影的透明边（ChromeHost.margin），透明边不接鼠标、不改光标，
    /// 交给下面的截图层（透明边会伸进选区，挡住选区下边的控制点）。只有面板本身（root.frame）接鼠标。
    var passThroughMargin = false {
        didSet { updateTrackingAreas() }
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        let v = super.hitTest(point)
        guard passThroughMargin, v === self else { return v }
        return root.frame.contains(convert(point, from: superview)) ? self : nil
    }
    override var acceptsFirstResponder: Bool { takesFocus }
    override var mouseDownCanMoveWindow: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = trackingArea { removeTrackingArea(t) }
        // 透明边不接鼠标时，悬停与光标也只在面板本身（不用 inVisibleRect，按面板的矩形）
        let t = passThroughMargin
            ? NSTrackingArea(rect: root.frame, options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .cursorUpdate], owner: self, userInfo: nil)
            : NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect, .cursorUpdate], owner: self, userInfo: nil)
        addTrackingArea(t)
        trackingArea = t
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.saveGState()
        ctx.clip(to: dirtyRect)
        background?(ctx, dirtyRect)
        root.draw(ctx)
        ctx.restoreGState()
    }

    func point(_ e: NSEvent) -> CGPoint { convert(e.locationInWindow, from: nil) }

    func setFocus(_ w: Widget?) {
        if focused === w { return }
        let old = focused
        focused = w
        old?.focusChanged()
        w?.focusChanged()
        if w != nil && takesFocus { window?.makeFirstResponder(self) }
    }

    private func updateHover(_ p: CGPoint?) {
        let w = p.flatMap { root.hitTest($0) }
        if w !== hoverWidget {
            hoverWidget?.mouseExited()
            hoverWidget = w
            w?.mouseEntered()
            hideTooltip()
            if let w = w, w.tooltip != nil { scheduleTooltip(for: w) }
        }
        if let p = p { w?.mouseMoved(p) }
        // 透明边不接鼠标时，移出面板后光标交给下面的截图层，不在这里改成箭头
        if p == nil && passThroughMargin { return }
        (w?.hoverCursor ?? NSCursor.arrow).set()
    }

    override func cursorUpdate(with event: NSEvent) {
        (hoverWidget?.hoverCursor ?? NSCursor.arrow).set()
    }

    override func mouseMoved(with event: NSEvent) { if pressedWidget == nil { updateHover(point(event)) } }
    override func mouseEntered(with event: NSEvent) { updateHover(point(event)) }
    override func mouseExited(with event: NSEvent) { if pressedWidget == nil { updateHover(nil) } }

    override func mouseDown(with event: NSEvent) {
        hideTooltip()
        let p = point(event)
        let w = root.hitTest(p)
        if let w = w, w.isEnabled {
            pressedWidget = w
            w.pressed = true
            if w.acceptsFocus { setFocus(w) } else if focused != nil, !(focused!.frame.contains(p)) { setFocus(nil) }
            w.mouseDown(p, event: event)
            w.setNeedsDisplay()
        } else {
            setFocus(nil)
        }
    }

    override func rightMouseDown(with event: NSEvent) {
        let p = point(event)
        if let w = root.hitTest(p) as? ContextMenuProvider, let m = w.contextMenu() {
            NSMenu.popUpContextMenu(m, with: event, for: self)
        } else {
            super.rightMouseDown(with: event)
        }
    }

    override func mouseDragged(with event: NSEvent) {
        pressedWidget?.mouseDragged(point(event), event: event)
    }

    override func mouseUp(with event: NSEvent) {
        guard let w = pressedWidget else { return }
        let p = point(event)
        w.pressed = false
        pressedWidget = nil
        w.mouseUp(p, inside: w.frame.contains(p), event: event)
        w.setNeedsDisplay()
        updateHover(p)
    }

    override func scrollWheel(with event: NSEvent) {
        let p = point(event)
        var w = root.hitTest(p)
        while let c = w {
            if c.scrollWheel(event) { return }
            w = c.parent
        }
        super.scrollWheel(with: event)
    }

    override func keyDown(with event: NSEvent) {
        if let f = focused, f.keyDown(event) { return }
        super.keyDown(with: event)
    }

    override func flagsChanged(with event: NSEvent) {
        if let f = focused as? FlagsObserver { f.flagsChanged(event) }
        super.flagsChanged(with: event)
    }

    // MARK: 提示气泡

    private func scheduleTooltip(for w: Widget) {
        tooltipTimer?.invalidate()
        tooltipTimer = Timer.scheduledTimer(withTimeInterval: tooltipDelay, repeats: false) { [weak self, weak w] _ in
            guard let self = self, let w = w, w === self.hoverWidget, let text = w.tooltip, let r = w.screenRect() else { return }
            self.tooltipBubble = Bubble.showTooltip(text, anchor: r)
        }
    }

    func hideTooltip() {
        tooltipTimer?.invalidate()
        tooltipTimer = nil
        tooltipBubble?.close()
        tooltipBubble = nil
    }

    override func viewWillMove(toWindow newWindow: NSWindow?) {
        if newWindow == nil { hideTooltip() }
        super.viewWillMove(toWindow: newWindow)
    }
}

protocol ContextMenuProvider: AnyObject { func contextMenu() -> NSMenu? }
protocol FlagsObserver: AnyObject { func flagsChanged(_ event: NSEvent) }

/// 只负责画的视图：闭包里画进左上原点的 CGContext。
final class PaintView: NSView {
    var painter: (CGContext, CGRect) -> Void
    init(frame: CGRect, painter: @escaping (CGContext, CGRect) -> Void) {
        self.painter = painter
        super.init(frame: frame)
        wantsLayer = true
        layerContentsRedrawPolicy = .onSetNeedsDisplay
    }
    required init?(coder: NSCoder) { fatalError() }
    override var isFlipped: Bool { true }
    override var isOpaque: Bool { false }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        painter(ctx, dirtyRect)
    }
}

/// 自测与导出：把控件树画进离屏位图。
enum Offscreen {
    static func render(size: CGSize, scale: CGFloat, _ body: (CGContext) -> Void) -> CGImage? {
        guard let ctx = Draw.bitmap(width: size.width, height: size.height, scale: scale) else { return nil }
        body(ctx)
        return ctx.makeImage()
    }
}
