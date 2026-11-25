import AppKit
import LemonShotCore

/// 工具样式控件（spec 2.4 第 1 行）。样式面板和偏好设置「标注」页共用。
final class StyleControls {
    let tool: Int32
    let get: () -> ls_style
    let set: (ls_style) -> Void
    /// 第 1 行控件（null 表示组间分隔）。
    private(set) var row1: [Widget?] = []
    /// 马赛克的滑块放第 2 行（马赛克没有颜色）。
    private(set) var row2Extra: [Widget] = []
    private var refreshers: [(ls_style) -> Void] = []
    /// 样式改动后（控件显隐可能变化）通知容器重新排版。
    var afterChange: (() -> Void)?

    static var btn: CGFloat { Theme.f("popover.rowHeight") }
    /// 序号工具的「下一个」步进（偏好设置里不显示：它不写回设置）。
    let includeNext: Bool

    init(tool: Int32, get: @escaping () -> ls_style, set: @escaping (ls_style) -> Void, includeNext: Bool = true) {
        self.tool = tool
        self.get = get
        self.set = set
        self.includeNext = includeNext
        build()
        refresh()
    }

    func refresh() {
        let st = get()
        for r in refreshers { r(st) }
    }

    private func change(_ f: (inout ls_style) -> Void) {
        var st = get()
        f(&st)
        set(st)
        refresh()
        afterChange?()
    }

    private func button(icon: String?, tip: String, width: CGFloat? = nil, custom: ((CGContext, CGRect, CGColor) -> Void)? = nil, action: @escaping () -> Void, selected: @escaping (ls_style) -> Bool) -> IconButton {
        let b = IconButton(icon: icon, size: StyleControls.btn, iconSize: 18)
        if let w = width { b.frame.size.width = w }
        b.tooltip = tip
        b.custom = custom
        b.action = action
        refreshers.append { [weak b] st in b?.selected = selected(st) }
        return b
    }

    private func label(_ key: String) -> Label {
        let l = Label(L(key), font: Fonts.ui(Fonts.caption + 1), colorKey: "textSecondary")
        l.frame.size = CGSize(width: Draw.textWidth(L(key), l.font) + 2, height: StyleControls.btn)
        return l
    }

    private func widthLevels() -> [Widget?] {
        var out: [Widget?] = []
        for (i, lv) in AnnotStyles.widthLevels.enumerated() {
            let dot = AnnotStyles.widthDots[i]
            out.append(button(icon: nil, tip: L("style.lineWidth"), custom: { ctx, r, fg in
                Draw.ellipse(ctx, CGRect(x: r.midX - dot, y: r.midY - dot, width: 2 * dot, height: 2 * dot), fill: fg)
            }, action: { [weak self] in self?.change { $0.line_width = lv } }, selected: { st in
                AnnotStyles.nearestLevel(st.line_width, AnnotStyles.widthLevels) == i
            }))
        }
        return out
    }

    private func textChip(_ text: String, tip: String, action: @escaping () -> Void, selected: @escaping (ls_style) -> Bool) -> IconButton {
        let font = Fonts.ui(Fonts.caption + 1)
        let w = ceil(Draw.textWidth(text, font) + 16)
        return button(icon: nil, tip: tip, width: max(StyleControls.btn, w), custom: { ctx, r, fg in
            Draw.text(ctx, text, font: font, color: fg, in: r, align: .center, truncate: false)
        }, action: action, selected: selected)
    }

    private func slider(min: Double, max: Double, step: Double, get: @escaping (ls_style) -> Double, set: @escaping (inout ls_style, Double) -> Void, format: @escaping (Double) -> String) -> SliderW {
        // 数值带单位（如「18 pt」）：数值列放宽，滑块轨道相应缩短，总宽不变
        let s = SliderW(min: min, max: max, step: step, value: get(self.get()), trackWidth: Theme.f("popover.sliderWidth") - 50, format: format)
        s.valueWidth = 48
        s.frame.size = CGSize(width: Theme.f("popover.sliderWidth"), height: StyleControls.btn)
        s.onChange = { [weak self] v in self?.change { set(&$0, v) } }
        refreshers.append { [weak s] st in s?.value = get(st) }
        return s
    }

