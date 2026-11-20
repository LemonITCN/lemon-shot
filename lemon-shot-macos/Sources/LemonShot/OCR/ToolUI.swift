import AppKit

// 第二期几个结果窗口（文字识别、翻译、长截图结果、带壳预览）共用的自绘部件与窗口外壳。

// MARK: - 窗口外壳

/// 承载工具窗口内容的视图：标题栏区域里没有控件的地方可以拖动窗口。
final class ToolHost: WidgetHost {
    var dragHeight: CGFloat = Theme.f("window.titleBarHeight")

    override func mouseDown(with event: NSEvent) {
        let p = point(event)
        if p.y < dragHeight, root.hitTest(p) == nil {
            window?.performDrag(with: event)
            return
        }
        super.mouseDown(with: event)
    }
}

/// 工具窗口：透明标题栏 + 全尺寸自绘内容，主题变化时重画，大小变化时重新排版。
class ToolWindowController: NSObject, NSWindowDelegate {
    let window: NSWindow
    let host: ToolHost
    let root: Widget
    /// 原生子视图（文本区、可滚动图片）排版后的定位。
    var placeNative: (() -> Void)?
    var onClose: (() -> Void)?

    init(size: CGSize, minSize: CGSize, title: String, root: Widget, resizable: Bool) {
        var style: NSWindow.StyleMask = [.titled, .closable, .miniaturizable, .fullSizeContentView]
        if resizable { style.insert(.resizable) }
        window = NSWindow(contentRect: CGRect(origin: .zero, size: size), styleMask: style, backing: .buffered, defer: false)
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.title = title
        window.isReleasedWhenClosed = false
        window.minSize = minSize
        window.collectionBehavior = [.fullScreenAuxiliary]
        self.root = root
        root.frame = CGRect(origin: .zero, size: size)
        host = ToolHost(frame: CGRect(origin: .zero, size: size), root: root)
        host.autoresizingMask = [.width, .height]
        host.background = { ctx, r in Draw.fill(ctx, r, Theme.color("windowBg")) }
        super.init()
        window.contentView = host
        window.delegate = self
        applyTheme()
        NotificationCenter.default.addObserver(self, selector: #selector(themeDidChange), name: Theme.didChange, object: nil)
    }

    func relayout() {
        root.frame = host.bounds
        root.layout()
        placeNative?()
        host.needsDisplay = true
    }

    func applyTheme() {
        window.appearance = Theme.appearance
        window.backgroundColor = NSColor(cgColor: Theme.color("windowBg"))
        host.needsDisplay = true
    }

    @objc func themeDidChange() {
        applyTheme()
        themeChanged()
    }

    /// 子类：主题变化时更新原生视图颜色。
    func themeChanged() {}

    func show(center: Bool, activate: Bool = true) {
        relayout()
        if center {
            let m = NSEvent.mouseLocation
            let vis = (ScreenCapture.screen(at: m) ?? NSScreen.main)?.visibleFrame ?? CGRect(x: 0, y: 0, width: 1440, height: 900)
            let s = window.frame.size
            window.setFrameOrigin(CGPoint(x: (vis.midX - s.width / 2).rounded(), y: (vis.midY - s.height / 2).rounded()))
        }
        if activate { NSApp.activate(ignoringOtherApps: true) }
        window.makeKeyAndOrderFront(nil)
    }

    func windowDidResize(_ notification: Notification) { relayout() }

    func windowWillClose(_ notification: Notification) {
        NativeEditor.end()
        PopupPanel.closeAll()
        host.hideTooltip()
        NotificationCenter.default.removeObserver(self)
        onClose?()
    }
}

// MARK: - 标题栏

/// 窗口顶部：居中标题（可带副标题），底部分隔线。右侧可放控件（由外部排版）。
final class ToolHeader: Widget {
    var title: String { didSet { setNeedsDisplay() } }
    var subtitle: String? { didSet { setNeedsDisplay() } }

    init(title: String) { self.title = title }

