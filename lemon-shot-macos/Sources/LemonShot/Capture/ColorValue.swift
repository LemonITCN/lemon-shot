import AppKit
import LemonShotCore

/// 取色值（spec 9）：屏幕原始值按 color.space 换算，再按设置格式化。
enum ColorValue {
    static func spaceIndex(_ s: String) -> Int32? {
        switch s {
        case "srgb": return Int32(LS_SPACE_SRGB.rawValue)
        case "p3": return Int32(LS_SPACE_P3.rawValue)
        case "adobe": return Int32(LS_SPACE_ADOBE.rawValue)
        case "generic": return Int32(LS_SPACE_GENERIC.rawValue)
        default: return nil
        }
    }

    /// 屏幕色彩空间 → sRGB（ColorSync），再用内核换到目标空间。native 原样返回。
    static func converted(_ c: ls_color, frozen: FrozenImage?) -> ls_color {
        let space = Settings.shared.string("color.space")
        guard let target = spaceIndex(space) else { return c }
        var srgb = c
        if let fz = frozen, fz.colorSpace != Theme.sRGB,
           let src = CGColor(colorSpace: fz.colorSpace, components: [CGFloat(c.r) / 255, CGFloat(c.g) / 255, CGFloat(c.b) / 255, 1]),
           let conv = src.converted(to: Theme.sRGB, intent: .relativeColorimetric, options: nil), let comps = conv.components, comps.count >= 3 {
            func b(_ v: CGFloat) -> UInt8 { UInt8(max(0, min(255, (v * 255).rounded()))) }
            srgb = ls_color(r: b(comps[0]), g: b(comps[1]), b: b(comps[2]), a: 255)
        }
        if target == Int32(LS_SPACE_SRGB.rawValue) { return srgb }
        let inp: [Float] = [Float(srgb.r) / 255, Float(srgb.g) / 255, Float(srgb.b) / 255]
        var out: [Float] = [0, 0, 0]
        ls_color_convert(Int32(LS_SPACE_SRGB.rawValue), target, inp, &out)
        func b(_ v: Float) -> UInt8 { UInt8(max(0, min(255, (v * 255).rounded()))) }
        return ls_color(r: b(out[0]), g: b(out[1]), b: b(out[2]), a: 255)
    }

    private static func call(_ body: (UnsafeMutablePointer<CChar>, Int32) -> Int32) -> String {
        var buf = [CChar](repeating: 0, count: 256)
        let n = buf.withUnsafeMutableBufferPointer { body($0.baseAddress!, 256) }
        guard n > 0 else { return "" }
        return String(cString: buf)
    }

    static func hex(_ c: ls_color) -> String {
        let s = Settings.shared
        return call { ls_color_hex(c.r, c.g, c.b, s.bool("color.hexPrefix") ? 1 : 0, s.bool("color.hexUppercase") ? 1 : 0, $0, $1) }
    }

    static func rgb(_ c: ls_color) -> String { "\(c.r), \(c.g), \(c.b)" }

    static func custom(_ c: ls_color, pattern: String? = nil) -> String {
        let p = pattern ?? Settings.shared.string("color.customFormat")
        let upper: Int32 = Settings.shared.bool("color.hexUppercase") ? 1 : 0
        return p.withCString { pp in call { ls_color_format(pp, c.r, c.g, c.b, upper, $0, $1) } }
    }

    static func name(_ c: ls_color) -> String {
        let lang = I18n.lang
        return lang.withCString { lp in call { ls_color_name(c.r, c.g, c.b, lp, $0, $1) } }
    }
}
