import AppKit
import CoreText

/// 自绘基础：所有界面都画进「左上原点、y 向下」的 CGContext，屏幕与自测共用。
enum Draw {
    static func rounded(_ r: CGRect, _ radius: CGFloat) -> CGPath {
        let rad = max(0, min(radius, min(r.width, r.height) / 2))
        return CGPath(roundedRect: r, cornerWidth: rad, cornerHeight: rad, transform: nil)
    }

    static func fill(_ ctx: CGContext, _ r: CGRect, _ color: CGColor, radius: CGFloat = 0) {
        ctx.setFillColor(color)
        if radius > 0 { ctx.addPath(rounded(r, radius)); ctx.fillPath() } else { ctx.fill(r) }
    }

    /// 内描边（线完全在 r 内）。
    static func stroke(_ ctx: CGContext, _ r: CGRect, _ color: CGColor, width: CGFloat = 1, radius: CGFloat = 0) {
        let rr = r.insetBy(dx: width / 2, dy: width / 2)
        ctx.setStrokeColor(color)
        ctx.setLineWidth(width)
        if radius > 0 { ctx.addPath(rounded(rr, max(0, radius - width / 2))) } else { ctx.addRect(rr) }
        ctx.strokePath()
    }

    static func ellipse(_ ctx: CGContext, _ r: CGRect, fill: CGColor? = nil, stroke: CGColor? = nil, width: CGFloat = 1) {
        if let f = fill { ctx.setFillColor(f); ctx.fillEllipse(in: r) }
        if let s = stroke {
            ctx.setStrokeColor(s)
            ctx.setLineWidth(width)
            ctx.strokeEllipse(in: r.insetBy(dx: width / 2, dy: width / 2))
        }
    }

    /// 带投影的圆角底板（工具栏、面板、气泡、toast 共用）。
    static func panel(_ ctx: CGContext, _ r: CGRect, radius: CGFloat, fill fillColor: CGColor, border: CGColor?, shadow: CGColor?, blur: CGFloat, offsetY: CGFloat) {
        if let sh = shadow, blur > 0 {
            ctx.saveGState()
            setShadow(ctx, offsetY: offsetY, blur: blur, color: sh)
            fill(ctx, r, fillColor, radius: radius)
            ctx.restoreGState()
        } else {
            fill(ctx, r, fillColor, radius: radius)
        }
        if let b = border { stroke(ctx, r, b, width: 1, radius: radius) }
    }

    /// 投影（spec 1「阴影」）：offsetY 向下为正（逻辑点），blur 按 CSS box-shadow 模糊半径理解（高斯 σ = blur / 2）。
    /// 契约里 Mac 写的是 CALayer.shadowRadius = blur / 2；这里不用 CALayer 阴影（离屏 render(in:) 实测 shadowRadius r
    /// 只得到 σ ≈ r/2，自测图对不上），屏幕与自测都用同一段 CG 投影画出同样的 σ。
    ///
    /// CG 投影参数不受 CTM 影响，单位是像素、y 向上（base space）。实测（macOS 27）：
    /// - CG 的 blur B 像素 ≈ 高斯 σ 0.47·B 像素，即 CSS 模糊半径的含义，所以 blur 直接乘像素倍数传入；
    /// - 「向下」不能从变换里推：屏幕上层支持视图的上下文 ctm 是单位阵、userSpaceToDeviceSpace 的 d < 0，
    ///   离屏翻转位图 ctm 的 d < 0、userSpaceToDeviceSpace 的 d 却 > 0，两边按任何一个算都会有一边投影朝上
    ///   （以前按 userSpaceToDeviceSpace 算，屏幕对、自测图朝上）。
    /// 所有界面都画在「左上原点、y 向下」的用户空间里，向下一律是 base space 的负 y，只从变换里取倍数。
    static func setShadow(_ ctx: CGContext, offsetY: CGFloat, blur: CGFloat, color: CGColor) {
        let t = ctx.userSpaceToDeviceSpaceTransform
        let s = sqrt(t.a * t.a + t.b * t.b)
        ctx.setShadow(offset: CGSize(width: 0, height: -offsetY * s), blur: blur * s, color: color)
    }

    // MARK: 文字

