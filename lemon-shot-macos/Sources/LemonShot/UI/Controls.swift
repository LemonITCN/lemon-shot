import AppKit

/// 可交互控件基类。
class Control: Widget {
    override var isInteractive: Bool { true }
    var alpha: CGFloat { isEnabled ? 1 : 0.45 }
    var controlHeight: CGFloat { Theme.f("window.controlHeight") }
    var controlRadius: CGFloat { Theme.f("window.controlRadius") }

    func drawInputBox(_ ctx: CGContext, _ r: CGRect, focused: Bool = false, danger: Bool = false) {
        if focused {
            Draw.fill(ctx, r.insetBy(dx: -3, dy: -3), Theme.color("focusRing"), radius: controlRadius + 3)
        }
        Draw.fill(ctx, r, Theme.color("inputBg"), radius: controlRadius)
        Draw.stroke(ctx, r, danger ? Theme.color("danger") : (focused ? Theme.color("accent") : Theme.color("inputBorder")), width: 1, radius: controlRadius)
    }
}

// MARK: - 标签

final class Label: Widget {
    var text: String { didSet { setNeedsDisplay() } }
    var font: NSFont
    var colorKey: String
    var color: CGColor?
    var align: Draw.HAlign = .left
    var wraps = false

    init(_ text: String, font: NSFont = Fonts.ui(Fonts.body), colorKey: String = "text") {
        self.text = text
        self.font = font
        self.colorKey = colorKey
    }

    override func preferredSize() -> CGSize {
        CGSize(width: Draw.textWidth(text, font), height: Draw.lineHeight(font))
    }

    override func draw(_ ctx: CGContext) {
        let c = color ?? Theme.color(colorKey)
        if wraps {
            Draw.wrapped(ctx, text, font: font, color: c, x: frame.minX, y: frame.minY, width: frame.width, align: align)
        } else {
            Draw.text(ctx, text, font: font, color: c, in: frame, align: align)
        }
    }
}

// MARK: - 按钮

final class ButtonW: Control {
    enum Kind { case normal, primary, danger }
    var title: String { didSet { setNeedsDisplay() } }
    var kind: Kind
    var icon: String?
    var action: (() -> Void)?

    init(_ title: String, kind: Kind = .normal, icon: String? = nil, action: (() -> Void)? = nil) {
        self.title = title
        self.kind = kind
        self.icon = icon
        self.action = action
    }

    var font: NSFont { Fonts.medium(Fonts.body) }

    override func preferredSize() -> CGSize {
        var w = Draw.textWidth(title, font) + 2 * Theme.f("window.buttonPaddingX")
        if icon != nil { w += 16 + 6 }
        return CGSize(width: ceil(w), height: controlHeight)
    }

    override func draw(_ ctx: CGContext) {
        let r = frame
        ctx.saveGState()
        ctx.setAlpha(alpha)
        let textColor: CGColor
        switch kind {
        case .primary:
            Draw.fill(ctx, r, Theme.color(hovered || pressed ? "accentHover" : "accent"), radius: controlRadius)
            textColor = Theme.color("accentText")
        case .danger, .normal:
            let bg = pressed ? Theme.color("surfacePressed") : (hovered ? Theme.color("surfaceHover") : Theme.color("surface"))
            Draw.fill(ctx, r, bg, radius: controlRadius)
            Draw.stroke(ctx, r, Theme.color("inputBorder"), width: 1, radius: controlRadius)
            textColor = kind == .danger ? Theme.color("danger") : Theme.color("text")
        }
        let tw = Draw.textWidth(title, font)
        var x = r.midX - tw / 2
        if let icon = icon {
            let total = tw + 22
            x = r.midX - total / 2
            Icons.draw(ctx, icon, in: CGRect(x: x, y: r.midY - 8, width: 16, height: 16), color: textColor, lineWidth: 1.6)
            x += 22
        }
        Draw.text(ctx, title, font: font, color: textColor, in: CGRect(x: x, y: r.minY, width: tw + 1, height: r.height), truncate: false)
        ctx.restoreGState()
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        if inside { action?() }
    }
}

/// 图标按钮（工具栏、样式面板、贴图按钮）。custom 可替代图标画预览。
final class IconButton: Control {
    var icon: String?
    var selected = false { didSet { if oldValue != selected { setNeedsDisplay() } } }
    var iconSize: CGFloat
    var iconColor: CGColor?
    var fillIcon = false
    var radius: CGFloat
    var lineWidth: CGFloat?
    var custom: ((CGContext, CGRect, CGColor) -> Void)?
    var action: (() -> Void)?
    var onHoverChange: ((Bool) -> Void)?
    /// 深色底（贴图上的小按钮）。
    var onDark = false

    init(icon: String?, size: CGFloat, iconSize: CGFloat, radius: CGFloat = Theme.f("toolbar.buttonRadius"), action: (() -> Void)? = nil) {
        self.icon = icon
        self.iconSize = iconSize
        self.radius = radius
        self.action = action
        super.init()
        frame.size = CGSize(width: size, height: size)
    }

    var foreground: CGColor {
        if !isEnabled { return onDark ? CGColor(gray: 1, alpha: 0.35) : Theme.color("textTertiary") }
        if selected { return Theme.color("iconActive") }
        if let c = iconColor { return c }
        return onDark ? CGColor(gray: 1, alpha: 1) : Theme.color("icon")
    }

