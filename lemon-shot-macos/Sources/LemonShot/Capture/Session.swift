import AppKit
import LemonShotCore

/// 冻结画面：BGRA 预乘像素（内核借用）+ 同一块内存上的 CGImage（显示与导出用）。
/// 像素内存归 CGImage 所有，最后一个引用释放时才 free，所以图层还在显示时不会悬空。
final class FrozenImage {
    let image: ls_image
    let cgImage: CGImage
    let colorSpace: CGColorSpace
    /// 每逻辑点像素数。
    let scale: CGFloat
    var pixelWidth: Int { Int(image.width) }
    var pixelHeight: Int { Int(image.height) }
    var logicalSize: CGSize { CGSize(width: CGFloat(image.width) / scale, height: CGFloat(image.height) / scale) }

    private init(image: ls_image, cgImage: CGImage, colorSpace: CGColorSpace, scale: CGFloat) {
        self.image = image
        self.cgImage = cgImage
        self.colorSpace = colorSpace
        self.scale = scale
    }

    static func rgbSpace(_ s: CGColorSpace?) -> CGColorSpace {
        if let s = s, s.model == .rgb, s.supportsOutput { return s }
        return Theme.sRGB
    }

    /// 分配一块像素并用 draw 画进去（左上原点，像素坐标）。
    static func make(width: Int, height: Int, scale: CGFloat, space: CGColorSpace?, draw: (CGContext) -> Void) -> FrozenImage? {
        let cs = rgbSpace(space)
        var img = ls_image_alloc(Int32(width), Int32(height))
        guard let data = img.data else { return nil }
        let bi = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
        guard let ctx = CGContext(data: data, width: width, height: height, bitsPerComponent: 8, bytesPerRow: Int(img.stride), space: cs, bitmapInfo: bi) else {
            ls_image_free(&img)
            return nil
        }
        ctx.translateBy(x: 0, y: CGFloat(height))
        ctx.scaleBy(x: 1, y: -1)
        draw(ctx)
        guard let provider = CGDataProvider(dataInfo: nil, data: data, size: Int(img.stride) * height, releaseData: { _, d, _ in
            free(UnsafeMutableRawPointer(mutating: d))
        }), let cg = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: Int(img.stride), space: cs,
                              bitmapInfo: CGBitmapInfo(rawValue: bi), provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent) else {
            ls_image_free(&img)
            return nil
        }
        return FrozenImage(image: img, cgImage: cg, colorSpace: cs, scale: scale)
    }

    /// 从任意 CGImage 复制成内核格式（保持原色彩空间，不做颜色转换）。
    static func from(_ src: CGImage, scale: CGFloat) -> FrozenImage? {
        make(width: src.width, height: src.height, scale: scale, space: src.colorSpace) { ctx in
            ctx.interpolationQuality = .none
            Draw.image(ctx, src, in: CGRect(x: 0, y: 0, width: src.width, height: src.height))
        }
    }

    /// 内核新分配的图（ls_pixelate 等的输出）接管成 FrozenImage。
    static func adopt(_ img: ls_image, scale: CGFloat, space: CGColorSpace) -> FrozenImage? {
        guard let data = img.data else { return nil }
        let bi = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
        guard let provider = CGDataProvider(dataInfo: nil, data: data, size: Int(img.stride) * Int(img.height), releaseData: { _, d, _ in
            free(UnsafeMutableRawPointer(mutating: d))
        }), let cg = CGImage(width: Int(img.width), height: Int(img.height), bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: Int(img.stride), space: space,
                              bitmapInfo: CGBitmapInfo(rawValue: bi), provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent) else { return nil }
        return FrozenImage(image: img, cgImage: cg, colorSpace: space, scale: scale)
    }
}

