import AppKit
@_weakLinked import ScreenCaptureKit

/// 系统捕获（spec 2.1）：每屏冻结画面、窗口列表、单个窗口。
/// macOS 14+ 用 SCScreenshotManager；11–13 用 CGWindowListCreateImage（SDK 已标为不可用，运行时 dlsym 取）。
enum ScreenCapture {
    struct WindowInfo {
        var id: CGWindowID
        /// 全局坐标（主屏左上角为原点，y 向下）。
        var frame: CGRect
        var layer: Int
        var owner: String
        var pid: pid_t
    }

    static func displayID(_ screen: NSScreen) -> CGDirectDisplayID {
        (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value ?? CGMainDisplayID()
    }

    /// 显示器的稳定标识（上次区域按它存）。
    static func displayKey(_ id: CGDirectDisplayID) -> String {
        if let u = CGDisplayCreateUUIDFromDisplayID(id)?.takeRetainedValue(), let s = CFUUIDCreateString(nil, u) as String? { return s }
        return "display-\(id)"
    }

    static func screen(at p: CGPoint) -> NSScreen? {
        NSScreen.screens.first { NSMouseInRect(p, $0.frame, false) } ?? NSScreen.main
    }

    // MARK: 窗口列表

    /// 在屏的普通窗口（layer 0）以及菜单栏、程序坞，按 z 序从上到下，排除自己。
    static func windowList() -> [WindowInfo] {
        guard let arr = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] else { return [] }
        let me = ProcessInfo.processInfo.processIdentifier
        var out: [WindowInfo] = []
        for d in arr {
            guard let num = d[kCGWindowNumber as String] as? NSNumber,
                  let layer = (d[kCGWindowLayer as String] as? NSNumber)?.intValue,
                  let bd = d[kCGWindowBounds as String] as? NSDictionary,
                  let rect = CGRect(dictionaryRepresentation: bd) else { continue }
            let pid = (d[kCGWindowOwnerPID as String] as? NSNumber)?.int32Value ?? 0
            if pid == me { continue }
            let owner = d[kCGWindowOwnerName as String] as? String ?? ""
            let name = d[kCGWindowName as String] as? String ?? ""
            let alpha = (d[kCGWindowAlpha as String] as? NSNumber)?.doubleValue ?? 1
            if alpha <= 0.01 || rect.width < 24 || rect.height < 16 { continue }
            var keep = false
            if layer == 0 {
                keep = true
            } else if layer == Int(CGWindowLevelForKey(.mainMenuWindow)) && (name == "Menubar" || owner == "Window Server") {
                keep = true
            } else if layer == Int(CGWindowLevelForKey(.dockWindow)) && owner == "Dock" && min(rect.width, rect.height) < 300 {
                keep = true
            }
            if keep { out.append(WindowInfo(id: CGWindowID(num.uint32Value), frame: rect, layer: layer, owner: owner, pid: pid)) }
        }
        return out
    }

    /// 光标下最上层的普通窗口。
    static func windowUnderCursor() -> WindowInfo? {
        let m = NSEvent.mouseLocation
        let primaryH = NSScreen.screens.first?.frame.height ?? 0
        let p = CGPoint(x: m.x, y: primaryH - m.y)
        return windowList().first { $0.layer == 0 && $0.frame.contains(p) }
    }

    // MARK: 截屏

    private typealias CreateImageFn = @convention(c) (CGRect, UInt32, UInt32, UInt32) -> Unmanaged<CGImage>?

    private static let legacyCreateImage: CreateImageFn? = {
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage") else { return nil }
        return unsafeBitCast(sym, to: CreateImageFn.self)
    }()

    private static func legacyImage(_ rect: CGRect, list: UInt32, window: CGWindowID, options: UInt32) -> CGImage? {
        legacyCreateImage?(rect, list, window, options)?.takeRetainedValue()
    }