    override func draw(_ ctx: CGContext) {
        let r = frame
        if isEnabled {
            if selected {
                Draw.fill(ctx, r, Theme.color("surfaceSelected"), radius: radius)
            } else if pressed {
                Draw.fill(ctx, r, onDark ? CGColor(gray: 1, alpha: 0.25) : Theme.color("surfacePressed"), radius: radius)
            } else if hovered {
                // 深色底悬停：白色 15%（spec 3 贴图悬停按钮）
                Draw.fill(ctx, r, onDark ? CGColor(gray: 1, alpha: 0.15) : Theme.color("surfaceHover"), radius: radius)
            }
        }
        let fg = foreground
        let ir = CGRect(x: r.midX - iconSize / 2, y: r.midY - iconSize / 2, width: iconSize, height: iconSize)
        if let custom = custom {
            custom(ctx, r, fg)
        } else if let icon = icon {
            Icons.draw(ctx, icon, in: ir, color: fg, lineWidth: lineWidth, fill: fillIcon ? fg : nil)
        }
    }

    override func mouseEntered() { super.mouseEntered(); onHoverChange?(true) }
    override func mouseExited() { super.mouseExited(); onHoverChange?(false) }
    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) { if inside { action?() } }
}

// MARK: - 开关

final class SwitchW: Control {
    var isOn: Bool { didSet { setNeedsDisplay() } }
    var onChange: ((Bool) -> Void)?

    init(_ on: Bool, onChange: ((Bool) -> Void)? = nil) {
        isOn = on
        self.onChange = onChange
        super.init()
        frame.size = CGSize(width: Theme.f("window.switchWidth"), height: Theme.f("window.switchHeight"))
    }

    override func preferredSize() -> CGSize { CGSize(width: Theme.f("window.switchWidth"), height: Theme.f("window.switchHeight")) }

    override func draw(_ ctx: CGContext) {
        let r = CGRect(origin: CGPoint(x: frame.minX, y: frame.midY - Theme.f("window.switchHeight") / 2), size: preferredSize())
        ctx.saveGState()
        ctx.setAlpha(alpha)
        Draw.fill(ctx, r, isOn ? Theme.color("accent") : Theme.color("controlTrack"), radius: r.height / 2)
        let d = r.height - 4
        let kx = isOn ? r.maxX - 2 - d : r.minX + 2
        let knob = CGRect(x: kx, y: r.minY + 2, width: d, height: d)
        ctx.saveGState()
        Draw.setShadow(ctx, offsetY: 1, blur: 2, color: CGColor(gray: 0, alpha: 0.25))
        Draw.ellipse(ctx, knob, fill: Theme.color("controlKnob"))
        ctx.restoreGState()
        ctx.restoreGState()
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside else { return }
        isOn.toggle()
        onChange?(isOn)
    }
}

// MARK: - 分段选择

final class SegmentedW: Control {
    var items: [String]
    var icons: [String]?
    var selectedIndex: Int { didSet { setNeedsDisplay() } }
    var onChange: ((Int) -> Void)?
    var segmentWidth: CGFloat?
    private var hoverIndex = -1

    init(_ items: [String], selected: Int, onChange: ((Int) -> Void)? = nil) {
        self.items = items
        selectedIndex = selected
        self.onChange = onChange
    }

    var font: NSFont { Fonts.ui(Fonts.body) }

    override func preferredSize() -> CGSize {
        let w = segmentWidth ?? (items.map { Draw.textWidth($0, font) }.max() ?? 40) + 24
        return CGSize(width: ceil(w * CGFloat(items.count) + 4), height: controlHeight)
    }

    private func segRect(_ i: Int) -> CGRect {
        let inner = frame.insetBy(dx: 2, dy: 2)
        let w = inner.width / CGFloat(max(1, items.count))
        return CGRect(x: inner.minX + w * CGFloat(i), y: inner.minY, width: w, height: inner.height)
    }

    override func draw(_ ctx: CGContext) {
        ctx.saveGState()
        ctx.setAlpha(alpha)
        Draw.fill(ctx, frame, Theme.color("controlTrack"), radius: controlRadius)
        for i in items.indices {
            let r = segRect(i)
            if i == selectedIndex {
                ctx.saveGState()
                Draw.setShadow(ctx, offsetY: 1, blur: 2, color: CGColor(gray: 0, alpha: 0.18))
                Draw.fill(ctx, r, Theme.isDark ? Theme.color("surfacePressed") : Theme.color("surface"), radius: controlRadius - 1)
                ctx.restoreGState()
            } else if i == hoverIndex && isEnabled {
                Draw.fill(ctx, r, Theme.color("surfaceHover").alpha(0.6), radius: controlRadius - 1)
            }
            let color = i == selectedIndex ? Theme.color("text") : Theme.color("textSecondary")
            if let icons = icons, i < icons.count {
                Icons.draw(ctx, icons[i], in: CGRect(x: r.midX - 8, y: r.midY - 8, width: 16, height: 16), color: color)
            } else {
                Draw.text(ctx, items[i], font: i == selectedIndex ? Fonts.medium(Fonts.body) : font, color: color, in: r.insetBy(dx: 4, dy: 0), align: .center)
            }
        }
        ctx.restoreGState()
    }

    override func mouseMoved(_ p: CGPoint) {
        let idx = items.indices.first { segRect($0).contains(p) } ?? -1
        if idx != hoverIndex { hoverIndex = idx; setNeedsDisplay() }
    }

