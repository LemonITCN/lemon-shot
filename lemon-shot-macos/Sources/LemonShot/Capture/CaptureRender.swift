import AppKit
import LemonShotCore

/// 截图层各部分的绘制函数（spec 2.2）。屏幕上每部分是一个视图，自测时按同样的位置依次画进一张图。
enum CaptureRender {
    // MARK: 遮罩、悬停、标注、选区

    struct Options {
        var showHandles = true
        var showMask = true
        var showSelectionBorder = true
    }

    /// 遮罩 + 悬停 + 标注（裁剪到选区）+ 选区边框与控制点 + 选中标注控制点。
    static func drawMain(_ ctx: CGContext, session: Session, state st: ls_session_state, bounds: CGRect, dirty: CGRect, options: Options = Options()) {
        let mask = Theme.cap("mask")
        let selectedPhase = st.phase == Int32(LS_PHASE_SELECTED.rawValue)
        // 没有选区：整屏遮罩 + 悬停挖空。只要有选区（包括正在拖出选区时）就只遮选区外（spec 2.2）
        if st.has_selection == 0 {
            if options.showMask {
                if st.has_hover != 0 {
                    let h = st.hover.cg
                    ctx.saveGState()
                    ctx.addRect(bounds)
                    ctx.addRect(h)
                    ctx.setFillColor(mask)
                    ctx.fillPath(using: .evenOdd)
                    ctx.restoreGState()
                    Draw.fill(ctx, h, Theme.cap("windowHoverFill"))
                    let bw = Theme.f("capture.windowHoverBorderWidth")
                    Draw.stroke(ctx, h.intersection(bounds), Theme.cap("windowHover"), width: bw)
                } else {
                    Draw.fill(ctx, bounds, mask)
                }
            }
            return
        }
        let sel = st.selection.cg
        if options.showMask {
            ctx.saveGState()
            ctx.addRect(bounds)
            ctx.addRect(sel)
            ctx.setFillColor(mask)
            ctx.fillPath(using: .evenOdd)
            ctx.restoreGState()
        }
        if sel.intersects(dirty) {
            ctx.saveGState()
            ctx.clip(to: sel)
            AnnotRender.drawAll(ctx, session: session, dirty: dirty)
            ctx.restoreGState()
        }
        if options.showSelectionBorder {
            // 正在画标注、拖出新选区时不画控制点
            let handles = options.showHandles && selectedPhase && st.dragging_shape == 0
            drawSelectionChrome(ctx, sel, handles: handles)
        }
        drawSelectedShapeChrome(ctx, session: session, state: st)
    }

    static func drawSelectedShapeChrome(_ ctx: CGContext, session: Session, state st: ls_session_state) {
        guard st.selected_shape != 0 else { return }
        let pts = session.shapeHandles()
        if pts.isEmpty {
            for i in 0..<session.shapeCount {
                let v = session.shape(i)
                if v.id == st.selected_shape && v.editing == 0 { AnnotRender.drawSelectionDash(ctx, v.bounds.cg) }
            }
        } else {
            AnnotRender.drawShapeHandles(ctx, pts)
        }
    }

    static func drawSelectionChrome(_ ctx: CGContext, _ sel: CGRect, handles: Bool) {
        let bw = Theme.f("capture.selectionBorderWidth")
        ctx.saveGState()
        ctx.setStrokeColor(Theme.cap("selectionBorder"))
        ctx.setLineWidth(bw)
        // 边框画在选区外侧，不压住选中的像素
        ctx.stroke(sel.insetBy(dx: -bw / 2, dy: -bw / 2))
        ctx.restoreGState()
        guard handles else { return }
        let hs = Theme.f("capture.handleSize")
        for p in handlePoints(sel) {
            let r = CGRect(x: p.x - hs / 2, y: p.y - hs / 2, width: hs, height: hs)
            Draw.fill(ctx, r, Theme.cap("handleFill"))
            Draw.stroke(ctx, r, Theme.cap("handleBorder"), width: 1)
        }
    }