    override func draw(_ ctx: CGContext) {
        Draw.fill(ctx, CGRect(x: frame.minX, y: frame.maxY - 1, width: frame.width, height: 1), Theme.color("divider"))
        let tf = Fonts.semibold(Fonts.title - 2)
        let tw = Draw.textWidth(title, tf)
        if let sub = subtitle, !sub.isEmpty {
            let cf = Fonts.digits(Fonts.caption + 1)
            let cw = Draw.textWidth(sub, cf)
            let x = frame.midX - (tw + 8 + cw) / 2
            Draw.text(ctx, title, font: tf, color: Theme.color("text"), in: CGRect(x: x, y: frame.minY, width: tw + 1, height: frame.height), truncate: false)
            Draw.text(ctx, sub, font: cf, color: Theme.color("textSecondary"), in: CGRect(x: x + tw + 8, y: frame.minY + 1, width: cw + 1, height: frame.height), truncate: false)
        } else {
            Draw.text(ctx, title, font: tf, color: Theme.color("text"), in: CGRect(x: frame.midX - tw / 2, y: frame.minY, width: tw + 1, height: frame.height), truncate: false)
        }
        drawChildren(ctx)
    }
}

/// 底部按钮栏：顶部分隔线。
final class ToolFooter: Widget {
    override func draw(_ ctx: CGContext) {
        Draw.fill(ctx, CGRect(x: frame.minX, y: frame.minY, width: frame.width, height: 1), Theme.color("divider"))
        drawChildren(ctx)
    }

    /// 从右往左排按钮（数组第一个在最右），返回左边界。
    @discardableResult
    func layoutRight(_ buttons: [Widget], margin: CGFloat = 16, gap: CGFloat = 8) -> CGFloat {
        var x = frame.maxX - margin
        for b in buttons where !b.isHidden {
            let s = b.preferredSize()
            x -= s.width
            b.frame = CGRect(x: x, y: frame.midY - s.height / 2, width: s.width, height: s.height)
            x -= gap
        }
        return x + gap
    }
}

// MARK: - 开关胶囊

/// 工具行上的开关：文字胶囊，开时 surfaceSelected 底 + 强调色字与边。
final class ToggleChip: Control {
    var title: String { didSet { setNeedsDisplay() } }
    var isOn: Bool { didSet { if oldValue != isOn { setNeedsDisplay() } } }
    var onChange: ((Bool) -> Void)?

    init(_ title: String, isOn: Bool, onChange: ((Bool) -> Void)? = nil) {
        self.title = title
        self.isOn = isOn
        self.onChange = onChange
        super.init()
    }

    var font: NSFont { isOn ? Fonts.medium(Fonts.body - 1) : Fonts.ui(Fonts.body - 1) }
    static let height: CGFloat = 26

    override func preferredSize() -> CGSize {
        CGSize(width: ceil(Draw.textWidth(title, Fonts.medium(Fonts.body - 1)) + 22), height: ToggleChip.height)
    }

    override func draw(_ ctx: CGContext) {
        let r = CGRect(x: frame.minX, y: frame.midY - ToggleChip.height / 2, width: frame.width, height: ToggleChip.height)
        ctx.saveGState()
        ctx.setAlpha(alpha)
        if isOn {
            Draw.fill(ctx, r, Theme.color("surfaceSelected"), radius: r.height / 2)
            Draw.stroke(ctx, r, Theme.color("accent").alpha(0.55), width: 1, radius: r.height / 2)
        } else {
            Draw.fill(ctx, r, pressed ? Theme.color("surfacePressed") : (hovered ? Theme.color("surfaceHover") : Theme.color("surface")), radius: r.height / 2)
            Draw.stroke(ctx, r, Theme.color("inputBorder"), width: 1, radius: r.height / 2)
        }
        Draw.text(ctx, title, font: font, color: isOn ? Theme.color("iconActive") : Theme.color("textSecondary"), in: r, align: .center, truncate: false)
        ctx.restoreGState()
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside else { return }
        isOn.toggle()
        onChange?(isOn)
    }
}

// MARK: - 文本区

/// 多行文本区的外框。屏幕上由原生 NSTextView 叠在框里负责输入；自测时 drawsText 为真，直接画出文字。
final class TextAreaW: Widget {
    var text = ""
    var drawsText = false
    /// 内容为空时框中央显示的状态（正在识别…）。
    var overlay: String? { didSet { setNeedsDisplay() } }
    var overlayColorKey = "textTertiary"
    static let inset = CGSize(width: 8, height: 8)