    override func mouseExited() { hoverIndex = -1; super.mouseExited() }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside, let idx = items.indices.first(where: { segRect($0).contains(p) }), idx != selectedIndex else { return }
        selectedIndex = idx
        onChange?(idx)
    }
}

// MARK: - 下拉

struct Option {
    var value: String
    var label: String
    var enabled = true
    init(_ value: String, _ label: String, enabled: Bool = true) { self.value = value; self.label = label; self.enabled = enabled }
}

final class DropdownW: Control {
    var options: [Option] { didSet { setNeedsDisplay() } }
    var value: String { didSet { setNeedsDisplay() } }
    var onChange: ((String) -> Void)?
    private var popup: PopupPanel?
    private var closedAt: TimeInterval = 0

    init(_ options: [Option], value: String, width: CGFloat = 200, onChange: ((String) -> Void)? = nil) {
        self.options = options
        self.value = value
        self.onChange = onChange
        super.init()
        frame.size = CGSize(width: width, height: Theme.f("window.controlHeight"))
    }

    var label: String { options.first { $0.value == value }?.label ?? value }

    override func draw(_ ctx: CGContext) {
        let r = frame
        ctx.saveGState()
        ctx.setAlpha(alpha)
        drawInputBox(ctx, r, focused: popup != nil)
        if hovered && popup == nil { Draw.fill(ctx, r.insetBy(dx: 1, dy: 1), Theme.color("surfaceHover").alpha(0.5), radius: controlRadius - 1) }
        Draw.text(ctx, label, font: Fonts.ui(Fonts.body), color: Theme.color("text"), in: CGRect(x: r.minX + 10, y: r.minY, width: r.width - 36, height: r.height))
        Icons.draw(ctx, "chevron-down", in: CGRect(x: r.maxX - 24, y: r.midY - 7, width: 14, height: 14), color: Theme.color("textSecondary"), lineWidth: 1.6)
        ctx.restoreGState()
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside, popup == nil, Date().timeIntervalSince1970 - closedAt > 0.25, let anchor = screenRect() else { return }
        let list = MenuListWidget(options: options, selected: value, minWidth: frame.width)
        let panel = PopupPanel(content: list, size: list.contentSize(), interactive: true)
        list.onPick = { [weak self, weak panel] v in
            panel?.close()
            guard let self = self else { return }
            if v != self.value { self.value = v; self.onChange?(v) }
        }
        panel.onClose = { [weak self] in
            self?.popup = nil
            self?.closedAt = Date().timeIntervalSince1970
            self?.setNeedsDisplay()
        }
        popup = panel
        panel.show(below: anchor, gap: 4)
        setNeedsDisplay()
    }
}

/// 下拉菜单列表。
final class MenuListWidget: Widget {
    let options: [Option]
    let selected: String
    let minWidth: CGFloat
    var onPick: ((String) -> Void)?
    private var hoverIndex = -1
    static let rowH: CGFloat = 28
    static let pad: CGFloat = 5

    init(options: [Option], selected: String, minWidth: CGFloat) {
        self.options = options
        self.selected = selected
        self.minWidth = minWidth
    }

    override var isInteractive: Bool { true }

    func contentSize() -> CGSize {
        let tw = options.map { Draw.textWidth($0.label, Fonts.ui(Fonts.body)) }.max() ?? 80
        return CGSize(width: max(minWidth, tw + 52), height: CGFloat(options.count) * MenuListWidget.rowH + 2 * MenuListWidget.pad)
    }

    private func rowRect(_ i: Int) -> CGRect {
        CGRect(x: frame.minX + MenuListWidget.pad, y: frame.minY + MenuListWidget.pad + CGFloat(i) * MenuListWidget.rowH, width: frame.width - 2 * MenuListWidget.pad, height: MenuListWidget.rowH)
    }

    override func draw(_ ctx: CGContext) {
        Draw.panel(ctx, frame, radius: 8, fill: Theme.color("popoverBg"), border: Theme.color("border"), shadow: Theme.color("shadow"), blur: Theme.f("toolbar.shadowBlur"), offsetY: Theme.f("toolbar.shadowOffsetY"))
        for (i, o) in options.enumerated() {
            let r = rowRect(i)
            let hot = i == hoverIndex && o.enabled
            if hot { Draw.fill(ctx, r, Theme.color("accent"), radius: 5) }
            let tc = !o.enabled ? Theme.color("textTertiary") : (hot ? Theme.color("accentText") : Theme.color("text"))
            if o.value == selected {
                Icons.draw(ctx, "check", in: CGRect(x: r.minX + 8, y: r.midY - 7, width: 14, height: 14), color: hot ? Theme.color("accentText") : Theme.color("accent"), lineWidth: 2)
            }
            Draw.text(ctx, o.label, font: Fonts.ui(Fonts.body), color: tc, in: CGRect(x: r.minX + 30, y: r.minY, width: r.width - 38, height: r.height))
        }
    }

    override func mouseMoved(_ p: CGPoint) {
        let idx = options.indices.first { rowRect($0).contains(p) } ?? -1
        if idx != hoverIndex { hoverIndex = idx; setNeedsDisplay() }
    }

    override func mouseExited() { hoverIndex = -1; setNeedsDisplay() }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard let idx = options.indices.first(where: { rowRect($0).contains(p) }), options[idx].enabled else { return }
        onPick?(options[idx].value)
    }
}

// MARK: - 滑块

