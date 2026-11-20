import AppKit

/// 带壳截图的机型（spec 7）。值写进设置 shell.model。
enum ShellModel: String, CaseIterable {
    case macbookAir, macbookPro14, macbookPro16, imac24, studioDisplay, laptop, monitor, iphone, ipad

    var titleKey: String { "shell.device.\(rawValue)" }

    /// 屏幕宽高比（横放）。
    var screenAspect: CGFloat {
        switch self {
        case .macbookAir: return 2560.0 / 1664.0
        case .macbookPro14: return 3024.0 / 1964.0
        case .macbookPro16: return 3456.0 / 2234.0
        case .imac24, .studioDisplay, .monitor: return 16.0 / 9.0
        case .laptop: return 16.0 / 10.0
        case .iphone: return 2556.0 / 1179.0
        case .ipad: return 2360.0 / 1640.0
        }
    }
}

/// 矢量自绘外壳：圆角机身、屏幕黑边、刘海/摄像头、底座。不用任何图片素材。
/// 所有尺寸按屏幕宽 W（像素）的比例给出，输出与截图同一像素密度。
enum ShellRenderer {
    private static let space = Theme.sRGB

    private static func rgb(_ hex: String) -> CGColor { Theme.hex(hex) }

    /// 套壳。screenshot 等比缩放、居中裁切进外壳屏幕区域；background：transparent / white / black。
    static func render(_ shot: CGImage, model: ShellModel, background: String) -> CGImage? {
        let aspect = model.screenAspect
        // 屏幕区域按截图像素 1:1（裁切后的那块），过大时限制在 4096 宽
        var sw = min(CGFloat(shot.width), CGFloat(shot.height) * aspect)
        sw = min(sw, 4096).rounded()
        let sh = (sw / aspect).rounded()
        let W = sw
        let screen0 = CGRect(x: 0, y: 0, width: sw, height: sh)
        let body = bounds(model, screen: screen0, W: W)
        let pad = (max(body.width, body.height) * 0.06).rounded()
        let canvas = CGSize(width: (body.width + 2 * pad).rounded(), height: (body.height + 2 * pad).rounded())
        let dx = pad - body.minX, dy = pad - body.minY
        let screen = screen0.offsetBy(dx: dx, dy: dy)
        guard let ctx = CGContext(data: nil, width: Int(canvas.width), height: Int(canvas.height), bitsPerComponent: 8, bytesPerRow: 0, space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        ctx.translateBy(x: 0, y: canvas.height)
        ctx.scaleBy(x: 1, y: -1)
        ctx.interpolationQuality = .high
        switch background {
        case "white": Draw.fill(ctx, CGRect(origin: .zero, size: canvas), rgb("#FFFFFF"))
        case "black": Draw.fill(ctx, CGRect(origin: .zero, size: canvas), rgb("#000000"))
        default: break
        }
        // 机身投影
        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: 0, height: -W * 0.012), blur: W * 0.04, color: CGColor(gray: 0, alpha: background == "black" ? 0 : 0.28))
        ctx.beginTransparencyLayer(auxiliaryInfo: nil)
        drawBody(ctx, model, screen: screen, W: W)
        drawScreen(ctx, shot, model: model, screen: screen, W: W)
        drawFront(ctx, model, screen: screen, W: W)
        ctx.endTransparencyLayer()
        ctx.restoreGState()
        return ctx.makeImage()
    }

    // MARK: 几何

    /// 屏幕四角圆角半径。
    private static func screenRadius(_ m: ShellModel, W: CGFloat, H: CGFloat) -> (top: CGFloat, bottom: CGFloat) {
        switch m {
        case .macbookAir, .macbookPro14, .macbookPro16: return (W * 0.0105, 0)
        case .imac24, .studioDisplay, .monitor, .laptop: return (0, 0)
        case .iphone: return (H * 0.17, H * 0.17)
        case .ipad: return (H * 0.035, H * 0.035)
        }
    }

    private struct Laptop {
        var side, top, bottom, lidRadius, baseH, baseOver: CGFloat
        var rim, bezel, baseTop, baseBottom, notch: String
        var hasNotch: Bool
    }

    private static func laptop(_ m: ShellModel) -> Laptop {
        switch m {
        case .macbookAir:
            return Laptop(side: 0.019, top: 0.03, bottom: 0.03, lidRadius: 0.028, baseH: 0.024, baseOver: 0.075,
                          rim: "#BFC1C6", bezel: "#0A0A0B", baseTop: "#DDDEE2", baseBottom: "#A7A9AE", notch: "#0A0A0B", hasNotch: true)
        case .macbookPro14, .macbookPro16:
            return Laptop(side: 0.02, top: 0.03, bottom: 0.032, lidRadius: 0.03, baseH: m == .macbookPro16 ? 0.03 : 0.028, baseOver: 0.07,
                          rim: "#3A3B3F", bezel: "#050506", baseTop: "#55575C", baseBottom: "#2A2B2E", notch: "#050506", hasNotch: true)
        default:
            return Laptop(side: 0.03, top: 0.045, bottom: 0.055, lidRadius: 0.022, baseH: 0.036, baseOver: 0.08,
                          rim: "#2E3034", bezel: "#111214", baseTop: "#5A5D63", baseBottom: "#33363B", notch: "#111214", hasNotch: false)
        }
    }

    /// 外壳总外接框（相对于屏幕区域所在坐标）。
    static func bounds(_ m: ShellModel, screen s: CGRect, W: CGFloat) -> CGRect {
        switch m {
        case .macbookAir, .macbookPro14, .macbookPro16, .laptop:
            let l = laptop(m)
            let lid = CGRect(x: s.minX - l.side * W, y: s.minY - l.top * W, width: s.width + 2 * l.side * W, height: s.height + (l.top + l.bottom) * W)
            let baseW = lid.width * (1 + 2 * l.baseOver)
            return CGRect(x: lid.midX - baseW / 2, y: lid.minY, width: baseW, height: lid.height + l.baseH * W)
        case .imac24:
            let b = 0.034 * W, chin = 0.15 * W
            let bodyR = CGRect(x: s.minX - b, y: s.minY - b, width: s.width + 2 * b, height: s.height + b + chin)
            return CGRect(x: bodyR.minX, y: bodyR.minY, width: bodyR.width, height: bodyR.height + 0.2 * W)
        case .studioDisplay:
            let b = 0.034 * W
            let bodyR = CGRect(x: s.minX - b, y: s.minY - b, width: s.width + 2 * b, height: s.height + 2 * b)
            return CGRect(x: bodyR.minX, y: bodyR.minY, width: bodyR.width, height: bodyR.height + 0.22 * W)
        case .monitor:
            let b = 0.012 * W, chin = 0.034 * W
            let bodyR = CGRect(x: s.minX - b, y: s.minY - b, width: s.width + 2 * b, height: s.height + b + chin)
            return CGRect(x: bodyR.minX, y: bodyR.minY, width: bodyR.width, height: bodyR.height + 0.17 * W)
        case .iphone:
            let b = s.height * 0.048
            return s.insetBy(dx: -b - s.height * 0.012, dy: -b - s.height * 0.012)
        case .ipad:
            let b = s.height * 0.05
            return s.insetBy(dx: -b, dy: -b)
        }
    }

    // MARK: 绘制

    private static func linear(_ ctx: CGContext, _ path: CGPath, _ c0: CGColor, _ c1: CGColor, vertical: Bool = true) {
        let r = path.boundingBox
        ctx.saveGState()
        ctx.addPath(path)
        ctx.clip()
        if let g = CGGradient(colorsSpace: space, colors: [c0, c1] as CFArray, locations: [0, 1]) {
            ctx.drawLinearGradient(g, start: CGPoint(x: r.minX, y: r.minY), end: vertical ? CGPoint(x: r.minX, y: r.maxY) : CGPoint(x: r.maxX, y: r.minY), options: [])
        }
        ctx.restoreGState()
    }

    private static func fillPath(_ ctx: CGContext, _ p: CGPath, _ c: CGColor) {
        ctx.addPath(p)
        ctx.setFillColor(c)
        ctx.fillPath()
    }

    /// 上下圆角不同的圆角矩形。
    private static func roundedRect(_ r: CGRect, top: CGFloat, bottom: CGFloat) -> CGPath {
        let p = CGMutablePath()
        let t = min(top, r.width / 2, r.height / 2), b = min(bottom, r.width / 2, r.height / 2)
        p.move(to: CGPoint(x: r.minX + t, y: r.minY))
        p.addLine(to: CGPoint(x: r.maxX - t, y: r.minY))
        if t > 0 { p.addArc(tangent1End: CGPoint(x: r.maxX, y: r.minY), tangent2End: CGPoint(x: r.maxX, y: r.minY + t), radius: t) }
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY - b))
        if b > 0 { p.addArc(tangent1End: CGPoint(x: r.maxX, y: r.maxY), tangent2End: CGPoint(x: r.maxX - b, y: r.maxY), radius: b) }
        p.addLine(to: CGPoint(x: r.minX + b, y: r.maxY))
        if b > 0 { p.addArc(tangent1End: CGPoint(x: r.minX, y: r.maxY), tangent2End: CGPoint(x: r.minX, y: r.maxY - b), radius: b) }
        p.addLine(to: CGPoint(x: r.minX, y: r.minY + t))
        if t > 0 { p.addArc(tangent1End: CGPoint(x: r.minX, y: r.minY), tangent2End: CGPoint(x: r.minX + t, y: r.minY), radius: t) }
        p.closeSubpath()
        return p
    }

    private static func camera(_ ctx: CGContext, center c: CGPoint, r: CGFloat) {
        Draw.ellipse(ctx, CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r), fill: rgb("#1B1D24"))
        let ir = r * 0.45
        Draw.ellipse(ctx, CGRect(x: c.x - ir, y: c.y - ir, width: 2 * ir, height: 2 * ir), fill: rgb("#2C3550"))
        let hr = r * 0.18
        Draw.ellipse(ctx, CGRect(x: c.x - ir * 0.4 - hr, y: c.y - ir * 0.4 - hr, width: 2 * hr, height: 2 * hr), fill: CGColor(gray: 1, alpha: 0.35))
    }

    private static func drawBody(_ ctx: CGContext, _ m: ShellModel, screen s: CGRect, W: CGFloat) {
        switch m {
        case .macbookAir, .macbookPro14, .macbookPro16, .laptop:
            let l = laptop(m)
            let lid = CGRect(x: s.minX - l.side * W, y: s.minY - l.top * W, width: s.width + 2 * l.side * W, height: s.height + (l.top + l.bottom) * W)
            // 底座：比屏幕宽一些的薄板，下缘圆角，正中开槽
            let baseW = lid.width * (1 + 2 * l.baseOver)
            let base = CGRect(x: lid.midX - baseW / 2, y: lid.maxY, width: baseW, height: l.baseH * W)
            let basePath = roundedRect(base, top: base.height * 0.15, bottom: base.height * 0.55)
            linear(ctx, basePath, rgb(l.baseTop), rgb(l.baseBottom))
            // 底座上缘高光
            Draw.fill(ctx, CGRect(x: base.minX + base.height * 0.3, y: base.minY, width: base.width - base.height * 0.6, height: max(1, W * 0.0012)), CGColor(gray: 1, alpha: 0.35))
            let notchW = W * 0.13, notchH = base.height * 0.32
            let notch = roundedRect(CGRect(x: base.midX - notchW / 2, y: base.minY, width: notchW, height: notchH), top: 0, bottom: notchH)
            fillPath(ctx, notch, rgb(l.baseBottom).alpha(0.9))
            // 转轴阴影
            Draw.fill(ctx, CGRect(x: lid.minX + lid.width * 0.02, y: lid.maxY - W * 0.002, width: lid.width * 0.96, height: W * 0.004), CGColor(gray: 0, alpha: 0.45))
            // 屏幕外壳：外缘金属边 + 黑色玻璃
            let lidPath = roundedRect(lid, top: l.lidRadius * W, bottom: W * 0.004)
            linear(ctx, lidPath, rgb(l.rim), rgb(l.rim).alpha(0.85))
            let glass = lid.insetBy(dx: W * 0.0035, dy: W * 0.0035)
            fillPath(ctx, roundedRect(glass, top: l.lidRadius * W - W * 0.0035, bottom: W * 0.002), rgb(l.bezel))
            if !l.hasNotch {
                camera(ctx, center: CGPoint(x: s.midX, y: s.minY - l.top * W / 2), r: W * 0.0035)
            }
        case .imac24:
            let b = 0.034 * W, chin = 0.15 * W
            let bodyR = CGRect(x: s.minX - b, y: s.minY - b, width: s.width + 2 * b, height: s.height + b + chin)
            // 支架：一片薄板 + 底座
            let standW = W * 0.24
            let stand = CGRect(x: bodyR.midX - standW / 2, y: bodyR.maxY - W * 0.01, width: standW, height: W * 0.19)
            linear(ctx, roundedRect(stand, top: 0, bottom: W * 0.004), rgb("#9DB8D8"), rgb("#7E9CC0"))
            let foot = CGRect(x: bodyR.midX - W * 0.15, y: stand.maxY - W * 0.004, width: W * 0.3, height: W * 0.014)
            linear(ctx, Draw.rounded(foot, foot.height / 2), rgb("#A9C3E0"), rgb("#7393BA"))
            // 机身：白色玻璃边 + 彩色下巴
            let bodyPath = Draw.rounded(bodyR, W * 0.014)
            ctx.saveGState()
            ctx.addPath(bodyPath)
            ctx.clip()
            Draw.fill(ctx, bodyR, rgb("#F4F5F7"))
            let chinR = CGRect(x: bodyR.minX, y: s.maxY + b, width: bodyR.width, height: bodyR.maxY - s.maxY - b)
            linear(ctx, CGPath(rect: chinR, transform: nil), rgb("#B9D0EA"), rgb("#93B3D8"))
            ctx.restoreGState()
            Draw.stroke(ctx, bodyR, CGColor(gray: 0, alpha: 0.12), width: max(1, W * 0.001), radius: W * 0.014)
            camera(ctx, center: CGPoint(x: s.midX, y: s.minY - b / 2), r: W * 0.0028)
        case .studioDisplay:
            let b = 0.034 * W
            let bodyR = CGRect(x: s.minX - b, y: s.minY - b, width: s.width + 2 * b, height: s.height + 2 * b)
            let neckW = W * 0.24
            let neck = CGRect(x: bodyR.midX - neckW / 2, y: bodyR.maxY - W * 0.02, width: neckW, height: W * 0.21)
            linear(ctx, roundedRect(neck, top: 0, bottom: W * 0.006), rgb("#D3D4D8"), rgb("#A6A8AD"))
            let foot = CGRect(x: bodyR.midX - W * 0.17, y: neck.maxY - W * 0.006, width: W * 0.34, height: W * 0.018)
            linear(ctx, Draw.rounded(foot, foot.height / 2), rgb("#DADBDF"), rgb("#9C9EA3"))
            let bodyPath = Draw.rounded(bodyR, W * 0.016)
            linear(ctx, bodyPath, rgb("#D2D3D7"), rgb("#B4B6BB"))
            let glass = bodyR.insetBy(dx: W * 0.004, dy: W * 0.004)
            fillPath(ctx, Draw.rounded(glass, W * 0.012), rgb("#0A0A0B"))
            camera(ctx, center: CGPoint(x: s.midX, y: s.minY - b / 2), r: W * 0.0028)
        case .monitor:
            let b = 0.012 * W, chin = 0.034 * W
            let bodyR = CGRect(x: s.minX - b, y: s.minY - b, width: s.width + 2 * b, height: s.height + b + chin)
            let neckW = W * 0.06
            let neck = CGRect(x: bodyR.midX - neckW / 2, y: bodyR.maxY - W * 0.01, width: neckW, height: W * 0.15)
            linear(ctx, CGPath(rect: neck, transform: nil), rgb("#2F3135"), rgb("#1E1F22"), vertical: false)
            let foot = CGRect(x: bodyR.midX - W * 0.16, y: neck.maxY - W * 0.008, width: W * 0.32, height: W * 0.028)
            linear(ctx, roundedRect(foot, top: foot.height * 0.6, bottom: foot.height * 0.3), rgb("#3A3C41"), rgb("#1C1D20"))
            linear(ctx, Draw.rounded(bodyR, W * 0.008), rgb("#26272B"), rgb("#18191B"))
            fillPath(ctx, Draw.rounded(CGRect(x: s.minX - W * 0.004, y: s.minY - W * 0.004, width: s.width + W * 0.008, height: s.height + W * 0.008), W * 0.003), rgb("#08080A"))
            // 下巴正中的电源指示灯
            Draw.ellipse(ctx, CGRect(x: s.midX - W * 0.002, y: s.maxY + chin / 2 - W * 0.002, width: W * 0.004, height: W * 0.004), fill: rgb("#4C82FF").alpha(0.8))
        case .iphone:
            let H = s.height
            let b = H * 0.048, rim = H * 0.012
            let outer = s.insetBy(dx: -b - rim, dy: -b - rim)
            let r = H * 0.17 + b + rim
            // 侧键：横放时音量键在下缘、电源键在上缘
            let key = rgb("#8D8983")
            let kt = rim * 0.9
            Draw.fill(ctx, CGRect(x: outer.minX + outer.width * 0.18, y: outer.maxY - 1, width: outer.width * 0.06, height: kt + 1), key, radius: kt / 2)
            Draw.fill(ctx, CGRect(x: outer.minX + outer.width * 0.27, y: outer.maxY - 1, width: outer.width * 0.09, height: kt + 1), key, radius: kt / 2)
            Draw.fill(ctx, CGRect(x: outer.minX + outer.width * 0.39, y: outer.maxY - 1, width: outer.width * 0.09, height: kt + 1), key, radius: kt / 2)
            Draw.fill(ctx, CGRect(x: outer.minX + outer.width * 0.28, y: outer.minY - kt, width: outer.width * 0.14, height: kt + 1), key, radius: kt / 2)
            linear(ctx, Draw.rounded(outer, r), rgb("#B7B2AA"), rgb("#7F7B75"))
            fillPath(ctx, Draw.rounded(outer.insetBy(dx: rim, dy: rim), r - rim), rgb("#050506"))
        case .ipad:
            let H = s.height
            let b = H * 0.05
            let outer = s.insetBy(dx: -b, dy: -b)
            let r = H * 0.035 + b
            let key = rgb("#6E7075")
            Draw.fill(ctx, CGRect(x: outer.maxX - outer.width * 0.12, y: outer.minY - H * 0.006, width: outer.width * 0.06, height: H * 0.008), key, radius: H * 0.004)
            linear(ctx, Draw.rounded(outer, r), rgb("#8A8C91"), rgb("#5E6065"))
            fillPath(ctx, Draw.rounded(outer.insetBy(dx: H * 0.006, dy: H * 0.006), r - H * 0.006), rgb("#0A0A0B"))
            camera(ctx, center: CGPoint(x: s.midX, y: s.minY - b / 2), r: H * 0.006)
        }
    }

    private static func drawScreen(_ ctx: CGContext, _ shot: CGImage, model: ShellModel, screen s: CGRect, W: CGFloat) {
        let rad = screenRadius(model, W: W, H: s.height)
        let clip = roundedRect(s, top: rad.top, bottom: rad.bottom)
        ctx.saveGState()
        ctx.addPath(clip)
        ctx.clip()
        Draw.fill(ctx, s, rgb("#000000"))
        // 等比缩放铺满，居中裁切
        let iw = CGFloat(shot.width), ih = CGFloat(shot.height)
        let k = max(s.width / iw, s.height / ih)
        let dw = iw * k, dh = ih * k
        Draw.image(ctx, shot, in: CGRect(x: s.midX - dw / 2, y: s.midY - dh / 2, width: dw, height: dh))
        ctx.restoreGState()
    }

    /// 压在画面上的部分：刘海、灵动岛。
    private static func drawFront(_ ctx: CGContext, _ m: ShellModel, screen s: CGRect, W: CGFloat) {
        switch m {
        case .macbookAir, .macbookPro14, .macbookPro16:
            let l = laptop(m)
            let nw = W * 0.072, nh = W * 0.0235
            let notch = roundedRect(CGRect(x: s.midX - nw / 2, y: s.minY - 1, width: nw, height: nh + 1), top: 0, bottom: nh * 0.36)
            fillPath(ctx, notch, rgb(l.notch))
            camera(ctx, center: CGPoint(x: s.midX, y: s.minY + nh * 0.48), r: W * 0.0028)
        case .iphone:
            let H = s.height
            let iw = W * 0.043, ih = H * 0.32
            let island = CGRect(x: s.minX + W * 0.014, y: s.midY - ih / 2, width: iw, height: ih)
            fillPath(ctx, Draw.rounded(island, iw / 2), rgb("#000000"))
            camera(ctx, center: CGPoint(x: island.midX, y: island.maxY - iw / 2), r: iw * 0.28)
        default:
            break
        }
    }
}
