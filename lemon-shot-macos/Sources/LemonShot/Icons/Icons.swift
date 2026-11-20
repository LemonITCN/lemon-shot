import AppKit
import LemonShotCore

/// 图标：icons.json 的 SVG 路径（24 网格）经内核 ls_svg_path_parse 解析成 CGPath，原生描边。
enum Icons {
    private static var source: [String: String] = {
        var out: [String: String] = [:]
        if let o = Resources.json("icons.json")["icons"]?.object {
            for k in o.keys { if let d = o[k]?["d"]?.string { out[k] = d } }
        }
        return out
    }()
    private static var cache: [String: CGPath] = [:]

    static func has(_ name: String) -> Bool { source[name] != nil }

    /// 24×24 网格坐标的路径。
    static func path(_ name: String) -> CGPath? {
        if let p = cache[name] { return p }
        guard let d = source[name] else { return nil }
        let p = cgPath(svg: absoluteSubpaths(d), scale: 1, dx: 0, dy: 0)
        cache[name] = p
        return p
    }

    /// icons.json 把 Lucide 的多个 <path>/<circle> 元素用空格拼成一条 d；每个元素开头的小写 m 在原元素里是绝对坐标，
    /// 拼接后会被当成相对上一段终点。这里把「空格 + m」还原成 M（Lucide 压缩后的路径内部命令前没有空格，不会误改）。
    /// 注意 m 后面隐含的坐标对是相对 lineto，改成 M 时要补一个 l。
    static func absoluteSubpaths(_ d: String) -> String {
        let c = Array(d.unicodeScalars)
        var out = String.UnicodeScalarView()
        func isSep(_ u: Unicode.Scalar) -> Bool { u == " " || u == "," || u == "\n" || u == "\t" }
        func isNumStart(_ u: Unicode.Scalar) -> Bool { (u >= "0" && u <= "9") || u == "-" || u == "+" || u == "." }
        /// 读一个 SVG 数字，返回结束位置。
        func number(_ start: Int) -> Int {
            var i = start
            if i < c.count, c[i] == "-" || c[i] == "+" { i += 1 }
            var dot = false
            while i < c.count {
                let u = c[i]
                if u >= "0" && u <= "9" { i += 1; continue }
                if u == "." && !dot { dot = true; i += 1; continue }
                if (u == "e" || u == "E") && i > start {
                    i += 1
                    if i < c.count, c[i] == "-" || c[i] == "+" { i += 1 }
                    continue
                }
                break
            }
            return i
        }
        var i = 0
        while i < c.count {
            if c[i] == "m" && i > 0 && c[i - 1] == " " {
                out.append("M")
                i += 1
                for _ in 0..<2 {
                    while i < c.count, isSep(c[i]) { out.append(c[i]); i += 1 }
                    let e = number(i)
                    if e == i { break }
                    out.append(contentsOf: c[i..<e])
                    i = e
                }
                var j = i
                while j < c.count, isSep(c[j]) { j += 1 }
                if j < c.count, isNumStart(c[j]) { out.append(contentsOf: " l".unicodeScalars) }
                continue
            }
            out.append(c[i])
            i += 1
        }
        return String(out)
    }

    static func cgPath(svg d: String, scale: Float, dx: Float, dy: Float) -> CGPath {
        guard let lp = ls_path_create() else { return CGMutablePath() }
        defer { ls_path_destroy(lp) }
        _ = d.withCString { ls_svg_path_parse($0, scale, dx, dy, lp) }
        return cgPath(lp)
    }