final class SliderW: Control {
    var minValue: Double
    var maxValue: Double
    var step: Double
    var value: Double { didSet { setNeedsDisplay() } }
    var format: (Double) -> String
    var onChange: ((Double) -> Void)?
    var valueWidth: CGFloat = 52
    var trackWidth: CGFloat

    init(min: Double, max: Double, step: Double, value: Double, trackWidth: CGFloat = 180, format: @escaping (Double) -> String, onChange: ((Double) -> Void)? = nil) {
        minValue = min
        maxValue = max
        self.step = step
        self.value = value
        self.format = format
        self.onChange = onChange
        self.trackWidth = trackWidth
        super.init()
        frame.size = CGSize(width: trackWidth + 10 + valueWidth, height: Theme.f("window.controlHeight"))
    }

    override func preferredSize() -> CGSize { CGSize(width: trackWidth + 10 + valueWidth, height: controlHeight) }

    private var trackRect: CGRect { CGRect(x: frame.minX + 8, y: frame.midY - 2, width: trackWidth - 16, height: 4) }

    override func draw(_ ctx: CGContext) {
        ctx.saveGState()
        ctx.setAlpha(alpha)
        let t = trackRect
        Draw.fill(ctx, t, Theme.color("controlTrack"), radius: 2)
        let f = CGFloat((min(max(value, minValue), maxValue) - minValue) / max(0.0001, maxValue - minValue))
        Draw.fill(ctx, CGRect(x: t.minX, y: t.minY, width: t.width * f, height: t.height), Theme.color("accent"), radius: 2)
        let kd: CGFloat = 16
        let knob = CGRect(x: t.minX + t.width * f - kd / 2, y: t.midY - kd / 2, width: kd, height: kd)
        ctx.saveGState()
        Draw.setShadow(ctx, offsetY: 1, blur: 3, color: CGColor(gray: 0, alpha: 0.25))
        Draw.ellipse(ctx, knob, fill: Theme.color("controlKnob"))
        ctx.restoreGState()
        Draw.ellipse(ctx, knob, stroke: Theme.color("inputBorder"), width: 0.5)
        Draw.text(ctx, format(value), font: Fonts.digits(Fonts.body - 1), color: Theme.color("textSecondary"),
                  in: CGRect(x: frame.minX + trackWidth + 10, y: frame.minY, width: valueWidth, height: frame.height), align: .left)
        ctx.restoreGState()
    }

    private func setFrom(_ p: CGPoint) {
        let t = trackRect
        let f = Double(min(max((p.x - t.minX) / max(1, t.width), 0), 1))
        var v = minValue + f * (maxValue - minValue)
        if step > 0 { v = (v / step).rounded() * step }
        v = min(max(v, minValue), maxValue)
        if abs(v - value) > 1e-9 { value = v; onChange?(v) }
    }

    override func mouseDown(_ p: CGPoint, event: NSEvent?) { setFrom(p) }
    override func mouseDragged(_ p: CGPoint, event: NSEvent?) { setFrom(p) }

    override func scrollWheel(_ event: NSEvent) -> Bool {
        let d = event.scrollingDeltaY != 0 ? event.scrollingDeltaY : event.scrollingDeltaX
        guard d != 0 else { return true }
        let v = min(max(value + (d > 0 ? step : -step), minValue), maxValue)
        if v != value { value = v; onChange?(v) }
        return true
    }
}

// MARK: - 数字步进

final class StepperW: Control {
    var minValue: Double
    var maxValue: Double
    var step: Double
    var value: Double { didSet { setNeedsDisplay() } }
    var format: (Double) -> String
    var onChange: ((Double) -> Void)?
    private var hotPart = 0 // -1 减，1 加
    private var editing = false

    init(min: Double, max: Double, step: Double, value: Double, width: CGFloat = 112, format: @escaping (Double) -> String = { String(Int($0)) }, onChange: ((Double) -> Void)? = nil) {
        minValue = min
        maxValue = max
        self.step = step
        self.value = value
        self.format = format
        self.onChange = onChange
        super.init()
        frame.size = CGSize(width: width, height: Theme.f("window.controlHeight"))
    }

    private var minusRect: CGRect { CGRect(x: frame.minX, y: frame.minY, width: 28, height: frame.height) }
    private var plusRect: CGRect { CGRect(x: frame.maxX - 28, y: frame.minY, width: 28, height: frame.height) }
    private var valueRect: CGRect { CGRect(x: frame.minX + 28, y: frame.minY, width: frame.width - 56, height: frame.height) }

    override func draw(_ ctx: CGContext) {
        ctx.saveGState()
        ctx.setAlpha(alpha)
        drawInputBox(ctx, frame, focused: editing)
        for (part, r, icon) in [(-1, minusRect, "minus"), (1, plusRect, "plus")] {
            if hotPart == part && hovered && isEnabled { Draw.fill(ctx, r.insetBy(dx: 2, dy: 2), pressed ? Theme.color("surfacePressed") : Theme.color("surfaceHover"), radius: controlRadius - 2) }
            let disabled = (part < 0 && value <= minValue) || (part > 0 && value >= maxValue)
            Icons.draw(ctx, icon, in: CGRect(x: r.midX - 7, y: r.midY - 7, width: 14, height: 14), color: Theme.color(disabled ? "textTertiary" : "icon"), lineWidth: 1.75)
        }
        ctx.setStrokeColor(Theme.color("divider"))
        ctx.setLineWidth(1)
        ctx.strokeLineSegments(between: [CGPoint(x: minusRect.maxX + 0.5, y: frame.minY + 6), CGPoint(x: minusRect.maxX + 0.5, y: frame.maxY - 6),
                                         CGPoint(x: plusRect.minX - 0.5, y: frame.minY + 6), CGPoint(x: plusRect.minX - 0.5, y: frame.maxY - 6)])
        if !editing {
            Draw.text(ctx, format(value), font: Fonts.digits(Fonts.body), color: Theme.color("text"), in: valueRect, align: .center)
        }
        ctx.restoreGState()
    }

