import AppKit
import CoreText
import LemonShotCore

/// 标注文字：测量回调与绘制共用同一套排版（按 \n 分行，行高取字体行高）。
enum AnnotText {
    private static var fontCache: [String: NSFont] = [:]

    static func font(_ size: CGFloat, bold: Bool) -> NSFont {
        let key = "\(size)-\(bold)"
        if let f = fontCache[key] { return f }
        let f = NSFont.systemFont(ofSize: max(1, size), weight: bold ? .bold : .regular)
        fontCache[key] = f
        return f
    }

    static func lineHeight(_ f: NSFont) -> CGFloat { ceil(f.ascender - f.descender + f.leading) }

    private static func ctLine(_ s: String, _ f: NSFont, _ color: CGColor) -> CTLine {
        var attrs: [NSAttributedString.Key: Any] = [.font: f, NSAttributedString.Key(kCTForegroundColorAttributeName as String): color]
        if I18n.lang == "zh-Hans" { attrs[NSAttributedString.Key(kCTLanguageAttributeName as String)] = "zh-Hans" }
        return CTLineCreateWithAttributedString(NSAttributedString(string: s, attributes: attrs))
    }

    static func measure(_ s: String, size: CGFloat, bold: Bool) -> CGSize {
        let f = font(size, bold: bold)
        let lines = s.components(separatedBy: "\n")
        var w: CGFloat = 0
        for l in lines where !l.isEmpty {
            w = max(w, CGFloat(CTLineGetTypographicBounds(ctLine(l, f, CGColor(gray: 0, alpha: 1)), nil, nil, nil)))
        }
        if s.isEmpty { w = size * 0.6 }
        return CGSize(width: ceil(w), height: lineHeight(f) * CGFloat(max(1, lines.count)))
    }

    /// 文字左上角在 origin。outline 非空时先画描边（宽 = 字号/8）。
    static func draw(_ ctx: CGContext, _ s: String, origin: CGPoint, size: CGFloat, bold: Bool, color: CGColor, outline: CGColor?, center: CGRect? = nil) {
        let f = font(size, bold: bold)
        let lh = lineHeight(f)
        let lines = s.components(separatedBy: "\n")
        var top = origin.y
        if let c = center { top = c.midY - lh * CGFloat(lines.count) / 2 }
        for l in lines {
            defer { top += lh }
            if l.isEmpty { continue }
            let line = ctLine(l, f, color)
            var x = origin.x
            if let c = center { x = c.midX - CGFloat(CTLineGetTypographicBounds(line, nil, nil, nil)) / 2 }
            let baseline = top + f.ascender + f.leading / 2
            if let oc = outline {
                let path = glyphPath(line, x: x, baseline: baseline)
                ctx.saveGState()
                ctx.addPath(path)
                ctx.setStrokeColor(oc)
                ctx.setLineWidth(size / 8)
                ctx.setLineJoin(.round)
                ctx.setLineCap(.round)
                ctx.strokePath()
                ctx.restoreGState()
            }
            Draw.drawLine(ctx, line, x: x, baseline: baseline)
        }
    }

    /// 一行文字的字形轮廓（翻转坐标）。
    static func glyphPath(_ line: CTLine, x: CGFloat, baseline: CGFloat) -> CGPath {
        let path = CGMutablePath()
        guard let runs = CTLineGetGlyphRuns(line) as? [CTRun] else { return path }
        for run in runs {
            let attrs = CTRunGetAttributes(run) as NSDictionary
            guard let fontRef = attrs[kCTFontAttributeName as String] else { continue }
            let runFont = fontRef as! CTFont
            let n = CTRunGetGlyphCount(run)
            var glyphs = [CGGlyph](repeating: 0, count: n)
            var pos = [CGPoint](repeating: .zero, count: n)
            CTRunGetGlyphs(run, CFRange(location: 0, length: n), &glyphs)
            CTRunGetPositions(run, CFRange(location: 0, length: n), &pos)
            for i in 0..<n {
                guard let gp = CTFontCreatePathForGlyph(runFont, glyphs[i], nil) else { continue }
                let t = CGAffineTransform(translationX: x + pos[i].x, y: baseline - pos[i].y).scaledBy(x: 1, y: -1)
                path.addPath(gp, transform: t)
            }
        }
        return path
    }
}