    private func build() {
        let t = Int(tool)
        switch t {
        case 1, 2:
            row1 = widthLevels() + [nil]
            let filledIcon = t == 1 ? "square" : "circle"
            row1.append(button(icon: filledIcon, tip: L("style.filled"), custom: { ctx, r, fg in
                Icons.draw(ctx, filledIcon, in: CGRect(x: r.midX - 8, y: r.midY - 8, width: 16, height: 16), color: fg, fill: fg)
            }, action: { [weak self] in self?.change { $0.filled = $0.filled != 0 ? 0 : 1 } }, selected: { $0.filled != 0 }))
            if t == 1 {
                row1.append(textChip(L("style.cornerRadius"), tip: L("style.cornerRadius"), action: { [weak self] in
                    self?.change { $0.corner_radius = $0.corner_radius > 0 ? 0 : AnnotStyles.cornerOn }
                }, selected: { $0.corner_radius > 0 }))
            }
        case 3:
            row1 = widthLevels() + [nil]
            row1.append(button(icon: nil, tip: L("style.line.1"), custom: { ctx, r, fg in
                ctx.saveGState()
                ctx.setStrokeColor(fg)
                ctx.setLineWidth(2)
                ctx.setLineCap(.butt)
                ctx.setLineDash(phase: 0, lengths: [4, 3])
                ctx.strokeLineSegments(between: [CGPoint(x: r.minX + 6, y: r.maxY - 6), CGPoint(x: r.maxX - 6, y: r.minY + 6)])
                ctx.restoreGState()
            }, action: { [weak self] in self?.change { $0.variant = $0.variant == 1 ? 0 : 1 } }, selected: { $0.variant == 1 }))
        case 4:
            for v in 0..<4 {
                row1.append(button(icon: nil, tip: L("style.arrow.\(v)"), custom: { ctx, r, fg in
                    if let img = KernelPreview.image(tool: Int32(LS_TOOL_ARROW.rawValue), variant: Int32(v), size: r.height, color: fg, scale: 2, lineWidth: 2) {
                        Draw.image(ctx, img, in: CGRect(x: r.midX - r.height / 2, y: r.minY, width: r.height, height: r.height))
                    }
                }, action: { [weak self] in self?.change { $0.variant = Int32(v) } }, selected: { $0.variant == Int32(v) }))
            }
            row1.append(nil)
            row1 += widthLevels()
        case 5:
            for (v, icon) in ["pencil", "highlighter"].enumerated() {
                row1.append(button(icon: icon, tip: L("style.pen.\(v)"), action: { [weak self] in self?.change { $0.variant = Int32(v) } }, selected: { $0.variant == Int32(v) }))
            }
            row1.append(nil)
            row1 += widthLevels()
        case 6:
            for (v, icon) in ["grid-3x3", "brush", "droplet", "blend"].enumerated() {
                row1.append(button(icon: icon, tip: L("style.mosaic.\(v)"), action: { [weak self] in self?.change { $0.variant = Int32(v) } }, selected: { $0.variant == Int32(v) }))
            }
            row2Extra = [label("style.blockSize"),
                         slider(min: 2, max: 60, step: 1, get: { Double($0.block_size) }, set: { $0.block_size = Float($1) }, format: PrefsPages.points)]
            let brushLabel = label("style.brushSize")
            let brush = slider(min: 4, max: 120, step: 1, get: { Double($0.line_width) }, set: { $0.line_width = Float($1) }, format: PrefsPages.points)
            let always = !includeNext
            refreshers.append { [weak brushLabel, weak brush] st in
                let show = always || st.variant == 1 || st.variant == 3
                brushLabel?.isHidden = !show
                brush?.isHidden = !show
            }
            row2Extra += [brushLabel, brush]
        case 7:
            for v in 0..<3 {
                row1.append(button(icon: nil, tip: L("style.text.\(v)"), custom: { ctx, r, fg in
                    let ir = CGRect(x: r.midX - 9, y: r.midY - 9, width: 18, height: 18)
                    switch v {
                    case 0: Icons.draw(ctx, "type", in: ir, color: fg)
                    case 1: Icons.draw(ctx, "type-outline", in: ir, color: fg)
                    default:
                        Draw.fill(ctx, ir.insetBy(dx: -1, dy: -1), fg, radius: 4)
                        Icons.draw(ctx, "type", in: ir.insetBy(dx: 2, dy: 2), color: Theme.color("surface"))
                    }
                }, action: { [weak self] in self?.change { $0.variant = Int32(v) } }, selected: { $0.variant == Int32(v) }))
            }
            row1.append(nil)
            row1.append(label("style.fontSize"))
            row1.append(slider(min: 8, max: 200, step: 1, get: { Double($0.font_size) }, set: { $0.font_size = Float($1) }, format: PrefsPages.points))
            row1.append(nil)
            row1.append(button(icon: "bold", tip: L("style.bold"), action: { [weak self] in self?.change { $0.bold = $0.bold != 0 ? 0 : 1 } }, selected: { $0.bold != 0 }))
        case 8:
            for v in 0..<3 {
                row1.append(button(icon: nil, tip: L("style.step.\(v)"), custom: { ctx, r, fg in
                    if let img = KernelPreview.image(tool: Int32(LS_TOOL_STEP.rawValue), variant: Int32(v), size: r.height, color: fg, scale: 2, lineWidth: 18) {
                        Draw.image(ctx, img, in: CGRect(x: r.midX - r.height / 2, y: r.minY, width: r.height, height: r.height))
                    }
                }, action: { [weak self] in self?.change { $0.variant = Int32(v) } }, selected: { $0.variant == Int32(v) }))
            }
            row1.append(nil)
            row1.append(label("style.size"))
            row1.append(slider(min: 16, max: 64, step: 1, get: { Double($0.line_width) }, set: { $0.line_width = Float($1) }, format: PrefsPages.points))
            guard includeNext else { break }
            row1.append(nil)
            row1.append(label("style.next"))
            let stepper = StepperW(min: 1, max: 999, step: 1, value: Double(get().next_number), width: 92)
            stepper.frame.size.height = StyleControls.btn
            stepper.onChange = { [weak self] v in self?.change { $0.next_number = Int32(v) } }
            refreshers.append { [weak stepper] st in stepper?.value = Double(st.next_number) }
            row1.append(stepper)
        case 9:
            row1.append(label("style.opacity"))
            row1.append(slider(min: 0.1, max: 0.95, step: 0.05, get: { Double($0.opacity) }, set: { $0.opacity = Float($1) }, format: { "\(Int(($0 * 100).rounded()))%" }))
            row1.append(nil)
            row1.append(textChip(L("style.cornerRadius"), tip: L("style.cornerRadius"), action: { [weak self] in
                self?.change { $0.corner_radius = $0.corner_radius > 0 ? 0 : AnnotStyles.cornerOn }
            }, selected: { $0.corner_radius > 0 }))
        case 10:
            // 滚轮按 0.5 调（1.5–6）：不在档位上时亮最近的一档
            for (i, z) in AnnotStyles.magnifyZooms.enumerated() {
                let text = (z == z.rounded() ? String(Int(z)) : String(z)) + "×"
                row1.append(textChip(text, tip: L("style.zoom"), action: { [weak self] in self?.change { $0.zoom = z } },
                                     selected: { AnnotStyles.nearestLevel($0.zoom, AnnotStyles.magnifyZooms) == i }))
            }
            row1.append(nil)
            row1 += widthLevels()
        default: break
        }
    }