    override func mouseMoved(_ p: CGPoint) {
        let part = minusRect.contains(p) ? -1 : (plusRect.contains(p) ? 1 : 0)
        if part != hotPart { hotPart = part; setNeedsDisplay() }
    }

    override func mouseDown(_ p: CGPoint, event: NSEvent?) { mouseMoved(p) }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside else { return }
        if minusRect.contains(p) { bump(-step) } else if plusRect.contains(p) { bump(step) } else { beginEditing() }
    }

    func bump(_ d: Double) {
        let v = min(max(value + d, minValue), maxValue)
        if v != value { value = v; onChange?(v) }
    }

    override func scrollWheel(_ event: NSEvent) -> Bool {
        let d = event.scrollingDeltaY
        if d != 0 { bump(d > 0 ? step : -step) }
        return true
    }

    private func beginEditing() {
        guard let host = host else { return }
        editing = true
        setNeedsDisplay()
        NativeEditor.begin(in: host, rect: valueRect.insetBy(dx: 2, dy: 0), text: String(Int(value)), font: Fonts.digits(Fonts.body), secure: false, align: .center, onChange: { _ in }, onEnd: { [weak self] s in
            guard let self = self else { return }
            self.editing = false
            if let v = Double(s.trimmingCharacters(in: .whitespaces)) {
                let c = min(max(v, self.minValue), self.maxValue)
                if c != self.value { self.value = c; self.onChange?(c) }
            }
            self.setNeedsDisplay()
        })
    }
}

// MARK: - 文本框 / 密码框

final class TextFieldW: Control {
    var text: String { didSet { setNeedsDisplay() } }
    var placeholder: String
    var secure: Bool
    var onChange: ((String) -> Void)?
    private var editing = false

    init(_ text: String, placeholder: String = "", secure: Bool = false, width: CGFloat = 260, onChange: ((String) -> Void)? = nil) {
        self.text = text
        self.placeholder = placeholder
        self.secure = secure
        self.onChange = onChange
        super.init()
        frame.size = CGSize(width: width, height: Theme.f("window.controlHeight"))
        hoverCursor = .iBeam
    }

    private var textRect: CGRect { frame.insetBy(dx: 8, dy: 0) }

    override func draw(_ ctx: CGContext) {
        ctx.saveGState()
        ctx.setAlpha(alpha)
        drawInputBox(ctx, frame, focused: editing)
        if !editing {
            if text.isEmpty {
                Draw.text(ctx, placeholder, font: Fonts.ui(Fonts.body), color: Theme.color("textTertiary"), in: textRect)
            } else {
                let shown = secure ? String(repeating: "•", count: min(text.count, 24)) : text
                Draw.text(ctx, shown, font: Fonts.ui(Fonts.body), color: Theme.color("text"), in: textRect)
            }
        }
        ctx.restoreGState()
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside, !editing, let host = host else { return }
        editing = true
        setNeedsDisplay()
        NativeEditor.begin(in: host, rect: textRect, text: text, font: Fonts.ui(Fonts.body), secure: secure, align: .left, onChange: { [weak self] s in
            self?.text = s
            self?.onChange?(s)
        }, onEnd: { [weak self] s in
            guard let self = self else { return }
            self.editing = false
            if s != self.text { self.text = s; self.onChange?(s) }
            self.setNeedsDisplay()
        })
    }
}

/// 自绘文本框编辑时叠一个原生输入框（支持输入法），结束后移除。
enum NativeEditor {
    private final class Delegate: NSObject, NSTextFieldDelegate {
        var onChange: (String) -> Void
        var onEnd: (String) -> Void
        weak var field: NSTextField?
        var ended = false
        init(onChange: @escaping (String) -> Void, onEnd: @escaping (String) -> Void) { self.onChange = onChange; self.onEnd = onEnd }
        func controlTextDidChange(_ obj: Notification) { if let f = field { onChange(f.stringValue) } }
        func controlTextDidEndEditing(_ obj: Notification) { finish() }
        func control(_ control: NSControl, textView: NSTextView, doCommandBy sel: Selector) -> Bool {
            if sel == #selector(NSResponder.cancelOperation(_:)) || sel == #selector(NSResponder.insertNewline(_:)) {
                let win = control.window
                finish()
                // 键盘还给窗口的内容视图（截图层），不接收的就交给窗口
                if let cv = win?.contentView, cv.acceptsFirstResponder { win?.makeFirstResponder(cv) } else { win?.makeFirstResponder(nil) }
                return true
            }
            return false
        }
        func finish() {
            guard !ended, let f = field else { return }
            ended = true
            let s = f.stringValue
            f.removeFromSuperview()
            NativeEditor.current = nil
            onEnd(s)
        }
    }

    private static var current: Delegate?

    static func end() { current?.finish() }

    static func begin(in host: NSView, rect: CGRect, text: String, font: NSFont, secure: Bool, align: Draw.HAlign, onChange: @escaping (String) -> Void, onEnd: @escaping (String) -> Void) {
        end()
        let f: NSTextField = secure ? NSSecureTextField(frame: .zero) : NSTextField(frame: .zero)
        f.isBordered = false
        f.drawsBackground = false
        f.focusRingType = .none
        f.font = font
        f.textColor = NSColor(cgColor: Theme.color("text"))
        f.stringValue = text
        f.alignment = align == .center ? .center : .left
        f.cell?.usesSingleLineMode = true
        f.cell?.isScrollable = true
        f.cell?.wraps = false
        let h = ceil(font.ascender - font.descender) + 4
        // NSTextField 单元格左右各有 2pt 内边距
        f.frame = CGRect(x: rect.minX - 2, y: rect.midY - h / 2, width: rect.width + 4, height: h)
        let d = Delegate(onChange: onChange, onEnd: onEnd)
        d.field = f
        f.delegate = d
        current = d
        host.addSubview(f)
        host.window?.makeFirstResponder(f)
        f.currentEditor()?.selectAll(nil)
    }
}

