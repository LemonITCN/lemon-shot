import AppKit

/// 偏好设置页面排版（spec 11）：页标题 + 分组（组标题 caption、textSecondary）+ 每行左标签右控件。
final class FormPage: Widget {
    enum Item {
        case group(String, info: String?)
        case row(String, [Widget], info: String?)
        /// 控件下方的说明值（例如文件名示例），左对齐到控件列。
        case value(Label)
        case buttons([Widget])
        case custom(Widget, height: CGFloat)
    }

    let title: String
    /// 页面上控件依赖的对象（样式控件组等），随页面释放。
    var keepAlive: [AnyObject] = []
    private(set) var items: [Item] = []
    private var labels: [(String, CGRect)] = []
    private var groupTitles: [(String, CGRect)] = []
    private var dividers: [CGRect] = []

    init(title: String) {
        self.title = title
    }

    override var isInteractive: Bool { false }

    func group(_ t: String, info: String? = nil) {
        items.append(.group(t, info: info))
        if let i = info { add(InfoW(i)) }
    }

    func row(_ label: String, _ controls: [Widget], info: String? = nil) {
        items.append(.row(label, controls, info: info))
        for c in controls { add(c) }
        if let i = info { add(InfoW(i)) }
    }

    func row(_ label: String, _ control: Widget, info: String? = nil) { row(label, [control], info: info) }

    func value(_ l: Label) {
        items.append(.value(l))
        add(l)
    }

    func buttons(_ bs: [Widget]) {
        items.append(.buttons(bs))
        for b in bs { add(b) }
    }

    func custom(_ w: Widget, height: CGFloat) {
        items.append(.custom(w, height: height))
        add(w)
    }

    var pad: CGFloat { Theme.f("window.contentPadding") }
    var labelWidth: CGFloat { Theme.f("window.labelWidth") }
    var rowHeight: CGFloat { Theme.f("window.rowHeight") }
    var rowGap: CGFloat { Theme.f("window.rowGap") }
    var sectionGap: CGFloat { Theme.f("window.sectionGap") }

    /// 按宽度排版，返回内容总高度。
    @discardableResult
    func layoutPage(width: CGFloat) -> CGFloat {
        labels.removeAll()
        groupTitles.removeAll()
        dividers.removeAll()
        var infos = children.compactMap { $0 as? InfoW }.makeIterator()
        let x0 = frame.minX + pad
        let cx = x0 + labelWidth
        var y = frame.minY + pad
        let headerH: CGFloat = 28
        y += headerH + 12
        var firstGroup = true
        for it in items {
            switch it {
            case .group(let t, let info):
                if !firstGroup { y += sectionGap - rowGap }
                if !firstGroup { dividers.append(CGRect(x: x0, y: y - sectionGap / 2, width: width - 2 * pad, height: 1)) }
                firstGroup = false
                // 空标题：只分隔不写组名
                if t.isEmpty { continue }
                let f = Fonts.semibold(Fonts.caption)
                let tw = Draw.textWidth(t, f)
                groupTitles.append((t, CGRect(x: x0, y: y, width: tw + 2, height: 20)))
                if info != nil, let iw = infos.next() { iw.frame = CGRect(x: x0 + tw + 6, y: y + 1, width: 18, height: 18) }
                y += 24
            case .row(let label, let controls, let info):
                firstGroup = false
                labels.append((label, CGRect(x: x0, y: y, width: labelWidth - 12, height: rowHeight)))
                var x = cx
                for c in controls where !c.isHidden {
                    let s = c.frame.size == .zero ? c.preferredSize() : c.frame.size
                    c.frame = CGRect(x: x, y: y + (rowHeight - s.height) / 2, width: s.width, height: s.height)
                    c.layout()
                    x += s.width + 8
                }
                // 说明图标紧跟在标签文字后面
                if info != nil, let iw = infos.next() {
                    let lw = min(Draw.textWidth(label, Fonts.ui(Fonts.body)), labelWidth - 36)
                    iw.frame = CGRect(x: x0 + lw + 4, y: y + (rowHeight - 18) / 2, width: 18, height: 18)
                }
                y += rowHeight + rowGap
            case .value(let l):
                l.frame = CGRect(x: cx, y: y - rowGap - 4, width: frame.minX + width - pad - cx, height: 18)
                y += 16
            case .buttons(let bs):
                y += sectionGap - rowGap
                var x = x0
                for b in bs {
                    let s = b.preferredSize()
                    b.frame = CGRect(x: x, y: y, width: s.width, height: s.height)
                    x += s.width + 10
                }
                y += controlH + rowGap
            case .custom(let w, let h):
                w.frame = CGRect(x: x0, y: y, width: width - 2 * pad, height: h)
                w.layout()
                y += h + rowGap
            }
        }
        return y - frame.minY + pad - rowGap
    }