    static func rowWidth(_ items: [Widget?]) -> CGFloat {
        var w: CGFloat = 0
        var first = true
        var pendingSep = false
        for it in items {
            if let it = it {
                if it.isHidden { continue }
                if pendingSep { w += 17; pendingSep = false } else if !first { w += it is Label ? 6 : 4 }
                w += it.frame.width
                first = false
            } else if !first {
                pendingSep = true
            }
        }
        return w
    }

    /// 从 (x, y) 开始横排，返回分隔线的 x。
    @discardableResult
    static func layoutRow(_ items: [Widget?], x: CGFloat, y: CGFloat, height: CGFloat) -> [CGFloat] {
        var cx = x
        var seps: [CGFloat] = []
        var first = true
        var pendingSep = false
        for it in items {
            if let it = it {
                if it.isHidden { continue }
                if pendingSep {
                    seps.append(cx + 8)
                    cx += 17
                    pendingSep = false
                } else if !first {
                    cx += it is Label ? 6 : 4
                }
                it.frame.origin = CGPoint(x: cx, y: y + (height - it.frame.height) / 2)
                it.layout()
                cx += it.frame.width
                first = false
            } else if !first {
                pendingSep = true
            }
        }
        return seps
    }

    static func drawSeparators(_ ctx: CGContext, _ xs: [CGFloat], y: CGFloat, height: CGFloat) {
        for x in xs { Draw.fill(ctx, CGRect(x: x, y: y + (height - 16) / 2, width: 1, height: 16), Theme.color("divider")) }
    }
}

