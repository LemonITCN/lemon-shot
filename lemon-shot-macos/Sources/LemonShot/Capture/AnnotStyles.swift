import AppKit
import LemonShotCore

/// 标注样式与设置 annotate.<tool> 的互转。颜色字段：数字 = 调色板序号，字符串 = #RRGGBB 自定义色。
enum AnnotStyles {
    static let toolKeys = ["", "rect", "ellipse", "line", "arrow", "pen", "mosaic", "text", "step", "highlight", "magnify"]
    static let toolIcons = ["", "square", "circle", "slash", "move-up-right", "pencil", "grid-3x3", "type", "list-ordered", "focus", "zoom-in"]
    /// 粗细三档。
    static let widthLevels: [Float] = [2, 3, 6]
    static let widthDots: [CGFloat] = [2, 3, 5]
    static let magnifyZooms: [Float] = [1.5, 2, 3, 4]
    /// 圆角开关打开时的半径（逻辑点）。
    static let cornerOn: Float = 6

    static func key(_ tool: Int32) -> String { tool >= 1 && Int(tool) < toolKeys.count ? toolKeys[Int(tool)] : "" }

    static func palette() -> [String] {
        let arr = Settings.shared.value("annotate.palette")?.array?.compactMap { $0.string } ?? []
        return arr.isEmpty ? ["#FF3B30"] : arr
    }

    static func colorHex(_ v: JSON?) -> String {
        let pal = palette()
        if case .number(let n)? = v { return pal[max(0, min(pal.count - 1, Int(n)))] }
        if let s = v?.string, Theme.parseHex(s) != nil { return s }
        return pal[0]
    }

    static func style(for tool: Int32) -> ls_style {
        let k = "annotate." + key(tool)
        let s = Settings.shared
        var st = ls_style()
        st.color = Theme.lsColor(hex: colorHex(s.value(k + ".color")))
        st.opacity = 1
        st.line_width = 3
        st.zoom = 2
        st.next_number = 1
        st.font_size = 18
        st.block_size = 10
        if let o = s.value(k + ".opacity")?.double { st.opacity = Float(o) }
        switch Int(tool) {
        case 1, 2, 3, 4, 5:
            st.line_width = Float(s.value(k + ".lineWidth")?.double ?? 3)
            st.filled = (s.value(k + ".filled")?.bool ?? false) ? 1 : 0
            st.variant = Int32(s.value(k + ".variant")?.int ?? 0)
            st.corner_radius = Float(s.value(k + ".cornerRadius")?.double ?? 0)
        case 6:
            st.variant = Int32(s.value(k + ".variant")?.int ?? 0)
            st.block_size = Float(s.value(k + ".blockSize")?.double ?? 10)
            st.line_width = Float(s.value(k + ".brushSize")?.double ?? 24)
        case 7:
            st.variant = Int32(s.value(k + ".variant")?.int ?? 0)
            st.font_size = Float(s.value(k + ".fontSize")?.double ?? 18)
            st.bold = (s.value(k + ".bold")?.bool ?? false) ? 1 : 0
        case 8:
            st.variant = Int32(s.value(k + ".variant")?.int ?? 0)
            st.line_width = Float(s.value(k + ".size")?.double ?? 24)
            st.next_number = Int32(s.value(k + ".next")?.int ?? 1)
        case 9:
            st.opacity = Float(s.value(k + ".opacity")?.double ?? 0.55)
            st.corner_radius = Float(s.value(k + ".cornerRadius")?.double ?? 6)
        case 10:
            st.zoom = Float(s.value(k + ".zoom")?.double ?? 2)
            st.line_width = Float(s.value(k + ".lineWidth")?.double ?? 3)
        default: break
        }
        return st
    }

    /// 把颜色写成调色板序号（命中调色板时）或 #RRGGBB。
    static func colorJSON(_ c: ls_color) -> JSON {
        let hex = Theme.hexString(c)
        if let i = palette().firstIndex(where: { $0.uppercased() == hex }) { return .number(Double(i)) }
        return .string(hex)
    }

    /// 写回设置（只写该工具用到的字段；序号「下一个」不写回，每次截图从设置值开始）。
    static func save(_ st: ls_style, for tool: Int32) {
        let k = "annotate." + key(tool)
        let s = Settings.shared
        var obj = s.value(k)?.object ?? JObject()
        func num(_ v: Float) -> JSON { .number((Double(v) * 1000).rounded() / 1000) }
        switch Int(tool) {
        case 1, 2, 3, 4, 5:
            obj["color"] = colorJSON(st.color)
            obj["lineWidth"] = num(st.line_width)
            obj["opacity"] = num(st.opacity)
            if tool == 1 || tool == 2 { obj["filled"] = .bool(st.filled != 0) }
            if tool == 1 { obj["cornerRadius"] = num(st.corner_radius) }
            if tool >= 3 { obj["variant"] = .number(Double(st.variant)) }
        case 6:
            obj["variant"] = .number(Double(st.variant))
            obj["blockSize"] = num(st.block_size)
            obj["brushSize"] = num(st.line_width)
        case 7:
            obj["color"] = colorJSON(st.color)
            obj["fontSize"] = num(st.font_size)
            obj["opacity"] = num(st.opacity)
            obj["variant"] = .number(Double(st.variant))
            obj["bold"] = .bool(st.bold != 0)
        case 8:
            obj["color"] = colorJSON(st.color)
            obj["size"] = num(st.line_width)
            obj["opacity"] = num(st.opacity)
            obj["variant"] = .number(Double(st.variant))
        case 9:
            obj["opacity"] = num(st.opacity)
            obj["cornerRadius"] = num(st.corner_radius)
        case 10:
            obj["zoom"] = num(st.zoom)
            obj["lineWidth"] = num(st.line_width)
        default: return
        }
        s.set(k, .object(obj))
    }

    static func nearestLevel(_ v: Float, _ levels: [Float]) -> Int {
        var best = 0
        for (i, l) in levels.enumerated() where abs(l - v) < abs(levels[best] - v) { best = i }
        return best
    }
}