/// 标注绘制（spec 2.5）。屏幕、导出、自测、预览共用。
enum AnnotRender {
    /// 画全部标注（调用方负责裁剪到选区）。dirty 非空时跳过不相交的标注。
    static func drawAll(_ ctx: CGContext, session: Session, dirty: CGRect? = nil) {
        let n = session.shapeCount
        for i in 0..<n {
            let v = session.shape(i)
            if let d = dirty, v.type != LS_SHAPE_MASK, !v.bounds.cg.insetBy(dx: -2, dy: -2).intersects(d) { continue }
            drawShape(ctx, v, frozen: session.frozen)
        }
    }

    static func drawShape(_ ctx: CGContext, _ v: ls_shape_view, frozen: FrozenImage?) {
        let st = v.style
        if v.type == LS_SHAPE_MASK {
            guard let fp = v.fill else { return }
            ctx.saveGState()
            ctx.addPath(Icons.cgPath(fp))
            let fc = v.fill_color.a > 0 ? Theme.cg(v.fill_color) : CGColor(gray: 0, alpha: 1)
            ctx.setFillColor(fc.alpha(CGFloat(st.opacity)))
            ctx.fillPath(using: .evenOdd)
            ctx.restoreGState()
            return
        }
        if v.type == Int32(LS_TOOL_MOSAIC.rawValue) {
            if let p = v.patch, let fz = frozen, let img = transientImage(p.pointee, space: fz.colorSpace) {
                ctx.saveGState()
                ctx.interpolationQuality = .none
                Draw.image(ctx, img, in: v.patch_rect.cg)
                ctx.restoreGState()
            }
            return
        }
        let opacity = CGFloat(max(0, min(1, st.opacity)))
        let layered = opacity < 0.999
        ctx.saveGState()
        if layered {
            ctx.setAlpha(opacity)
            ctx.beginTransparencyLayer(in: v.bounds.cg.insetBy(dx: -4, dy: -4), auxiliaryInfo: nil)
        }
        let color = Theme.cg(st.color)
        if v.type == Int32(LS_TOOL_MAGNIFY.rawValue), let fz = frozen {
            let r = CGFloat(v.radius)
            let circle = CGRect(x: CGFloat(v.p0.x) - r, y: CGFloat(v.p0.y) - r, width: 2 * r, height: 2 * r)
            ctx.saveGState()
            ctx.addEllipse(in: circle)
            ctx.clip()
            drawFrozen(ctx, fz, src: v.magnify_src.cg, dst: circle, smooth: true)
            ctx.restoreGState()
        }
        if v.type == Int32(LS_TOOL_TEXT.rawValue) && v.text_background.a > 0 {
            Draw.fill(ctx, v.text_rect.cg.insetBy(dx: -4, dy: -4), Theme.cg(v.text_background), radius: 4)
        }
        if let fp = v.fill {
            ctx.addPath(Icons.cgPath(fp))
            // fill_color：一般等于 style.color；空心序号为白色
            ctx.setFillColor(v.fill_color.a > 0 ? Theme.cg(v.fill_color) : color)
            ctx.fillPath(using: .evenOdd)
        }
        if let sp = v.stroke {
            let w = CGFloat(st.line_width)
            ctx.addPath(Icons.cgPath(sp))
            ctx.setStrokeColor(color)
            ctx.setLineWidth(w)
            ctx.setLineCap(.round)
            ctx.setLineJoin(.round)
            if v.dashed != 0 { ctx.setLineDash(phase: 0, lengths: [3 * w, 2 * w]) }
            ctx.strokePath()
            ctx.setLineDash(phase: 0, lengths: [])
        }
        if let t = v.text, v.editing == 0 {
            let s = String(cString: t)
            let tr = v.text_rect.cg
            let tc = Theme.cg(v.text_color)
            if v.type == Int32(LS_TOOL_STEP.rawValue) {
                AnnotText.draw(ctx, s, origin: tr.origin, size: CGFloat(v.text_size), bold: true, color: tc, outline: nil, center: tr)
            } else {
                let outline: CGColor? = v.text_outline.a > 0 ? Theme.cg(v.text_outline) : nil
                AnnotText.draw(ctx, s, origin: tr.origin, size: CGFloat(v.text_size), bold: st.bold != 0, color: tc, outline: outline)
            }
        }
        if layered { ctx.endTransparencyLayer() }
        ctx.restoreGState()
    }