    override func draw(_ ctx: CGContext) {
        let r = frame
        let radius = Theme.f("window.controlRadius")
        Draw.fill(ctx, r, Theme.color("inputBg"), radius: radius)
        Draw.stroke(ctx, r, Theme.color("inputBorder"), width: 1, radius: radius)
        if drawsText, !text.isEmpty {
            ctx.saveGState()
            ctx.clip(to: r.insetBy(dx: 1, dy: 1))
            // NSTextView：textContainerInset + lineFragmentPadding 5
            let x = r.minX + TextAreaW.inset.width + 5
            Draw.wrapped(ctx, text, font: NativeTextArea.font, color: Theme.color("text"), x: x, y: r.minY + TextAreaW.inset.height,
                         width: r.width - 2 * (TextAreaW.inset.width + 5), lineSpacing: 2)
            ctx.restoreGState()
        }
        if let o = overlay, text.isEmpty {
            Draw.text(ctx, o, font: Fonts.ui(Fonts.body), color: Theme.color(overlayColorKey), in: r, align: .center)
        }
    }
}

/// 叠在 TextAreaW 上的原生多行文本框（支持输入法、选择、滚动）。
final class NativeTextArea: NSObject, NSTextViewDelegate {
    static var font: NSFont { Fonts.ui(Fonts.body) }
    let scroll: NSScrollView
    let textView: NSTextView
    var onChange: ((String) -> Void)?

    init(editable: Bool) {
        scroll = NSScrollView(frame: .zero)
        scroll.drawsBackground = false
        scroll.borderType = .noBorder
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
        scroll.autohidesScrollers = true
        let size = scroll.contentSize
        textView = NSTextView(frame: CGRect(origin: .zero, size: size))
        textView.minSize = CGSize(width: 0, height: 0)
        textView.maxSize = CGSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        textView.isVerticallyResizable = true
        textView.isHorizontallyResizable = false
        textView.autoresizingMask = [.width]
        textView.textContainer?.widthTracksTextView = true
        textView.textContainerInset = TextAreaW.inset
        textView.drawsBackground = false
        textView.isRichText = false
        textView.importsGraphics = false
        textView.allowsUndo = true
        textView.isEditable = editable
        textView.isSelectable = true
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.isContinuousSpellCheckingEnabled = false
        textView.font = NativeTextArea.font
        let para = NSMutableParagraphStyle()
        para.lineSpacing = 2
        textView.defaultParagraphStyle = para
        scroll.documentView = textView
        super.init()
        textView.delegate = self
        applyTheme()
    }

    var string: String {
        get { textView.string }
        set {
            textView.string = newValue
            applyTheme()
        }
    }

    func applyTheme() {
        let c = NSColor(cgColor: Theme.color("text")) ?? .textColor
        textView.textColor = c
        textView.insertionPointColor = NSColor(cgColor: Theme.color("accent")) ?? c
        textView.font = NativeTextArea.font
        var attrs = textView.typingAttributes
        attrs[.foregroundColor] = c
        attrs[.font] = NativeTextArea.font
        if let p = textView.defaultParagraphStyle { attrs[.paragraphStyle] = p }
        textView.typingAttributes = attrs
        if let storage = textView.textStorage, storage.length > 0 {
            storage.addAttribute(.foregroundColor, value: c, range: NSRange(location: 0, length: storage.length))
            if let p = textView.defaultParagraphStyle { storage.addAttribute(.paragraphStyle, value: p, range: NSRange(location: 0, length: storage.length)) }
        }
        textView.selectedTextAttributes = [.backgroundColor: NSColor(cgColor: Theme.color("focusRing")) ?? .selectedTextBackgroundColor]
    }