    /// 内核路径转 CGPath（标注绘制也用它）。
    static func cgPath(_ lp: OpaquePointer?) -> CGPath {
        let path = CGMutablePath()
        guard let lp = lp else { return path }
        let n = ls_path_count(lp)
        var pts = [ls_point](repeating: ls_point(x: 0, y: 0), count: 3)
        var hasCurrent = false
        for i in 0..<n {
            let verb = ls_path_get(lp, i, &pts)
            switch verb {
            case LS_PATH_MOVE:
                path.move(to: CGPoint(x: CGFloat(pts[0].x), y: CGFloat(pts[0].y)))
                hasCurrent = true
            case LS_PATH_LINE:
                let p = CGPoint(x: CGFloat(pts[0].x), y: CGFloat(pts[0].y))
                if hasCurrent { path.addLine(to: p) } else { path.move(to: p); hasCurrent = true }
            case LS_PATH_CUBIC:
                let c1 = CGPoint(x: CGFloat(pts[0].x), y: CGFloat(pts[0].y))
                let c2 = CGPoint(x: CGFloat(pts[1].x), y: CGFloat(pts[1].y))
                let p = CGPoint(x: CGFloat(pts[2].x), y: CGFloat(pts[2].y))
                if !hasCurrent { path.move(to: c1); hasCurrent = true }
                path.addCurve(to: p, control1: c1, control2: c2)
            case LS_PATH_CLOSE:
                if hasCurrent { path.closeSubpath() }
            default: break
            }
        }
        return path
    }

    static var defaultStroke: CGFloat { Theme.f("toolbar.iconStroke") }

    /// 在 rect 内画图标。lineWidth 是缩放到实际尺寸后的逻辑线宽。
    static func draw(_ ctx: CGContext, _ name: String, in rect: CGRect, color: CGColor, lineWidth: CGFloat? = nil, fill: CGColor? = nil) {
        guard let p = path(name) else { return }
        let s = min(rect.width, rect.height) / 24
        var t = CGAffineTransform(translationX: rect.midX - 12 * s, y: rect.midY - 12 * s).scaledBy(x: s, y: s)
        guard let scaled = p.copy(using: &t) else { return }
        ctx.saveGState()
        if let fill = fill {
            ctx.addPath(scaled)
            ctx.setFillColor(fill)
            ctx.fillPath(using: .evenOdd)
        }
        ctx.addPath(scaled)
        ctx.setStrokeColor(color)
        ctx.setLineWidth(lineWidth ?? defaultStroke)
        ctx.setLineCap(.round)
        ctx.setLineJoin(.round)
        ctx.strokePath()
        ctx.restoreGState()
    }

    /// 菜单栏图标（spec 12）：剪刀单色剪影（brand/glyph-18.png 与 @2x 的 glyph-36.png），模板图随系统深浅色。
    /// 与 Windows 托盘同一形状；资源缺失时退回取景框加中心圆点。
    static func statusBarImage(size: CGFloat = 18) -> NSImage {
        let img = NSImage(size: NSSize(width: size, height: size))
        for px in [18, 36] {
            if let data = try? Data(contentsOf: Resources.url("brand/glyph-\(px).png")),
               let rep = NSBitmapImageRep(data: data) {
                rep.size = NSSize(width: size, height: size)
                img.addRepresentation(rep)
            }
        }
        if img.representations.isEmpty {
            return NSImage(size: NSSize(width: size, height: size), flipped: true) { rect in
                guard let ctx = NSGraphicsContext.current?.cgContext else { return false }
                let black = CGColor(gray: 0, alpha: 1)
                draw(ctx, "scan", in: rect, color: black, lineWidth: 1.6)
                let r = size / 24 * 2.6
                ctx.setFillColor(black)
                ctx.fillEllipse(in: CGRect(x: rect.midX - r, y: rect.midY - r, width: 2 * r, height: 2 * r))
                return true
            }.withTemplate()
        }
        img.isTemplate = true
        return img
    }

    /// 菜单栏等需要 NSImage 的地方。template 为真时只用作形状（系统着色）。
    static func image(_ name: String, size: CGFloat, color: CGColor = CGColor(gray: 0, alpha: 1), lineWidth: CGFloat? = nil, template: Bool = false) -> NSImage {
        let img = NSImage(size: NSSize(width: size, height: size), flipped: true) { rect in
            guard let ctx = NSGraphicsContext.current?.cgContext else { return false }
            draw(ctx, name, in: rect, color: color, lineWidth: lineWidth)
            return true
        }
        img.isTemplate = template
        return img
    }
}

private extension NSImage {
    func withTemplate() -> NSImage { isTemplate = true; return self }
}