// MARK: - 说明图标

final class InfoW: Control {
    var text: String
    private var bubble: PopupPanel?

    init(_ text: String) {
        self.text = text
        super.init()
        frame.size = CGSize(width: 18, height: 18)
    }

    override func draw(_ ctx: CGContext) {
        let r = CGRect(x: frame.midX - 8, y: frame.midY - 8, width: 16, height: 16)
        Icons.draw(ctx, "info", in: r, color: hovered ? Theme.color("text") : Theme.color("textSecondary"), lineWidth: 1.6)
    }

    override func mouseEntered() {
        super.mouseEntered()
        show()
    }

    override func mouseExited() {
        super.mouseExited()
        bubble?.close()
        bubble = nil
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) { if inside, bubble == nil { show() } }

    private func show() {
        guard bubble == nil, let r = screenRect() else { return }
        bubble = Bubble.showInfo(text, anchor: r)
        bubble?.onClose = { [weak self] in self?.bubble = nil }
    }
}

// MARK: - 颜色块

final class ColorSwatchW: Control {
    var hex: String { didSet { setNeedsDisplay() } }
    var allowsAlpha: Bool
    var onChange: ((String) -> Void)?
    private var popup: PopupPanel?

    init(_ hex: String, allowsAlpha: Bool = false, onChange: ((String) -> Void)? = nil) {
        self.hex = hex
        self.allowsAlpha = allowsAlpha
        self.onChange = onChange
        super.init()
        frame.size = CGSize(width: 44, height: Theme.f("window.controlHeight"))
    }

    /// 整块圆角矩形色块（契约 11 节）：颜色铺满控件，1pt 细边，悬停时细边加深；带透明度时底下垫棋盘格。
    override func draw(_ ctx: CGContext) {
        ctx.saveGState()
        ctx.setAlpha(alpha)
        let radius = Theme.f("window.controlRadius")
        let r = frame.insetBy(dx: 0.5, dy: 0.5)
        ColorSwatchW.drawChecker(ctx, r, radius: radius)
        Draw.fill(ctx, r, Theme.hex(hex), radius: radius)
        Draw.stroke(ctx, r, Theme.color(hovered ? "textTertiary" : "inputBorder"), width: 1, radius: radius)
        ctx.restoreGState()
    }

    static func drawChecker(_ ctx: CGContext, _ r: CGRect, radius: CGFloat) {
        ctx.saveGState()
        ctx.addPath(CGPath(roundedRect: r, cornerWidth: radius, cornerHeight: radius, transform: nil))
        ctx.clip()
        Draw.fill(ctx, r, CGColor(gray: 1, alpha: 1))
        let s: CGFloat = 5
        ctx.setFillColor(CGColor(gray: 0.82, alpha: 1))
        var y = r.minY
        var row = 0
        while y < r.maxY {
            var x = r.minX + (row % 2 == 0 ? 0 : s)
            while x < r.maxX { ctx.fill(CGRect(x: x, y: y, width: s, height: s)); x += 2 * s }
            y += s
            row += 1
        }
        ctx.restoreGState()
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside, popup == nil, let anchor = screenRect() else { return }
        let pal = PaletteWidget(selected: hex)
        let panel = PopupPanel(content: pal, size: pal.contentSize(), interactive: true)
        pal.onPick = { [weak self, weak panel] h in
            panel?.close()
            guard let self = self else { return }
            if let h = h {
                let v = self.allowsAlpha ? h : String(h.prefix(7))
                self.hex = v
                self.onChange?(v)
            } else {
                ColorPanelBridge.shared.open(initial: self.hex, alpha: self.allowsAlpha) { [weak self] v in
                    self?.hex = v
                    self?.onChange?(v)
                }
            }
        }
        panel.onClose = { [weak self] in self?.popup = nil; self?.setNeedsDisplay() }
        popup = panel
        panel.show(below: anchor, gap: 4)
    }
}

/// 调色板气泡：10 个标注色 + 自定义。
final class PaletteWidget: Widget {
    let selected: String
    var onPick: ((String?) -> Void)?
    private var hover = -1

    init(selected: String) { self.selected = selected }
    override var isInteractive: Bool { true }

    var colors: [String] { AnnotStyles.palette() }
    var sw: CGFloat { Theme.f("popover.swatch") }
    var gap: CGFloat { Theme.f("popover.swatchGap") }
    var pad: CGFloat { Theme.f("popover.padding") }