    /// 把冻结画面的逻辑矩形 src 画到 dst（超出画面的部分留空）。
    static func drawFrozen(_ ctx: CGContext, _ fz: FrozenImage, src: CGRect, dst: CGRect, smooth: Bool) {
        let s = fz.scale
        let px = CGRect(x: src.minX * s, y: src.minY * s, width: src.width * s, height: src.height * s)
        let bounds = CGRect(x: 0, y: 0, width: fz.pixelWidth, height: fz.pixelHeight)
        let inter = px.intersection(bounds).integral.intersection(bounds)
        guard !inter.isEmpty, px.width > 0, px.height > 0, let crop = fz.cgImage.cropping(to: inter) else { return }
        let sx = dst.width / px.width, sy = dst.height / px.height
        let d = CGRect(x: dst.minX + (inter.minX - px.minX) * sx, y: dst.minY + (inter.minY - px.minY) * sy, width: inter.width * sx, height: inter.height * sy)
        ctx.saveGState()
        ctx.interpolationQuality = smooth ? .high : .none
        Draw.image(ctx, crop, in: d)
        ctx.restoreGState()
    }

    /// 选中标注的控制点：白底圆形 + 边框。
    static func drawShapeHandles(_ ctx: CGContext, _ pts: [CGPoint]) {
        let d = Theme.f("capture.handleSize")
        for p in pts {
            let r = CGRect(x: p.x - d / 2, y: p.y - d / 2, width: d, height: d)
            Draw.ellipse(ctx, r, fill: Theme.cap("shapeHandleFill"))
            Draw.ellipse(ctx, r, stroke: Theme.cap("shapeHandleBorder"), width: 1)
        }
    }

    /// 没有控制点的选中标注（画笔、文字、序号）用虚线外框示意。
    static func drawSelectionDash(_ ctx: CGContext, _ r: CGRect) {
        ctx.saveGState()
        ctx.setStrokeColor(Theme.cap("shapeSelectionDash"))
        ctx.setLineWidth(1)
        ctx.setLineDash(phase: 0, lengths: [4, 3])
        ctx.stroke(r.insetBy(dx: 0.5, dy: 0.5))
        ctx.restoreGState()
    }
}

/// 用内核几何画的小预览（样式面板的箭头样式、序号样式）。内核占位时为空。
enum KernelPreview {
    private static var cache: [String: CGImage] = [:]

    /// 在 2 倍虚拟尺寸上用内核生成几何（箭头头长等按真实比例），再缩小到 size 显示。
    static func image(tool: Int32, variant: Int32, size outSize: CGFloat, color: CGColor, scale outScale: CGFloat, lineWidth outWidth: Float) -> CGImage? {
        let key = "\(tool)-\(variant)-\(outSize)-\(outScale)-\(outWidth)-\(color.components ?? [])"
        if let c = cache[key] { return c }
        let size = outSize * 2, scale = outScale / 2, lineWidth = outWidth * 2
        guard let fz = FrozenImage.make(width: Int(size * scale), height: Int(size * scale), scale: scale, space: Theme.sRGB, draw: { _ in }) else { return nil }
        var opts = Session.Options()
        opts.lockSelection = true
        opts.initialSelection = CGRect(x: 0, y: 0, width: size, height: size)
        opts.detectWindows = false
        guard let s = Session(frozen: fz, size: CGSize(width: size, height: size), options: opts) else { return nil }
        var st = s.style(tool)
        st.color = Theme.lsColor(color)
        st.variant = variant
        st.opacity = 1
        st.line_width = lineWidth
        st.next_number = 1
        _ = s.setStyle(tool, st)
        _ = s.setTool(tool)
        let m = size * 0.2
        if tool == Int32(LS_TOOL_STEP.rawValue) {
            _ = s.mouseDown(CGPoint(x: size / 2, y: size / 2), button: Int32(LS_BUTTON_LEFT), mods: 0, clicks: 1)
            _ = s.mouseUp(CGPoint(x: size / 2, y: size / 2), button: Int32(LS_BUTTON_LEFT), mods: 0)
        } else {
            _ = s.mouseDown(CGPoint(x: m, y: size - m), button: Int32(LS_BUTTON_LEFT), mods: 0, clicks: 1)
            _ = s.mouseMove(CGPoint(x: size / 2, y: size / 2), mods: 0)
            _ = s.mouseMove(CGPoint(x: size - m, y: m), mods: 0)
            _ = s.mouseUp(CGPoint(x: size - m, y: m), button: Int32(LS_BUTTON_LEFT), mods: 0)
        }
        guard s.shapeCount > 0 else { return nil }
        let img = Offscreen.render(size: CGSize(width: size, height: size), scale: scale) { ctx in
            for i in 0..<s.shapeCount {
                var v = s.shape(i)
                v.selected = 0
                AnnotRender.drawShape(ctx, v, frozen: fz)
            }
        }
        if let img = img { cache[key] = img }
        return img
    }
}