    private var controlH: CGFloat { Theme.f("window.controlHeight") }

    override func draw(_ ctx: CGContext) {
        Draw.text(ctx, title, font: Fonts.semibold(Fonts.header), color: Theme.color("text"),
                  in: CGRect(x: frame.minX + pad, y: frame.minY + pad, width: frame.width - 2 * pad, height: 28))
        for (t, r) in groupTitles {
            Draw.text(ctx, t, font: Fonts.semibold(Fonts.caption), color: Theme.color("textSecondary"), in: r, truncate: false)
        }
        for d in dividers { Draw.fill(ctx, d, Theme.color("divider")) }
        let lf = Fonts.ui(Fonts.body)
        for (t, r) in labels where !t.isEmpty {
            if Draw.textWidth(t, lf) <= r.width {
                Draw.text(ctx, t, font: lf, color: Theme.color("text"), in: r)
            } else {
                // 放不下（英文较长）时折成两行，垂直居中
                let small = Fonts.ui(Fonts.body - 1)
                let size = Draw.wrappedSize(t, font: small, maxWidth: r.width, lineSpacing: 0)
                Draw.wrapped(ctx, t, font: small, color: Theme.color("text"), x: r.minX, y: r.midY - min(size.height, r.height) / 2, width: r.width, lineSpacing: 0)
            }
        }
        drawChildren(ctx)
    }
}

/// 设置绑定的控件工厂：改动立即写设置。
enum Bind {
    static var s: Settings { Settings.shared }

    static func toggle(_ key: String, onChange: ((Bool) -> Void)? = nil) -> SwitchW {
        SwitchW(s.bool(key)) { v in s.set(key, v); onChange?(v) }
    }

    static func dropdown(_ key: String, _ options: [Option], width: CGFloat = 220) -> DropdownW {
        DropdownW(options, value: s.value(key).map { valueString($0) } ?? "", width: width) { v in s.set(key, jsonFor(v, like: s.value(key))) }
    }

    static func segmented(_ key: String, _ values: [String], _ labels: [String]) -> SegmentedW {
        let cur = s.value(key).map { valueString($0) } ?? ""
        return SegmentedW(labels, selected: values.firstIndex(of: cur) ?? 0) { i in s.set(key, jsonFor(values[i], like: s.value(key))) }
    }

    static func slider(_ key: String, min: Double, max: Double, step: Double, width: CGFloat = 180, scale: Double = 1, format: @escaping (Double) -> String) -> SliderW {
        SliderW(min: min, max: max, step: step, value: s.double(key) * scale, trackWidth: width, format: format) { v in s.set(key, .number(((v / scale) * 1000).rounded() / 1000)) }
    }

    static func stepper(_ key: String, min: Double, max: Double, step: Double = 1, scale: Double = 1, width: CGFloat = 112, format: @escaping (Double) -> String = { String(Int($0)) }) -> StepperW {
        StepperW(min: min, max: max, step: step, value: (s.double(key) / scale).rounded(), width: width, format: format) { v in s.set(key, .number(v * scale)) }
    }

    static func color(_ key: String, alpha: Bool = false) -> ColorSwatchW {
        ColorSwatchW(s.string(key), allowsAlpha: alpha) { v in s.set(key, v) }
    }

    static func path(_ key: String) -> PathW {
        PathW(s.string(key), width: 300) { v in s.set(key, v) }
    }

    static func text(_ key: String, secure: Bool = false, width: CGFloat = 260, onChange: ((String) -> Void)? = nil) -> TextFieldW {
        TextFieldW(s.string(key), secure: secure, width: width) { v in s.set(key, v); onChange?(v) }
    }

    static func valueString(_ j: JSON) -> String {
        switch j {
        case .string(let v): return v
        case .number(let n): return n == n.rounded() ? String(Int(n)) : String(n)
        case .bool(let b): return b ? "true" : "false"
        default: return ""
        }
    }

    /// 写回时保持原来的类型（数字仍是数字）。
    static func jsonFor(_ v: String, like old: JSON?) -> JSON {
        if case .number? = old, let d = Double(v) { return .number(d) }
        if case .bool? = old { return .bool(v == "true") }
        return .string(v)
    }
}
