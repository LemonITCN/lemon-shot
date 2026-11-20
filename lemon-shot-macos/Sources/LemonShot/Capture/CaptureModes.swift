import AppKit
import LemonShotCore

/// 其他截图方式（spec 2.9）：光标下窗口、全屏、延时、多窗口合成。上次区域与预设尺寸走截图层（CaptureController.start 的 initial）。
enum CaptureModes {
    /// 截取光标下的窗口：不进截图层，按 capture.windowCorners/Shadow 加圆角阴影，然后按 enterAction 输出。
    static func captureWindowUnderCursor() {
        guard !CaptureController.shared.isActive, CaptureController.ensurePermission() else { return }
        guard let w = ScreenCapture.windowUnderCursor() else {
            captureFullscreen()
            return
        }
        ScreenCapture.captureWindow(w.id) { img in
            guard let img = img else { return }
            let scale = max(1, CGFloat(img.width) / max(1, w.frame.width))
            let s = Settings.shared
            let framed = frameWindow(img, scale: scale, corners: s.bool("capture.windowCorners"), shadow: s.bool("capture.windowShadow"))
            let primaryH = NSScreen.screens.first?.frame.height ?? 0
            let pinFrame = CGRect(x: w.frame.minX, y: primaryH - w.frame.maxY, width: w.frame.width, height: w.frame.height)
            let kind = Export.output(kindFor: s.string("save.enterAction")) ?? .copy
            Export.output(Export.standardize(Export.Result(image: framed, scale: scale)), kind, pinFrame: pinFrame)
        }
    }

    /// 截取光标所在屏的整屏。
    static func captureFullscreen() {
        guard !CaptureController.shared.isActive, CaptureController.ensurePermission() else { return }
        guard let screen = ScreenCapture.screen(at: NSEvent.mouseLocation) else { return }
        Toast.dismiss(animated: false)
        ScreenCapture.captureScreens([screen], cursor: Settings.shared.bool("capture.captureCursor")) { images in
            guard let img = images[ScreenCapture.displayID(screen)] else { return }
            let scale = CGFloat(img.width) / screen.frame.width
            let kind = Export.output(kindFor: Settings.shared.string("save.enterAction")) ?? .copy
            Export.output(Export.standardize(Export.Result(image: img, scale: scale)), kind, pinFrame: screen.frame)
        }
    }

    /// 圆角 + 阴影（像素单位交给 ls_image_frame）。阴影向下偏移取阴影半径的 1/3。
    static func frameWindow(_ img: CGImage, scale: CGFloat, corners: Bool, shadow: Bool) -> CGImage {
        guard corners || shadow, let src = FrozenImage.from(img, scale: scale) else { return img }
        let s = Settings.shared
        let radius = corners ? Float(s.double("capture.windowCornerRadius") * Double(scale)) : 0
        let sr = shadow ? Float(s.double("capture.windowShadowRadius") * Double(scale)) : 0
        var input = src.image
        var out = ls_image()
        let color = Theme.lsColor(hex: s.string("capture.windowShadowColor"))
        guard ls_image_frame(&input, radius, sr, sr / 3, color, &out) == 0, let fz = FrozenImage.adopt(out, scale: scale, space: src.colorSpace) else { return img }
        return fz.cgImage
    }

    /// 多窗口：各窗口单独截图（同单窗口方式）按原位置合成在外接矩形内，透明背景。
    static func composeWindows(_ wins: [ScreenCapture.WindowInfo], scale: CGFloat, completion: @escaping (Export.Result?) -> Void) {
        guard !wins.isEmpty else { completion(nil); return }
        var images: [Int: CGImage] = [:]
        var pending = wins.count
        for (i, w) in wins.enumerated() {
            ScreenCapture.captureWindow(w.id) { img in
                if let img = img { images[i] = img }
                pending -= 1
                if pending == 0 { finishCompose(wins, images, scale: scale, completion: completion) }
            }
        }
    }

