import AppKit

/// 长截图结果里的图：按窗口宽度等比显示，只画可见部分（图可能有几万像素高）。
final class LongImageView: NSView {
    let image: CGImage
    init(image: CGImage, width: CGFloat) {
        self.image = image
        let h = (CGFloat(image.height) * width / CGFloat(max(1, image.width))).rounded()
        super.init(frame: CGRect(x: 0, y: 0, width: width, height: h))
    }
    required init?(coder: NSCoder) { fatalError() }
    override var isFlipped: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        let s = CGFloat(image.width) / bounds.width
        // 脏区换算到像素，裁出对应的一段再画
        let py0 = max(0, floor(dirtyRect.minY * s)), py1 = min(CGFloat(image.height), ceil(dirtyRect.maxY * s))
        guard py1 > py0, let part = image.cropping(to: CGRect(x: 0, y: py0, width: CGFloat(image.width), height: py1 - py0)) else { return }
        ctx.interpolationQuality = .high
        Draw.image(ctx, part, in: CGRect(x: 0, y: py0 / s, width: bounds.width, height: (py1 - py0) / s))
    }
}

/// 长截图结果窗口内容：标题（含高度）、底部复制/保存/贴图/关闭。图片区是原生滚动视图。
final class LongShotResultContent: Widget {
    let header: ToolHeader
    let footer = ToolFooter()
    let copy = ButtonW(L("action.copy"), kind: .primary)
    let save = ButtonW(L("action.save"))
    let pin = ButtonW(L("action.pin"))
    let close = ButtonW(L("common.close"))
    var imageRect = CGRect.zero
    static let footerH: CGFloat = 56

    init(heightPixels: Int) {
        header = ToolHeader(title: L("longshot.title"))
        header.subtitle = L("longshot.height", ["h": String(heightPixels)])
        super.init()
        add(header)
        for b in [copy, save, pin, close] { footer.add(b) }
        add(footer)
    }

    override func layout() {
        let f = frame
        let th = Theme.f("window.titleBarHeight")
        header.frame = CGRect(x: f.minX, y: f.minY, width: f.width, height: th)
        footer.frame = CGRect(x: f.minX, y: f.maxY - LongShotResultContent.footerH, width: f.width, height: LongShotResultContent.footerH)
        footer.layoutRight([copy, save, pin])
        let cs = close.preferredSize()
        close.frame = CGRect(x: f.minX + 16, y: footer.frame.midY - cs.height / 2, width: cs.width, height: cs.height)
        imageRect = CGRect(x: f.minX, y: header.frame.maxY, width: f.width, height: footer.frame.minY - header.frame.maxY)
    }
}

/// 长截图结果窗口（宽 = min(图宽, 屏宽 60%)，可滚动查看）。
final class LongShotResultController: ToolWindowController {
    private static var open: [LongShotResultController] = []
    let content: LongShotResultContent
    let image: CGImage
    let scale: CGFloat
    private let scroll = NSScrollView()
    private var imageView: LongImageView?

    static func show(image: CGImage, scale: CGFloat, screen: NSScreen) {
        let vis = (NSScreen.screens.contains(screen) ? screen : (NSScreen.main ?? screen)).visibleFrame
        let logicalW = CGFloat(image.width) / scale, logicalH = CGFloat(image.height) / scale
        let w = max(420, min(logicalW, vis.width * 0.6)).rounded()
        let shownH = logicalH * min(1, w / max(1, logicalW))
        let chrome = Theme.f("window.titleBarHeight") + LongShotResultContent.footerH
        let h = min(shownH + chrome, vis.height * 0.85).rounded()
        let c = LongShotResultController(image: image, scale: scale, size: CGSize(width: w, height: max(300, h)))
        open.append(c)
        c.show(center: true)
    }

    init(image: CGImage, scale: CGFloat, size: CGSize) {
        self.image = image
        self.scale = scale
        content = LongShotResultContent(heightPixels: image.height)
        super.init(size: size, minSize: CGSize(width: 420, height: 300), title: L("longshot.title"), root: content, resizable: true)
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
        scroll.autohidesScrollers = true
        host.addSubview(scroll)
        placeNative = { [weak self] in self?.placeImage() }
        content.copy.action = { [weak self] in
            guard let self = self else { return }
            Export.copy(Export.Result(image: self.image, scale: self.scale))
            Toast.show(L("notify.copied"))
        }
        content.save.action = { [weak self] in
            guard let self = self, let url = Export.save(Export.Result(image: self.image, scale: self.scale)) else { return }
            Toast.show(L("notify.saved")) { NSWorkspace.shared.activateFileViewerSelecting([url]) }
        }
        content.pin.action = { [weak self] in
            guard let self = self else { return }
            // 太高的图贴出来时缩到屏幕高度的 80% 以内
            let visH = (NSScreen.main?.visibleFrame.height ?? 800) * 0.8
            let s = max(self.scale, CGFloat(self.image.height) / max(1, visH))
            PinManager.shared.pin(image: self.image, scale: s, frame: nil, source: "longshot")
            self.window.close()
        }
        content.close.action = { [weak self] in self?.window.close() }
        onClose = { [weak self] in
            LongShotResultController.open.removeAll { $0 === self }
        }
    }

    private func placeImage() {
        let r = content.imageRect
        scroll.frame = r
        let w = scroll.contentSize.width
        // 图比窗口窄时按原始大小居中，不放大
        let logicalW = CGFloat(image.width) / scale
        let shownW = min(w, logicalW)
        if imageView == nil || abs((imageView?.frame.width ?? 0) - shownW) > 0.5 {
            let v = LongImageView(image: image, width: shownW)
            let doc = FlippedView(frame: CGRect(x: 0, y: 0, width: w, height: v.frame.height))
            v.frame.origin.x = ((w - shownW) / 2).rounded()
            doc.addSubview(v)
            scroll.documentView = doc
            imageView = v
        } else if let doc = scroll.documentView {
            doc.frame.size.width = w
            imageView?.frame.origin.x = ((w - shownW) / 2).rounded()
        }
    }
}