    func contentSize() -> CGSize {
        let w = CGFloat(colors.count) * sw + CGFloat(colors.count - 1) * gap + 2 * pad
        return CGSize(width: w, height: 2 * pad + sw + 8 + 24)
    }

    private func swRect(_ i: Int) -> CGRect {
        CGRect(x: frame.minX + pad + CGFloat(i) * (sw + gap), y: frame.minY + pad, width: sw, height: sw)
    }

    private var customRect: CGRect { CGRect(x: frame.minX + pad, y: frame.minY + pad + sw + 8, width: frame.width - 2 * pad, height: 24) }

    override func draw(_ ctx: CGContext) {
        Draw.panel(ctx, frame, radius: Theme.f("popover.radius"), fill: Theme.color("popoverBg"), border: Theme.color("border"), shadow: Theme.color("shadow"), blur: Theme.f("toolbar.shadowBlur"), offsetY: Theme.f("toolbar.shadowOffsetY"))
        for (i, c) in colors.enumerated() {
            let r = swRect(i)
            if c.uppercased() == String(selected.prefix(7)).uppercased() {
                Draw.ellipse(ctx, r.insetBy(dx: -3, dy: -3), stroke: Theme.color("accent"), width: 2)
            } else if hover == i {
                Draw.ellipse(ctx, r.insetBy(dx: -3, dy: -3), stroke: Theme.color("border"), width: 2)
            }
            Draw.ellipse(ctx, r, fill: Theme.hex(c))
            Draw.ellipse(ctx, r, stroke: CGColor(gray: 0, alpha: 0.12), width: 1)
        }
        let cr = customRect
        if hover == 100 { Draw.fill(ctx, cr, Theme.color("surfaceHover"), radius: 5) }
        Icons.draw(ctx, "palette", in: CGRect(x: cr.minX + 4, y: cr.midY - 7, width: 14, height: 14), color: Theme.color("icon"), lineWidth: 1.6)
        Draw.text(ctx, L("common.custom"), font: Fonts.ui(Fonts.body - 1), color: Theme.color("text"), in: CGRect(x: cr.minX + 24, y: cr.minY, width: cr.width - 28, height: cr.height))
    }

    override func mouseMoved(_ p: CGPoint) {
        var h = colors.indices.first { swRect($0).insetBy(dx: -3, dy: -3).contains(p) } ?? -1
        if customRect.contains(p) { h = 100 }
        if h != hover { hover = h; setNeedsDisplay() }
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        if let i = colors.indices.first(where: { swRect($0).insetBy(dx: -3, dy: -3).contains(p) }) { onPick?(colors[i]) } else if customRect.contains(p) { onPick?(nil) }
    }
}

/// 系统取色面板。
final class ColorPanelBridge: NSObject {
    static let shared = ColorPanelBridge()
    private var callback: ((String) -> Void)?
    private var alpha = false

    func open(initial: String, alpha: Bool, onChange: @escaping (String) -> Void) {
        callback = onChange
        self.alpha = alpha
        let p = NSColorPanel.shared
        p.showsAlpha = alpha
        p.setTarget(self)
        p.setAction(#selector(changed(_:)))
        p.color = NSColor(cgColor: Theme.hex(initial)) ?? .red
        p.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 1)
        NSApp.activate(ignoringOtherApps: true)
        p.orderFrontRegardless()
        p.makeKey()
    }

    @objc private func changed(_ sender: NSColorPanel) {
        guard let c = sender.color.usingColorSpace(.sRGB) else { return }
        func b(_ v: CGFloat) -> Int { Int(max(0, min(255, (v * 255).rounded()))) }
        var s = String(format: "#%02X%02X%02X", b(c.redComponent), b(c.greenComponent), b(c.blueComponent))
        if alpha { s += String(format: "%02X", b(c.alphaComponent)) }
        callback?(s)
    }

    func detach() {
        NSColorPanel.shared.setTarget(nil)
        callback = nil
    }
}

// MARK: - 路径

final class PathW: Control {
    var path: String { didSet { setNeedsDisplay() } }
    var onChange: ((String) -> Void)?
    let button: ButtonW

    init(_ path: String, width: CGFloat = 300, onChange: ((String) -> Void)? = nil) {
        self.path = path
        self.onChange = onChange
        button = ButtonW(L("common.choose"))
        super.init()
        frame.size = CGSize(width: width, height: Theme.f("window.controlHeight"))
        add(button)
        button.action = { [weak self] in self?.choose() }
    }

    override func layout() {
        let bs = button.preferredSize()
        button.frame = CGRect(x: frame.maxX - bs.width, y: frame.minY, width: bs.width, height: bs.height)
    }

    var boxRect: CGRect { CGRect(x: frame.minX, y: frame.minY, width: frame.width - button.preferredSize().width - 8, height: frame.height) }

    static func shortName(_ raw: String) -> String {
        let url = Settings.resolvePath(raw)
        let comps = url.pathComponents.filter { $0 != "/" }
        return comps.suffix(2).joined(separator: "/")
    }

    override func draw(_ ctx: CGContext) {
        let r = boxRect
        drawInputBox(ctx, r)
        Icons.draw(ctx, "folder-open", in: CGRect(x: r.minX + 8, y: r.midY - 7, width: 14, height: 14), color: Theme.color("textSecondary"), lineWidth: 1.6)
        Draw.text(ctx, PathW.shortName(path), font: Fonts.ui(Fonts.body), color: Theme.color("text"), in: CGRect(x: r.minX + 28, y: r.minY, width: r.width - 36, height: r.height))
        drawChildren(ctx)
    }