    /// 每块屏的冻结画面（像素分辨率）。完成回调在主线程。
    static func captureScreens(_ screens: [NSScreen], cursor: Bool, completion: @escaping ([CGDirectDisplayID: CGImage]) -> Void) {
        if #available(macOS 14.0, *) {
            // NSScreen 只在主线程读：先算好每屏的编号与像素尺寸
            let targets = screens.map { (displayID($0), CGSize(width: $0.frame.width * $0.backingScaleFactor, height: $0.frame.height * $0.backingScaleFactor)) }
            SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { content, _ in
                guard let content = content else {
                    DispatchQueue.main.async { completion(legacyScreens(screens, cursor: cursor)) }
                    return
                }
                let group = DispatchGroup()
                let lock = NSLock()
                var results: [CGDirectDisplayID: CGImage] = [:]
                for (id, size) in targets {
                    guard let display = content.displays.first(where: { $0.displayID == id }) else { continue }
                    let filter = SCContentFilter(display: display, excludingWindows: [])
                    let cfg = SCStreamConfiguration()
                    cfg.width = Int(size.width)
                    cfg.height = Int(size.height)
                    cfg.showsCursor = cursor
                    cfg.captureResolution = .best
                    cfg.pixelFormat = kCVPixelFormatType_32BGRA
                    group.enter()
                    SCScreenshotManager.captureImage(contentFilter: filter, configuration: cfg) { img, _ in
                        if let img = img { lock.lock(); results[id] = img; lock.unlock() }
                        group.leave()
                    }
                }
                group.notify(queue: .main) {
                    if results.count < screens.count {
                        let legacy = legacyScreens(screens.filter { results[displayID($0)] == nil }, cursor: cursor)
                        results.merge(legacy) { a, _ in a }
                    }
                    completion(results)
                }
            }
        } else {
            completion(legacyScreens(screens, cursor: cursor))
        }
    }

    private static func legacyScreens(_ screens: [NSScreen], cursor: Bool) -> [CGDirectDisplayID: CGImage] {
        var out: [CGDirectDisplayID: CGImage] = [:]
        for s in screens {
            let id = displayID(s)
            // kCGWindowListOptionOnScreenOnly = 1，kCGWindowImageBestResolution = 8
            guard var img = legacyImage(CGDisplayBounds(id), list: 1, window: 0, options: 8) else { continue }
            if cursor, let withCursor = drawCursor(on: img, screen: s) { img = withCursor }
            out[id] = img
        }
        return out
    }

    /// 老系统的截屏不带光标：按设置把当前光标画上去。
    private static func drawCursor(on img: CGImage, screen: NSScreen) -> CGImage? {
        guard let cur = NSCursor.currentSystem, let cgCur = cur.image.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return nil }
        let scale = CGFloat(img.width) / screen.frame.width
        let m = NSEvent.mouseLocation
        let lx = m.x - screen.frame.minX, ly = screen.frame.maxY - m.y
        guard let ctx = CGContext(data: nil, width: img.width, height: img.height, bitsPerComponent: 8, bytesPerRow: 0, space: img.colorSpace ?? Theme.sRGB,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
        let size = cur.image.size
        let r = CGRect(x: (lx - cur.hotSpot.x) * scale, y: CGFloat(img.height) - (ly - cur.hotSpot.y + size.height) * scale, width: size.width * scale, height: size.height * scale)
        ctx.draw(cgCur, in: r)
        return ctx.makeImage()
    }

    /// 单个窗口（不含系统阴影）。完成回调在主线程。
    static func captureWindow(_ id: CGWindowID, completion: @escaping (CGImage?) -> Void) {
        if #available(macOS 14.0, *) {
            SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { content, _ in
                guard let w = content?.windows.first(where: { $0.windowID == id }) else {
                    DispatchQueue.main.async { completion(legacyWindow(id)) }
                    return
                }
                let filter = SCContentFilter(desktopIndependentWindow: w)
                let cfg = SCStreamConfiguration()
                let scale = CGFloat(filter.pointPixelScale)
                cfg.width = Int(filter.contentRect.width * scale)
                cfg.height = Int(filter.contentRect.height * scale)
                cfg.showsCursor = false
                cfg.ignoreShadowsSingleWindow = true
                cfg.captureResolution = .best
                cfg.pixelFormat = kCVPixelFormatType_32BGRA
                SCScreenshotManager.captureImage(contentFilter: filter, configuration: cfg) { img, _ in
                    DispatchQueue.main.async { completion(img ?? legacyWindow(id)) }
                }
            }
        } else {
            completion(legacyWindow(id))
        }
    }

    private static func legacyWindow(_ id: CGWindowID) -> CGImage? {
        // kCGWindowListOptionIncludingWindow = 8；BoundsIgnoreFraming(1) | BestResolution(8)
        legacyImage(.null, list: 8, window: id, options: 1 | 8)
    }

    static var hasPermission: Bool {
        if #available(macOS 10.15, *) { return CGPreflightScreenCaptureAccess() }
        return true
    }
}