/// 不复制像素、立即使用的临时 CGImage（标注的马赛克 patch）。调用方必须在内核数据失效前用完。
func transientImage(_ img: ls_image, space: CGColorSpace) -> CGImage? {
    guard let data = img.data, img.width > 0, img.height > 0 else { return nil }
    let bi = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
    guard let provider = CGDataProvider(dataInfo: nil, data: data, size: Int(img.stride) * Int(img.height), releaseData: { _, _, _ in }) else { return nil }
    return CGImage(width: Int(img.width), height: Int(img.height), bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: Int(img.stride), space: space,
                   bitmapInfo: CGBitmapInfo(rawValue: bi), provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
}

/// 截图会话的 Swift 包装：只转发，不自己判断交互。
final class Session {
    let ptr: OpaquePointer
    let frozen: FrozenImage
    let size: CGSize
    let scale: CGFloat

    struct Options {
        var lockSelection = false
        var initialSelection: CGRect?
        var detectWindows = Settings.shared.bool("capture.detectWindows")
        var rightClickExits = Settings.shared.bool("capture.rightClickExits")
    }

    init?(frozen: FrozenImage, size: CGSize, options: Options) {
        var cfg = ls_session_default_config()
        cfg.width = Float(size.width)
        cfg.height = Float(size.height)
        cfg.scale = Float(frozen.scale)
        cfg.base = frozen.image
        cfg.detect_windows = options.detectWindows ? 1 : 0
        cfg.right_click_exits = options.rightClickExits ? 1 : 0
        cfg.lock_selection = options.lockSelection ? 1 : 0
        cfg.handle_hit = Float(Theme.f("capture.handleHitSize"))
        cfg.edge_hit = Float(Theme.f("capture.edgeHitSize"))
        cfg.min_selection = Float(Theme.f("capture.minSelection"))
        if let r = options.initialSelection {
            cfg.has_initial_selection = 1
            cfg.initial_selection = ls_rect(x: Float(r.minX), y: Float(r.minY), w: Float(r.width), h: Float(r.height))
        }
        guard let p = ls_session_create(&cfg) else { return nil }
        ptr = p
        self.frozen = frozen
        self.size = size
        scale = frozen.scale
        ls_session_set_text_measurer(p, Session.measure, nil)
        for t in 1...10 {
            var st = AnnotStyles.style(for: Int32(t))
            _ = ls_session_set_style(p, Int32(t), &st)
        }
        for id in ShortcutTable.captureIDs {
            guard let b = ShortcutTable.binding(id), let key = KeyMap.sessionKey(forName: ShortcutTable.capture(id)) else { continue }
            ls_session_bind_key(p, b, key)
        }
    }

    deinit { ls_session_destroy(ptr) }

    private static let measure: ls_measure_text_fn = { _, utf8, size, bold, outW, outH in
        let s = utf8.map { String(cString: $0) } ?? ""
        let m = AnnotText.measure(s, size: CGFloat(size), bold: bold != 0)
        outW?.pointee = Float(m.width)
        outH?.pointee = Float(m.height)
    }

    // MARK: 输入

    static func mods(_ f: NSEvent.ModifierFlags) -> Int32 {
        var m: Int32 = 0
        if f.contains(.shift) { m |= Int32(LS_MOD_SHIFT) }
        if f.contains(.control) { m |= Int32(LS_MOD_CTRL) }
        if f.contains(.option) { m |= Int32(LS_MOD_ALT) }
        if f.contains(.command) { m |= Int32(LS_MOD_CMD) | Int32(LS_MOD_PRIMARY) }
        return m
    }

    func setWindows(_ wins: [ls_window]) {
        var w = wins
        ls_session_set_windows(ptr, &w, Int32(w.count))
    }

    func setLastArea(_ r: CGRect) { ls_session_set_last_area(ptr, ls_rect(x: Float(r.minX), y: Float(r.minY), w: Float(r.width), h: Float(r.height))) }

    func mouseDown(_ p: CGPoint, button: Int32, mods: Int32, clicks: Int) -> Int32 { ls_session_mouse_down(ptr, Float(p.x), Float(p.y), button, mods, Int32(clicks)) }
    func mouseMove(_ p: CGPoint, mods: Int32) -> Int32 { ls_session_mouse_move(ptr, Float(p.x), Float(p.y), mods) }
    func mouseUp(_ p: CGPoint, button: Int32, mods: Int32) -> Int32 { ls_session_mouse_up(ptr, Float(p.x), Float(p.y), button, mods) }
    func scroll(_ p: CGPoint, dy: CGFloat, mods: Int32) -> Int32 { ls_session_scroll(ptr, Float(p.x), Float(p.y), Float(dy), mods) }
    func keyDown(_ key: Int32, mods: Int32) -> Int32 { ls_session_key_down(ptr, key, mods) }
    func keyUp(_ key: Int32, mods: Int32) -> Int32 { ls_session_key_up(ptr, key, mods) }
    func modifiers(_ mods: Int32) -> Int32 { ls_session_modifiers(ptr, mods) }
    func mouseExit() -> Int32 { ls_session_mouse_exit(ptr) }

    // MARK: 命令

    var tool: Int32 { ls_session_get_tool(ptr) }
    func setTool(_ t: Int32) -> Int32 { ls_session_set_tool(ptr, t) }
    func style(_ tool: Int32) -> ls_style { var s = ls_style(); ls_session_get_style(ptr, tool, &s); return s }
    func setStyle(_ tool: Int32, _ s: ls_style) -> Int32 { var st = s; return ls_session_set_style(ptr, tool, &st) }
    func undo() -> Int32 { ls_session_undo(ptr) }
    func redo() -> Int32 { ls_session_redo(ptr) }
    func setSelection(_ r: CGRect) -> Int32 { ls_session_set_selection(ptr, ls_rect(x: Float(r.minX), y: Float(r.minY), w: Float(r.width), h: Float(r.height))) }
    func setToolbarVisible(_ v: Bool) -> Int32 { ls_session_set_toolbar_visible(ptr, v ? 1 : 0) }

    func takeActions() -> [ls_action] {
        var out: [ls_action] = []
        while true {
            let a = ls_session_take_action(ptr)
            if a == LS_ACTION_NONE || out.count > 32 { break }
            out.append(a)
        }
        return out
    }

    // MARK: 读取

    var state: ls_session_state { var s = ls_session_state(); ls_session_get_state(ptr, &s); return s }

    var selectionPixels: CGRect {
        var x: Int32 = 0, y: Int32 = 0, w: Int32 = 0, h: Int32 = 0
        ls_session_selection_pixels(ptr, &x, &y, &w, &h)
        return CGRect(x: Int(x), y: Int(y), width: Int(w), height: Int(h))
    }

    /// 这个点能不能抓住选区拉伸（控制点或边），能返回控制点编号，不能返回 nil。
    func selectionGrip(at p: CGPoint) -> Int32? {
        let g = ls_session_selection_grip_at(ptr, Float(p.x), Float(p.y))
        return g >= 0 ? g : nil
    }

    func selectedWindows() -> [Int64] {
        var ids = [Int64](repeating: 0, count: 64)
        let n = ls_session_selected_windows(ptr, &ids, 64)
        return Array(ids.prefix(Int(max(0, min(n, 64)))))
    }

    func pixel(at p: CGPoint) -> ls_color { ls_session_pixel_at(ptr, Float(p.x), Float(p.y)) }

    func placeToolbar(_ size: CGSize) -> CGRect { ls_session_place_toolbar(ptr, Float(size.width), Float(size.height), Float(Theme.f("capture.toolbarGap")), Float(Theme.f("capture.toolbarScreenMargin"))).cg }
    func placeSizeTip(_ size: CGSize) -> CGRect { ls_session_place_size_tip(ptr, Float(size.width), Float(size.height), Float(Theme.f("capture.tipGap"))).cg }
    func placeMagnifier(_ size: CGSize) -> CGRect { ls_session_place_magnifier(ptr, Float(size.width), Float(size.height), Float(Theme.f("capture.magnifier.offset"))).cg }
    func placePopover(toolbar: CGRect, anchorX: CGFloat, size: CGSize) -> CGRect {
        ls_session_place_popover(ptr, toolbar.ls, Float(anchorX), Float(size.width), Float(size.height), Float(Theme.f("capture.toolbarGap")), Float(Theme.f("capture.toolbarScreenMargin"))).cg
    }

    var shapeCount: Int { Int(ls_session_shape_count(ptr)) }

    func shape(_ i: Int) -> ls_shape_view {
        var v = ls_shape_view()
        ls_session_shape_at(ptr, Int32(i), &v)
        return v
    }

    func shapeHandles() -> [CGPoint] {
        var pts = [ls_point](repeating: ls_point(x: 0, y: 0), count: 9)
        let n = Int(ls_session_shape_handles(ptr, &pts, 9))
        return pts.prefix(max(0, min(n, 9))).map { CGPoint(x: CGFloat($0.x), y: CGFloat($0.y)) }
    }

    // MARK: 文字

    func textEdit() -> ls_text_edit? {
        var t = ls_text_edit()
        return ls_session_text_edit(ptr, &t) != 0 ? t : nil
    }

    func setText(_ s: String) -> Int32 { s.withCString { ls_session_set_text(ptr, $0) } }
    func endText() -> Int32 { ls_session_end_text(ptr) }
}

extension ls_rect {
    var cg: CGRect { CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(w), height: CGFloat(h)) }
}

extension CGRect {
    var ls: ls_rect { ls_rect(x: Float(minX), y: Float(minY), w: Float(width), h: Float(height)) }
}

extension ls_point {
    var cg: CGPoint { CGPoint(x: CGFloat(x), y: CGFloat(y)) }
}