    override func hitTest(_ p: CGPoint) -> Widget? {
        if button.frame.contains(p) { return button }
        return nil
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.directoryURL = Settings.resolvePath(path)
        NSApp.activate(ignoringOtherApps: true)
        if panel.runModal() == .OK, let url = panel.url {
            let v = Settings.abbreviatePath(url.path)
            path = v
            onChange?(v)
        }
    }
}

// MARK: - 快捷键录入

final class HotkeyW: Control, FlagsObserver {
    var shortcut: String { didSet { setNeedsDisplay() } }
    var conflict = false { didSet { setNeedsDisplay() } }
    /// 截图时单键：允许不带修饰键。
    var singleKey: Bool
    var onChange: ((String) -> Void)?
    private var recording = false
    private var liveMods: NSEvent.ModifierFlags = []
    private var invalid = false

    init(_ shortcut: String, singleKey: Bool = false, width: CGFloat = 170, onChange: ((String) -> Void)? = nil) {
        self.shortcut = shortcut
        self.singleKey = singleKey
        self.onChange = onChange
        super.init()
        frame.size = CGSize(width: width, height: Theme.f("window.controlHeight"))
    }

    override var acceptsFocus: Bool { true }

    private var clearRect: CGRect { CGRect(x: frame.maxX - 26, y: frame.midY - 10, width: 20, height: 20) }

    override func draw(_ ctx: CGContext) {
        ctx.saveGState()
        ctx.setAlpha(alpha)
        drawInputBox(ctx, frame, focused: recording, danger: conflict && !recording)
        let tr = CGRect(x: frame.minX + 10, y: frame.minY, width: frame.width - 40, height: frame.height)
        if recording {
            let live = Shortcut.display(mods: liveMods, key: nil)
            if live.isEmpty {
                Draw.text(ctx, invalid ? L("pref.shortcuts.invalid") : L("pref.shortcuts.record"), font: Fonts.ui(Fonts.body - 1), color: invalid ? Theme.color("danger") : Theme.color("textSecondary"), in: tr)
            } else {
                Draw.text(ctx, live, font: Fonts.ui(Fonts.body), color: Theme.color("accent"), in: tr)
            }
        } else if shortcut.isEmpty {
            Draw.text(ctx, "—", font: Fonts.ui(Fonts.body), color: Theme.color("textTertiary"), in: tr)
        } else {
            Draw.text(ctx, Shortcut.display(shortcut), font: Fonts.ui(Fonts.body), color: conflict ? Theme.color("danger") : Theme.color("text"), in: tr)
        }
        if !shortcut.isEmpty && !recording && (hovered || isFocused) {
            Icons.draw(ctx, "x", in: clearRect.insetBy(dx: 4, dy: 4), color: Theme.color("textSecondary"), lineWidth: 1.75)
        }
        ctx.restoreGState()
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside else { return }
        if !shortcut.isEmpty && !recording && clearRect.contains(p) {
            shortcut = ""
            onChange?("")
            host?.setFocus(nil)
            return
        }
        startRecording()
    }

    func startRecording() {
        recording = true
        invalid = false
        liveMods = []
        if !singleKey { Hotkeys.shared.suspend() }
        setNeedsDisplay()
    }

    private func stopRecording() {
        recording = false
        liveMods = []
        if !singleKey { Hotkeys.shared.resume() }
        setNeedsDisplay()
    }

    override func focusChanged() {
        if !isFocused && recording { stopRecording() }
        setNeedsDisplay()
    }

    func flagsChanged(_ event: NSEvent) {
        guard recording else { return }
        liveMods = event.modifierFlags.intersection([.command, .option, .control, .shift])
        invalid = false
        setNeedsDisplay()
    }

    override func keyDown(_ event: NSEvent) -> Bool {
        guard recording else {
            if event.keyCode == 36 || event.keyCode == 49 { startRecording(); return true }
            return false
        }
        let mods = event.modifierFlags.intersection([.command, .option, .control, .shift])
        if mods.isEmpty && event.keyCode == 53 { stopRecording(); host?.setFocus(nil); return true }
        if mods.isEmpty && !singleKey && (event.keyCode == 51 || event.keyCode == 117) {
            shortcut = ""
            onChange?("")
            stopRecording()
            host?.setFocus(nil)
            return true
        }
        guard let key = KeyMap.name(forKeyCode: event.keyCode) else { return true }
        if singleKey {
            let s = Shortcut.string(mods: [], key: key)
            shortcut = s
            onChange?(s)
            stopRecording()
            host?.setFocus(nil)
            return true
        }
        let isFKey = key.hasPrefix("F") && key.count > 1 && Int(key.dropFirst()) != nil
        if mods.subtracting(.shift).isEmpty && !isFKey {
            invalid = true
            setNeedsDisplay()
            return true
        }
        let s = Shortcut.string(mods: mods, key: key)
        shortcut = s
        onChange?(s)
        stopRecording()
        host?.setFocus(nil)
        return true
    }
}

// MARK: - 分隔线

final class DividerW: Widget {
    override func draw(_ ctx: CGContext) {
        Draw.fill(ctx, CGRect(x: frame.minX, y: frame.midY - 0.5, width: frame.width, height: 1), Theme.color("divider"))
    }
}