/// 调色板行（spec 2.4 第 2 行）。
final class PaletteRow: Widget {
    let get: () -> ls_color
    let set: (ls_color) -> Void
    private var hover = -1

    init(get: @escaping () -> ls_color, set: @escaping (ls_color) -> Void) {
        self.get = get
        self.set = set
        super.init()
        frame.size = CGSize(width: PaletteRow.width, height: Theme.f("popover.rowHeight"))
    }

    override var isInteractive: Bool { true }

    static var width: CGFloat {
        let sw = Theme.f("popover.swatch"), g = Theme.f("popover.swatchGap")
        let n = CGFloat(AnnotStyles.palette().count + 1)
        return n * sw + (n - 1) * g + 4
    }

    private func rect(_ i: Int) -> CGRect {
        let sw = Theme.f("popover.swatch"), g = Theme.f("popover.swatchGap")
        return CGRect(x: frame.minX + 2 + CGFloat(i) * (sw + g), y: frame.midY - sw / 2, width: sw, height: sw)
    }

    override func draw(_ ctx: CGContext) {
        let pal = AnnotStyles.palette()
        let cur = Theme.hexString(get())
        var matched = false
        for (i, h) in pal.enumerated() {
            let r = rect(i)
            if h.uppercased() == cur {
                matched = true
                Draw.ellipse(ctx, r.insetBy(dx: -3, dy: -3), stroke: Theme.color("accent"), width: 2)
            } else if hover == i {
                Draw.ellipse(ctx, r.insetBy(dx: -3, dy: -3), stroke: Theme.color("border"), width: 2)
            }
            Draw.ellipse(ctx, r, fill: Theme.hex(h))
            Draw.ellipse(ctx, r, stroke: CGColor(gray: 0, alpha: 0.15), width: 1)
        }
        // 自定义颜色：不在调色板里时显示当前色
        let r = rect(pal.count)
        if !matched {
            Draw.ellipse(ctx, r.insetBy(dx: -3, dy: -3), stroke: Theme.color("accent"), width: 2)
            Draw.ellipse(ctx, r, fill: Theme.cg(get()))
            Draw.ellipse(ctx, r, stroke: CGColor(gray: 0, alpha: 0.15), width: 1)
        } else {
            if hover == pal.count { Draw.ellipse(ctx, r.insetBy(dx: -3, dy: -3), stroke: Theme.color("border"), width: 2) }
            Icons.draw(ctx, "palette", in: r.insetBy(dx: 0.5, dy: 0.5), color: Theme.color("icon"), lineWidth: 1.5)
        }
    }

    override func mouseMoved(_ p: CGPoint) {
        let n = AnnotStyles.palette().count + 1
        let h = (0..<n).first { rect($0).insetBy(dx: -3, dy: -3).contains(p) } ?? -1
        if h != hover { hover = h; setNeedsDisplay() }
    }

    override func mouseExited() { hover = -1; super.mouseExited() }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        let pal = AnnotStyles.palette()
        guard let i = (0...pal.count).first(where: { rect($0).insetBy(dx: -3, dy: -3).contains(p) }) else { return }
        if i < pal.count {
            set(Theme.lsColor(hex: pal[i]))
            setNeedsDisplay()
        } else {
            ColorPanelBridge.shared.open(initial: Theme.hexString(get()), alpha: false) { [weak self] hex in
                self?.set(Theme.lsColor(hex: hex))
                self?.setNeedsDisplay()
            }
        }
    }
}