    static func handlePoints(_ r: CGRect) -> [CGPoint] {
        [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.midX, y: r.minY), CGPoint(x: r.maxX, y: r.minY),
         CGPoint(x: r.minX, y: r.midY), CGPoint(x: r.maxX, y: r.midY),
         CGPoint(x: r.minX, y: r.maxY), CGPoint(x: r.midX, y: r.maxY), CGPoint(x: r.maxX, y: r.maxY)]
    }

    /// 主层上会变化的区域（用于局部重画）：选区（含控制点）、悬停框、每个标注的外接框。
    static func dirtyRects(session: Session, state st: ls_session_state) -> [CGRect] {
        var out: [CGRect] = []
        let pad = Theme.f("capture.handleSize") + Theme.f("capture.windowHoverBorderWidth") + 2
        if st.has_hover != 0 && st.phase == Int32(LS_PHASE_SELECTING.rawValue) { out.append(st.hover.cg.insetBy(dx: -pad, dy: -pad)) }
        if st.has_selection != 0 { out.append(st.selection.cg.insetBy(dx: -pad, dy: -pad)) }
        return out
    }

    // MARK: 十字线

    static func drawCrosshair(_ ctx: CGContext, at p: CGPoint, bounds: CGRect) {
        ctx.setFillColor(Theme.cap("crosshair"))
        ctx.fill(CGRect(x: bounds.minX, y: floor(p.y), width: bounds.width, height: 1))
        ctx.fill(CGRect(x: floor(p.x), y: bounds.minY, width: 1, height: bounds.height))
    }

    // MARK: 尺寸提示

    static let ratioNames = ["", "1:1", "4:3", "3:2", "16:9", "16:10", "3:4", "2:3", "9:16", "21:9"]

    static var tipFont: NSFont { Fonts.digits(Fonts.caption + 1, .medium) }

    static func sizeTipText(session: Session, state st: ls_session_state) -> String? {
        var r: CGRect
        if st.has_selection != 0 { r = st.selection.cg } else if st.has_hover != 0 { r = st.hover.cg } else { return nil }
        var w = Int((r.width * session.scale).rounded()), h = Int((r.height * session.scale).rounded())
        if st.has_selection != 0 {
            let px = session.selectionPixels
            w = Int(px.width)
            h = Int(px.height)
        }
        var s = "\(w) × \(h)"
        if st.window_count > 1 {
            s += "　" + L("capture.tip.windows", ["n": String(st.window_count)])
        } else if st.ratio_index > 0 && Int(st.ratio_index) < ratioNames.count {
            s += "　" + L("capture.tip.ratio", ["ratio": ratioNames[Int(st.ratio_index)]])
        }
        return s
    }

    static func sizeTipSize(_ text: String) -> CGSize {
        CGSize(width: ceil(Draw.textWidth(text, tipFont) + 2 * Theme.f("capture.tipPaddingX")), height: Theme.f("capture.tipHeight"))
    }

    static func drawSizeTip(_ ctx: CGContext, _ r: CGRect, _ text: String) {
        Draw.fill(ctx, r, Theme.cap("tipBg"), radius: Theme.f("capture.tipRadius"))
        Draw.text(ctx, text, font: tipFont, color: Theme.cap("tipText"), in: r.insetBy(dx: Theme.f("capture.tipPaddingX"), dy: 0), truncate: false)
    }

    // MARK: 放大镜

    static var magnifierInfoFont: NSFont { Fonts.digits(Fonts.caption) }

    static func magnifierSize(showHint: Bool) -> CGSize {
        let size = Theme.f("capture.magnifier.size")
        return CGSize(width: size, height: size + Theme.f("capture.magnifier.infoHeight"))
    }

    struct PixelInfo {
        var x: Int
        var y: Int
        var color: ls_color
        var hex: String
        var name: String
    }

    static func pixelInfo(session: Session, at p: CGPoint) -> PixelInfo {
        let c = session.pixel(at: p)
        let shown = ColorValue.converted(c, frozen: session.frozen)
        return PixelInfo(x: Int(floor(p.x * session.scale)), y: Int(floor(p.y * session.scale)), color: shown,
                         hex: ColorValue.hex(shown), name: ColorValue.name(shown))
    }

    static func drawMagnifier(_ ctx: CGContext, _ rect: CGRect, frozen: FrozenImage, mouse: CGPoint, info: PixelInfo, showHint: Bool) {
        let size = Theme.f("capture.magnifier.size")
        let pixels = Theme.f("capture.magnifier.pixels")
        let radius = Theme.f("capture.magnifier.radius")
        let cell = size / pixels
        let top = CGRect(x: rect.minX, y: rect.minY, width: size, height: size)
        let infoR = CGRect(x: rect.minX, y: rect.minY + size, width: size, height: rect.height - size)
        ctx.saveGState()
        ctx.addPath(Draw.rounded(rect, radius))
        ctx.clip()
        Draw.fill(ctx, top, CGColor(gray: 0, alpha: 1))
        // 光标所在像素居中
        let px = floor(mouse.x * frozen.scale), py = floor(mouse.y * frozen.scale)
        let half = floor(pixels / 2)
        let src = CGRect(x: (px - half) / frozen.scale, y: (py - half) / frozen.scale, width: pixels / frozen.scale, height: pixels / frozen.scale)
        AnnotRender.drawFrozen(ctx, frozen, src: src, dst: top, smooth: false)
        ctx.setStrokeColor(Theme.cap("magnifierGrid"))
        ctx.setLineWidth(1)
        var lines: [CGPoint] = []
        var i: CGFloat = 1
        while i < pixels {
            let x = top.minX + i * cell, y = top.minY + i * cell
            lines += [CGPoint(x: x, y: top.minY), CGPoint(x: x, y: top.maxY), CGPoint(x: top.minX, y: y), CGPoint(x: top.maxX, y: y)]
            i += 1
        }
        ctx.strokeLineSegments(between: lines)
        let c0 = half * cell
        // 十字中线（穿过中心像素，避开中心框）
        ctx.setFillColor(Theme.cap("magnifierCenter").alpha(0.55))
        let mid = c0 + cell / 2 - 0.5
        ctx.fill(CGRect(x: top.minX, y: top.minY + mid, width: c0 - 1, height: 1))
        ctx.fill(CGRect(x: top.minX + c0 + cell + 1, y: top.minY + mid, width: size - c0 - cell - 1, height: 1))
        ctx.fill(CGRect(x: top.minX + mid, y: top.minY, width: 1, height: c0 - 1))
        ctx.fill(CGRect(x: top.minX + mid, y: top.minY + c0 + cell + 1, width: 1, height: size - c0 - cell - 1))
        Draw.stroke(ctx, CGRect(x: top.minX + c0, y: top.minY + c0, width: cell, height: cell).insetBy(dx: -1, dy: -1), Theme.cap("magnifierCenter"), width: 2)
        // 信息区
        Draw.fill(ctx, infoR, Theme.cap("tipBg"))
        let font = magnifierInfoFont
        let white = Theme.cap("tipText")
        let lineH: CGFloat = 14
        var y = infoR.minY + 4
        let x = infoR.minX + 8
        let w = infoR.width - 16
        Draw.text(ctx, "\(info.x), \(info.y)", font: font, color: white, in: CGRect(x: x, y: y, width: w, height: lineH))
        y += lineH
        let sw = CGRect(x: x, y: y + 2, width: 10, height: 10)
        Draw.fill(ctx, sw, Theme.cg(info.color), radius: 2)
        Draw.stroke(ctx, sw, CGColor(gray: 1, alpha: 0.6), width: 1, radius: 2)
        Draw.text(ctx, info.hex, font: font, color: white, in: CGRect(x: x + 16, y: y, width: w - 16, height: lineH))
        y += lineH
        Draw.text(ctx, info.name, font: Fonts.ui(Fonts.caption), color: white, in: CGRect(x: x, y: y, width: w, height: lineH))
        y += lineH
        if showHint {
            // 提示行按宽度自动缩小字号，保证完整显示
            let hint = L("capture.magnifier.copy")
            var fs = Fonts.caption - 1
            while fs > 7 && Draw.textWidth(hint, Fonts.ui(fs)) > w { fs -= 0.5 }
            Draw.text(ctx, hint, font: Fonts.ui(fs), color: white.alpha(0.75), in: CGRect(x: x, y: y, width: w, height: lineH))
        }
        ctx.restoreGState()
        Draw.stroke(ctx, rect, Theme.cap("magnifierBorder"), width: 1, radius: radius)
    }

    // MARK: 左下角提示

    static var tipsFont: NSFont { Fonts.ui(Fonts.caption + 1) }

    static func tipsLines(_ st: ls_session_state) -> [String] {
        st.has_selection != 0 && st.phase == Int32(LS_PHASE_SELECTED.rawValue) ? [L("capture.tip.adjust")] : [L("capture.tip.select"), L("capture.tip.keys")]
    }

    static func tipsSize(_ lines: [String]) -> CGSize {
        let w = lines.map { Draw.textWidth($0, tipsFont) }.max() ?? 0
        let lh = Draw.lineHeight(tipsFont) + 4
        return CGSize(width: ceil(w + 28), height: ceil(lh * CGFloat(lines.count) + 12))
    }

    static func drawTips(_ ctx: CGContext, _ r: CGRect, _ lines: [String], color: CGColor) {
        Draw.fill(ctx, r, Theme.cap("tipBg"), radius: min(r.height / 2, 14))
        let lh = Draw.lineHeight(tipsFont) + 4
        var y = r.minY + 6
        for l in lines {
            Draw.text(ctx, l, font: tipsFont, color: color, in: CGRect(x: r.minX + 14, y: y, width: r.width - 28, height: lh), truncate: false)
            y += lh
        }
    }

    // MARK: 提示气泡（工具栏按钮）

    static var tooltipFont: NSFont { Fonts.ui(Fonts.caption + 1) }

    static func tooltipSize(_ text: String) -> CGSize {
        CGSize(width: ceil(Draw.textWidth(text, tooltipFont) + 16), height: 24)
    }

    static func drawTooltip(_ ctx: CGContext, _ r: CGRect, _ text: String) {
        Draw.fill(ctx, r, Theme.cap("tipBg"), radius: Theme.f("capture.tipRadius"))
        Draw.text(ctx, text, font: tooltipFont, color: Theme.cap("tipText"), in: r.insetBy(dx: 8, dy: 0), truncate: false)
    }
}