    private static func finishCompose(_ wins: [ScreenCapture.WindowInfo], _ images: [Int: CGImage], scale: CGFloat, completion: (Export.Result?) -> Void) {
        let s = Settings.shared
        let corners = s.bool("capture.multiWindowCorners"), shadow = s.bool("capture.multiWindowShadow")
        var union = CGRect.null
        for w in wins { union = union.union(w.frame) }
        // 阴影边距：外扩阴影半径
        let margin = shadow ? CGFloat(s.double("capture.windowShadowRadius")) : 0
        let canvas = union.insetBy(dx: -margin, dy: -margin)
        let pw = Int((canvas.width * scale).rounded()), ph = Int((canvas.height * scale).rounded())
        guard pw > 0, ph > 0, let ctx = Export.bitmapContext(width: pw, height: ph, space: Theme.sRGB) else { completion(nil); return }
        ctx.translateBy(x: 0, y: CGFloat(ph))
        ctx.scaleBy(x: 1, y: -1)
        // 按 z 序从下往上画（列表是选择顺序，这里按全局窗口列表的上下关系排）
        let z = ScreenCapture.windowList().map { $0.id }
        let order = wins.indices.sorted { a, b in
            (z.firstIndex(of: wins[a].id) ?? 0) > (z.firstIndex(of: wins[b].id) ?? 0)
        }
        for i in order {
            guard let raw = images[i] else { continue }
            let w = wins[i]
            let framed = frameWindow(raw, scale: scale, corners: corners, shadow: shadow)
            let dx = CGFloat(framed.width - raw.width) / 2, dy = CGFloat(framed.height - raw.height) / 2
            let x = (w.frame.minX - canvas.minX) * scale - dx
            let y = (w.frame.minY - canvas.minY) * scale - dy
            Draw.image(ctx, framed, in: CGRect(x: x, y: y, width: CGFloat(framed.width), height: CGFloat(framed.height)))
        }
        guard let img = ctx.makeImage() else { completion(nil); return }
        completion(Export.standardize(Export.Result(image: img, scale: scale)))
    }

    // MARK: 延时截图

    private static var countdownPanel: NSPanel?
    private static var countdownTimer: Timer?

    static func captureDelayed() {
        guard !CaptureController.shared.isActive, countdownPanel == nil else { return }
        var remaining = max(1, Settings.shared.int("capture.delaySeconds"))
        let size = Theme.f("recordBar.countdownSize")
        let w = CountdownWidget(number: remaining)
        w.frame = CGRect(x: 0, y: 0, width: size, height: size)
        let host = WidgetHost(frame: w.frame, root: w)
        let panel = NSPanel(contentRect: w.frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.ignoresMouseEvents = true
        panel.level = .screenSaver
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.sharingType = .none
        panel.isReleasedWhenClosed = false
        panel.contentView = host
        let sf = (ScreenCapture.screen(at: NSEvent.mouseLocation) ?? NSScreen.main)?.frame ?? .zero
        panel.setFrameOrigin(CGPoint(x: sf.midX - size / 2, y: sf.midY - size / 2))
        panel.orderFrontRegardless()
        countdownPanel = panel
        let sound = Settings.shared.bool("capture.delaySound")
        if sound { NSSound(named: "Tink")?.play() }
        countdownTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { t in
            remaining -= 1
            if remaining <= 0 {
                t.invalidate()
                countdownTimer = nil
                countdownPanel?.orderOut(nil)
                countdownPanel?.close()
                countdownPanel = nil
                // 等倒计时窗口真正离开屏幕再冻结画面
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) { CaptureController.shared.start() }
                return
            }
            w.number = remaining
            host.needsDisplay = true
            if sound { NSSound(named: "Tink")?.play() }
        }
    }
}

/// 延时截图倒计时：countdownSize 圆形 tipBg 底 + 大号数字。
final class CountdownWidget: Widget {
    var number: Int
    init(number: Int) { self.number = number }
    override func draw(_ ctx: CGContext) {
        Draw.ellipse(ctx, frame, fill: Theme.cap("tipBg"))
        Draw.text(ctx, String(number), font: Fonts.digits(frame.height * 0.46, .semibold), color: Theme.cap("tipText"), in: frame, align: .center, truncate: false)
    }
}