    /// 放进 TextAreaW 的框内（宿主坐标，左上原点）。
    func place(in r: CGRect) {
        scroll.frame = r.insetBy(dx: 1, dy: 1)
    }

    func scrollToEnd() {
        textView.scrollToEndOfDocument(nil)
    }

    func textDidChange(_ notification: Notification) { onChange?(textView.string) }
}

// MARK: - 图片

/// 把一张图等比缩放画进框内（可选棋盘格底、上对齐）。
final class ImageBoxW: Widget {
    var image: CGImage? { didSet { setNeedsDisplay() } }
    var checker = false
    var topAligned = false
    var border = true
    var radius: CGFloat = 6

    /// 图在框内实际占的区域。
    func imageRect() -> CGRect? {
        guard let img = image, img.width > 0, img.height > 0 else { return nil }
        let inner = frame.insetBy(dx: 1, dy: 1)
        let s = min(inner.width / CGFloat(img.width), inner.height / CGFloat(img.height))
        let w = CGFloat(img.width) * s, h = CGFloat(img.height) * s
        return CGRect(x: inner.midX - w / 2, y: topAligned ? inner.minY : inner.midY - h / 2, width: w, height: h)
    }

    override func draw(_ ctx: CGContext) {
        guard let img = image, let ir = imageRect() else { return }
        ctx.saveGState()
        ctx.addPath(Draw.rounded(ir, radius))
        ctx.clip()
        if checker { ImageBoxW.drawChecker(ctx, ir, cell: 8) }
        ctx.interpolationQuality = .high
        Draw.image(ctx, img, in: ir)
        ctx.restoreGState()
        if border { Draw.stroke(ctx, ir, Theme.color("border"), width: 1, radius: radius) }
    }

    static func drawChecker(_ ctx: CGContext, _ r: CGRect, cell: CGFloat) {
        let light = Theme.isDark ? CGColor(gray: 0.24, alpha: 1) : CGColor(gray: 1, alpha: 1)
        let dark = Theme.isDark ? CGColor(gray: 0.18, alpha: 1) : CGColor(gray: 0.9, alpha: 1)
        Draw.fill(ctx, r, light)
        ctx.setFillColor(dark)
        var y = r.minY
        var row = 0
        while y < r.maxY {
            var x = r.minX + (row % 2 == 0 ? 0 : cell)
            while x < r.maxX {
                ctx.fill(CGRect(x: x, y: y, width: min(cell, r.maxX - x), height: min(cell, r.maxY - y)))
                x += 2 * cell
            }
            y += cell
            row += 1
        }
    }
}

// MARK: - 状态行

/// 底部左侧的状态：可选图标 + 一行文字（危险色表示出错）。
final class StatusW: Widget {
    var text = "" { didSet { setNeedsDisplay() } }
    var icon: String?
    var danger = false

    override func preferredSize() -> CGSize {
        CGSize(width: Draw.textWidth(text, Fonts.ui(Fonts.body - 1)) + (icon != nil ? 22 : 0), height: 20)
    }

    override func draw(_ ctx: CGContext) {
        guard !text.isEmpty || icon != nil else { return }
        let color = danger ? Theme.color("danger") : Theme.color("textSecondary")
        var x = frame.minX
        if let i = icon {
            Icons.draw(ctx, i, in: CGRect(x: x, y: frame.midY - 8, width: 16, height: 16), color: color, lineWidth: 1.6)
            x += 22
        }
        Draw.text(ctx, text, font: Fonts.ui(Fonts.body - 1), color: color, in: CGRect(x: x, y: frame.minY, width: frame.maxX - x, height: frame.height))
    }
}

/// 小号分组标题（caption，textSecondary）。
final class CaptionW: Widget {
    var text: String
    init(_ text: String) { self.text = text }
    override func draw(_ ctx: CGContext) {
        Draw.text(ctx, text, font: Fonts.medium(Fonts.caption), color: Theme.color("textSecondary"), in: frame)
    }
}