    static func attributed(_ s: String, _ font: NSFont, _ color: CGColor, kern: CGFloat = 0) -> NSAttributedString {
        var attrs: [NSAttributedString.Key: Any] = [
            .font: font,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String): color,
        ]
        if I18n.lang == "zh-Hans" { attrs[NSAttributedString.Key(kCTLanguageAttributeName as String)] = "zh-Hans" }
        if kern != 0 { attrs[.kern] = kern }
        return NSAttributedString(string: s, attributes: attrs)
    }

    struct LineMetrics { var width: CGFloat; var ascent: CGFloat; var descent: CGFloat; var leading: CGFloat
        var height: CGFloat { ascent + descent }
    }

    static func metrics(_ line: CTLine) -> LineMetrics {
        var a: CGFloat = 0, d: CGFloat = 0, l: CGFloat = 0
        let w = CGFloat(CTLineGetTypographicBounds(line, &a, &d, &l))
        return LineMetrics(width: w, ascent: a, descent: d, leading: l)
    }

    static func line(_ s: String, _ font: NSFont, _ color: CGColor) -> CTLine {
        CTLineCreateWithAttributedString(attributed(s, font, color))
    }

    static func textWidth(_ s: String, _ font: NSFont) -> CGFloat {
        ceil(metrics(line(s, font, CGColor(gray: 0, alpha: 1))).width)
    }

    /// 单行文字的视觉高度（按字体，不随内容变）。
    static func lineHeight(_ font: NSFont) -> CGFloat { ceil(font.ascender - font.descender) }

    /// 在基线位置画一行。
    static func drawLine(_ ctx: CGContext, _ line: CTLine, x: CGFloat, baseline: CGFloat) {
        ctx.saveGState()
        ctx.textMatrix = CGAffineTransform(scaleX: 1, y: -1)
        ctx.textPosition = CGPoint(x: x, y: baseline)
        CTLineDraw(line, ctx)
        ctx.restoreGState()
    }

    enum HAlign { case left, center, right }

    /// 单行文字，在 rect 内垂直居中，超宽时末尾省略。
    @discardableResult
    static func text(_ ctx: CGContext, _ s: String, font: NSFont, color: CGColor, in rect: CGRect, align: HAlign = .left, truncate: Bool = true) -> CGFloat {
        var ln = line(s, font, color)
        var m = metrics(ln)
        if truncate, m.width > rect.width + 0.5, rect.width > 0 {
            let token = CTLineCreateWithAttributedString(attributed("…", font, color))
            if let t = CTLineCreateTruncatedLine(ln, Double(rect.width), .end, token) { ln = t; m = metrics(ln) }
        }
        let x: CGFloat
        switch align {
        case .left: x = rect.minX
        case .center: x = rect.midX - m.width / 2
        case .right: x = rect.maxX - m.width
        }
        // 用字体自身的上升/下降居中，中英文混排时基线稳定
        let asc = font.ascender, desc = -font.descender
        let baseline = rect.midY + (asc - desc) / 2
        drawLine(ctx, ln, x: x, baseline: baseline)
        return m.width
    }

    /// 自动换行的多行文字，返回实际高度。draw 为假时只测量。
    @discardableResult
    static func wrapped(_ ctx: CGContext?, _ s: String, font: NSFont, color: CGColor, x: CGFloat, y: CGFloat, width: CGFloat, lineSpacing: CGFloat = 3, align: HAlign = .left) -> CGFloat {
        let lh = lineHeight(font) + lineSpacing
        var top = y
        for para in s.components(separatedBy: "\n") {
            let attr = attributed(para.isEmpty ? " " : para, font, color)
            let ts = CTTypesetterCreateWithAttributedString(attr)
            let len = attr.length
            var start = 0
            while start < len {
                var count = CTTypesetterSuggestLineBreak(ts, start, Double(width))
                if count <= 0 { count = 1 }
                let ln = CTTypesetterCreateLine(ts, CFRange(location: start, length: count))
                if let ctx = ctx {
                    let w = metrics(ln).width
                    let lx: CGFloat = align == .center ? x + (width - w) / 2 : (align == .right ? x + width - w : x)
                    drawLine(ctx, ln, x: lx, baseline: top + font.ascender + lineSpacing / 2)
                }
                top += lh
                start += count
            }
        }
        return top - y
    }

    static func wrappedSize(_ s: String, font: NSFont, maxWidth: CGFloat, lineSpacing: CGFloat = 3) -> CGSize {
        var w: CGFloat = 0
        for para in s.components(separatedBy: "\n") {
            let attr = attributed(para.isEmpty ? " " : para, font, CGColor(gray: 0, alpha: 1))
            let ts = CTTypesetterCreateWithAttributedString(attr)
            var start = 0
            while start < attr.length {
                var count = CTTypesetterSuggestLineBreak(ts, start, Double(maxWidth))
                if count <= 0 { count = 1 }
                let ln = CTTypesetterCreateLine(ts, CFRange(location: start, length: count))
                w = max(w, metrics(ln).width)
                start += count
            }
        }
        let h = wrapped(nil, s, font: font, color: CGColor(gray: 0, alpha: 1), x: 0, y: 0, width: maxWidth, lineSpacing: lineSpacing)
        return CGSize(width: ceil(min(w, maxWidth)), height: ceil(h))
    }

    // MARK: 位图

    /// 左上原点的位图上下文（BGRA 预乘），逻辑尺寸 × scale。
    static func bitmap(width: CGFloat, height: CGFloat, scale: CGFloat, space: CGColorSpace = Theme.sRGB) -> CGContext? {
        let pw = max(1, Int((width * scale).rounded())), ph = max(1, Int((height * scale).rounded()))
        guard let ctx = CGContext(data: nil, width: pw, height: ph, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        ctx.translateBy(x: 0, y: CGFloat(ph))
        ctx.scaleBy(x: scale, y: -scale)
        ctx.interpolationQuality = .high
        ctx.setShouldSmoothFonts(false)
        return ctx
    }

    /// 在翻转坐标下把 CGImage 正着画进 rect。
    static func image(_ ctx: CGContext, _ img: CGImage, in rect: CGRect) {
        ctx.saveGState()
        ctx.translateBy(x: rect.minX, y: rect.maxY)
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(img, in: CGRect(origin: .zero, size: rect.size))
        ctx.restoreGState()
    }

    static func writePNG(_ img: CGImage, to url: URL) -> Bool {
        guard let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else { return false }
        CGImageDestinationAddImage(dest, img, nil)
        return CGImageDestinationFinalize(dest)
    }
}

extension CGRect {
    var center: CGPoint { CGPoint(x: midX, y: midY) }
    func pixelAligned(_ scale: CGFloat) -> CGRect {
        CGRect(x: (minX * scale).rounded() / scale, y: (minY * scale).rounded() / scale,
               width: (width * scale).rounded() / scale, height: (height * scale).rounded() / scale)
    }
}

extension CGColor {
    func alpha(_ a: CGFloat) -> CGColor { copy(alpha: alpha * a) ?? self }
}
