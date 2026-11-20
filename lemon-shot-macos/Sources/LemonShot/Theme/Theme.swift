import AppKit
import LemonShotCore

/// 界面令牌（tokens.json）：颜色、尺寸、字体。截图层与标注颜色不随主题变。
enum Theme {
    static let didChange = Notification.Name("cn.lemonit.shot.themeDidChange")
    private(set) static var tokens: JSON = Resources.json("tokens.json")
    /// 自测时强制主题。
    static var forcedDark: Bool?
    private static var colorCache: [String: CGColor] = [:]
    private static var numCache: [String: CGFloat] = [:]

    static func reload() {
        tokens = Resources.json("tokens.json")
        colorCache.removeAll()
        numCache.removeAll()
    }

    static var isDark: Bool {
        if let f = forcedDark { return f }
        switch Settings.shared.string("general.theme") {
        case "dark": return true
        case "light": return false
        default:
            if let app = NSApp {
                return app.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
            }
            if let style = UserDefaults.standard.string(forKey: "AppleInterfaceStyle") { return style.lowercased() == "dark" }
            return false
        }
    }

    /// 窗口外观：跟随系统时为 nil（让系统决定），强制浅色/深色时固定。
    static var appearance: NSAppearance? {
        if forcedDark == nil && Settings.shared.string("general.theme") == "system" { return nil }
        return NSAppearance(named: isDark ? .darkAqua : .aqua)
    }

    /// 主题色（随浅色/深色变化）。
    static func color(_ name: String) -> CGColor {
        let palette = isDark ? "dark" : "light"
        return cached("\(palette).\(name)") { hex(tokens.at("color.\(palette).\(name)")?.string ?? "#FF00FF") }
    }

    /// 截图层颜色（固定）。
    static func cap(_ name: String) -> CGColor {
        cached("capture.\(name)") { hex(tokens.at("color.capture.\(name)")?.string ?? "#FF00FF") }
    }

    /// 任意路径上的颜色令牌，例如 pin.activeBorder。
    static func tokenColor(_ path: String) -> CGColor {
        cached("t.\(path)") { hex(tokens.at(path)?.string ?? "#FF00FF") }
    }

    static func f(_ path: String) -> CGFloat {
        if let v = numCache[path] { return v }
        let v = CGFloat(tokens.at(path)?.double ?? 0)
        numCache[path] = v
        return v
    }

    private static func cached(_ key: String, _ make: () -> CGColor) -> CGColor {
        if let c = colorCache[key] { return c }
        let c = make()
        colorCache[key] = c
        return c
    }

    static let sRGB = CGColorSpace(name: CGColorSpace.sRGB)!

    /// #RRGGBB 或 #RRGGBBAA。
    static func hex(_ s: String) -> CGColor {
        let c = parseHex(s) ?? (255, 0, 255, 255)
        return CGColor(colorSpace: sRGB, components: [CGFloat(c.0) / 255, CGFloat(c.1) / 255, CGFloat(c.2) / 255, CGFloat(c.3) / 255])!
    }

    static func parseHex(_ s: String) -> (UInt8, UInt8, UInt8, UInt8)? {
        var t = s.trimmingCharacters(in: .whitespaces)
        if t.hasPrefix("#") { t.removeFirst() }
        guard t.count == 6 || t.count == 8, let v = UInt64(t, radix: 16) else { return nil }
        if t.count == 6 { return (UInt8((v >> 16) & 255), UInt8((v >> 8) & 255), UInt8(v & 255), 255) }
        return (UInt8((v >> 24) & 255), UInt8((v >> 16) & 255), UInt8((v >> 8) & 255), UInt8(v & 255))
    }

    static func hexString(_ c: ls_color, alpha: Bool = false) -> String {
        alpha ? String(format: "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a) : String(format: "#%02X%02X%02X", c.r, c.g, c.b)
    }

    static func lsColor(hex s: String) -> ls_color {
        let c = parseHex(s) ?? (255, 0, 0, 255)
        return ls_color(r: c.0, g: c.1, b: c.2, a: c.3)
    }

    static func cg(_ c: ls_color, alpha: CGFloat = 1) -> CGColor {
        CGColor(colorSpace: sRGB, components: [CGFloat(c.r) / 255, CGFloat(c.g) / 255, CGFloat(c.b) / 255, CGFloat(c.a) / 255 * alpha])!
    }

    static func lsColor(_ c: CGColor) -> ls_color {
        let s = c.converted(to: sRGB, intent: .defaultIntent, options: nil) ?? c
        let comps = s.components ?? [0, 0, 0, 1]
        func b(_ v: CGFloat) -> UInt8 { UInt8(max(0, min(255, (v * 255).rounded()))) }
        if comps.count >= 4 { return ls_color(r: b(comps[0]), g: b(comps[1]), b: b(comps[2]), a: b(comps[3])) }
        if comps.count == 2 { return ls_color(r: b(comps[0]), g: b(comps[0]), b: b(comps[0]), a: b(comps[1])) }
        return ls_color(r: 0, g: 0, b: 0, a: 255)
    }
}

/// 字体：系统字体（中文自动落到苹方），字号来自 tokens.font.size。
enum Fonts {
    static var caption: CGFloat { Theme.f("font.size.caption") }
    static var body: CGFloat { Theme.f("font.size.body") }
    static var title: CGFloat { Theme.f("font.size.title") }
    static var header: CGFloat { Theme.f("font.size.header") }

    private static var cache: [String: NSFont] = [:]

    static func ui(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> NSFont {
        let key = "u\(size)-\(weight.rawValue)"
        if let f = cache[key] { return f }
        let f = NSFont.systemFont(ofSize: size, weight: weight)
        cache[key] = f
        return f
    }

    static func medium(_ size: CGFloat) -> NSFont { ui(size, .medium) }
    static func semibold(_ size: CGFloat) -> NSFont { ui(size, .semibold) }

    /// 等宽数字（尺寸提示、计时）。
    static func digits(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> NSFont {
        let key = "d\(size)-\(weight.rawValue)"
        if let f = cache[key] { return f }
        let f = NSFont.monospacedDigitSystemFont(ofSize: size, weight: weight)
        cache[key] = f
        return f
    }

    static func mono(_ size: CGFloat) -> NSFont {
        let key = "m\(size)"
        if let f = cache[key] { return f }
        let f: NSFont
        if #available(macOS 10.15, *) { f = NSFont.monospacedSystemFont(ofSize: size, weight: .regular) } else { f = NSFont.userFixedPitchFont(ofSize: size) ?? ui(size) }
        cache[key] = f
        return f
    }
}