/// 样式面板（spec 2.4）：圆角底板 + 第 1 行工具控件 + 第 2 行调色板（或马赛克滑块）+ 右上角说明。
final class StylePopover: Widget {
    let tool: Int32
    let controls: StyleControls
    private var palette: PaletteRow?
    private let info: InfoW
    private var seps: [CGFloat] = []
    private var seps2: [CGFloat] = []

    init(tool: Int32, get: @escaping () -> ls_style, set: @escaping (ls_style) -> Void) {
        self.tool = tool
        controls = StyleControls(tool: tool, get: get, set: set)
        info = InfoW(L("style.hint"))
        super.init()
        for w in controls.row1 { if let w = w { add(w) } }
        for w in controls.row2Extra { add(w) }
        if tool != Int32(LS_TOOL_HIGHLIGHT.rawValue) && tool != Int32(LS_TOOL_MOSAIC.rawValue) {
            let p = PaletteRow(get: { get().color }, set: { c in
                var st = get()
                st.color = c
                set(st)
            })
            palette = p
            add(p)
        }
        add(info)
        frame.size = contentSize()
        controls.afterChange = { [weak self] in self?.relayout() }
    }

    /// 尺寸变化时由宿主重新摆放（马赛克切换画笔样式会多一个滑块）。
    var onResize: (() -> Void)?

    private func relayout() {
        let size = contentSize()
        if size != frame.size {
            frame.size = size
            layout()
            onResize?()
        } else {
            layout()
        }
        host?.needsDisplay = true
    }

    override var isInteractive: Bool { true }

    var hasRow2: Bool { palette != nil || !controls.row2Extra.isEmpty }

    /// 第 2 行（马赛克）：块大小 | 笔刷大小。
    private var row2Items: [Widget?] {
        var items: [Widget?] = []
        for (i, w) in controls.row2Extra.enumerated() {
            if i > 0 && i % 2 == 0 { items.append(nil) }
            items.append(w)
        }
        return items
    }

    func contentSize() -> CGSize {
        let pad = Theme.f("popover.padding")
        let rh = Theme.f("popover.rowHeight")
        var w = StyleControls.rowWidth(controls.row1) + 8 + 18
        if let p = palette { w = max(w, p.frame.width) }
        if !controls.row2Extra.isEmpty {
            w = max(w, StyleControls.rowWidth(row2Items))
        }
        let h = hasRow2 ? rh * 2 + Theme.f("popover.gap") : rh
        return CGSize(width: ceil(w + 2 * pad), height: ceil(h + 2 * pad))
    }

    override func layout() {
        let pad = Theme.f("popover.padding")
        let rh = Theme.f("popover.rowHeight")
        seps = StyleControls.layoutRow(controls.row1, x: frame.minX + pad, y: frame.minY + pad, height: rh)
        info.frame = CGRect(x: frame.maxX - pad - 18, y: frame.minY + pad + (rh - 18) / 2, width: 18, height: 18)
        let y2 = frame.minY + pad + rh + Theme.f("popover.gap")
        if let p = palette {
            p.frame.origin = CGPoint(x: frame.minX + pad - 2, y: y2)
        }
        if !controls.row2Extra.isEmpty {
            seps2 = StyleControls.layoutRow(row2Items, x: frame.minX + pad, y: y2, height: rh)
        }
    }

    func refresh() {
        controls.refresh()
        layout()
        setNeedsDisplay()
        host?.needsDisplay = true
    }

    override func draw(_ ctx: CGContext) {
        Draw.panel(ctx, frame, radius: Theme.f("popover.radius"), fill: Theme.color("popoverBg"), border: Theme.color("toolbarBorder"),
                   shadow: Theme.color("shadow"), blur: Theme.f("toolbar.shadowBlur"), offsetY: Theme.f("toolbar.shadowOffsetY"))
        let pad = Theme.f("popover.padding")
        let rh = Theme.f("popover.rowHeight")
        StyleControls.drawSeparators(ctx, seps, y: frame.minY + pad, height: rh)
        StyleControls.drawSeparators(ctx, seps2, y: frame.minY + pad + rh + Theme.f("popover.gap"), height: rh)
        drawChildren(ctx)
    }
}
