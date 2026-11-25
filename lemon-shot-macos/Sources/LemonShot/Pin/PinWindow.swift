import AppKit
import LemonShotCore

/// 贴图窗内容（spec 3）：投影 + 圆角图片 + 1pt 边框 + 悬停按钮 + 缩放百分比。屏幕与自测共用。
final class PinWidget: Widget {
    var image: CGImage
    var corners: Bool
    var shadow: Bool
    var active = false
    var showButtons: Bool
    var zoomText: String?
    let buttons: [IconButton]
    var onButton: ((String) -> Void)?

    static var margin: CGFloat { Theme.f("pin.shadowBlur") + Theme.f("pin.shadowOffsetY") }
    static let buttonIcons = ["copy", "download", "pencil", "x"]
    // 悬停按钮（spec 3）：深色胶囊距贴图上、右边缘各 8pt，内边距 4pt，四个 24pt 按钮，图标 16pt
    static let buttonSize: CGFloat = 24
    static let buttonIconSize: CGFloat = 16
    static let barInset: CGFloat = 8
    static let barPadding: CGFloat = 4

    init(image: CGImage) {
        self.image = image
        let s = Settings.shared
        corners = s.bool("pin.corners")
        shadow = s.bool("pin.shadow")
        showButtons = false
        buttons = PinWidget.buttonIcons.map { IconButton(icon: $0, size: PinWidget.buttonSize, iconSize: PinWidget.buttonIconSize, radius: 6) }
        super.init()
        let tips = ["pin.copy", "pin.save", "pin.annotate", "pin.close"]
        for (i, b) in buttons.enumerated() {
            b.onDark = true
            b.lineWidth = 1.75
            b.tooltip = L(tips[i])
            b.isHidden = true
            b.action = { [weak self] in self?.onButton?(PinWidget.buttonIcons[i]) }
            add(b)
        }
    }

    override var isInteractive: Bool { true }

    var imageRect: CGRect { frame.insetBy(dx: PinWidget.margin, dy: PinWidget.margin) }
    var radius: CGFloat { corners ? CGFloat(Settings.shared.double("pin.cornerRadius")) : 0 }

    var buttonsRect: CGRect {
        let n = CGFloat(buttons.count)
        let pad = PinWidget.barPadding
        let w = n * PinWidget.buttonSize + 2 * pad
        let r = imageRect
        return CGRect(x: r.maxX - PinWidget.barInset - w, y: r.minY + PinWidget.barInset, width: w, height: PinWidget.buttonSize + 2 * pad)
    }

    override func layout() {
        let br = buttonsRect
        let pad = PinWidget.barPadding
        for (i, b) in buttons.enumerated() {
            b.frame = CGRect(x: br.minX + pad + CGFloat(i) * PinWidget.buttonSize, y: br.minY + pad, width: PinWidget.buttonSize, height: PinWidget.buttonSize)
            b.isHidden = !showButtons
        }
    }

    override func draw(_ ctx: CGContext) {
        let r = imageRect
        let rad = radius
        if shadow {
            ctx.saveGState()
            Draw.setShadow(ctx, offsetY: Theme.f("pin.shadowOffsetY"), blur: Theme.f("pin.shadowBlur"), color: Theme.color("shadow"))
            Draw.fill(ctx, r, CGColor(gray: 0.5, alpha: 1), radius: rad)
            ctx.restoreGState()
        }
        ctx.saveGState()
        if rad > 0 { ctx.addPath(Draw.rounded(r, rad)); ctx.clip() }
        ctx.interpolationQuality = .high
        Draw.image(ctx, image, in: r)
        ctx.restoreGState()
        let bw = Theme.f("pin.borderWidth")
        Draw.stroke(ctx, r.insetBy(dx: -bw, dy: -bw), Theme.tokenColor(active ? "pin.activeBorder" : "pin.inactiveBorder"), width: bw, radius: rad > 0 ? rad + bw : 0)
        if showButtons {
            let br = buttonsRect
            Draw.fill(ctx, br, Theme.cap("tipBg"), radius: br.height / 2)
        }
        drawChildren(ctx)
        if let z = zoomText {
            let font = Fonts.digits(15, .semibold)
            let size = CGSize(width: Draw.textWidth(z, font) + 24, height: 32)
            let zr = CGRect(x: r.midX - size.width / 2, y: r.midY - size.height / 2, width: size.width, height: size.height)
            Draw.fill(ctx, zr, Theme.cap("tipBg"), radius: 8)
            Draw.text(ctx, z, font: font, color: Theme.cap("tipText"), in: zr, align: .center, truncate: false)
        }
    }
}

/// 贴图窗口。
final class PinWindow: NSPanel {
    let pinID = UUID().uuidString
    private(set) var image: CGImage
    /// 100% 时每逻辑点像素数。
    private(set) var scale: CGFloat
    private(set) var zoom: CGFloat = 1
    let source: String
    let widget: PinWidget
    private let host: PinHost
    private var zoomHideWork: DispatchWorkItem?
    private var annotator: PinAnnotator?

    var imageSize: CGSize { CGSize(width: CGFloat(image.width) / scale * zoom, height: CGFloat(image.height) / scale * zoom) }
    /// 图片在屏幕上的位置（Cocoa 坐标）。
    var imageFrame: CGRect { frame.insetBy(dx: PinWidget.margin, dy: PinWidget.margin) }

    init(image: CGImage, scale: CGFloat, imageFrame: CGRect, source: String) {
        self.image = image
        self.scale = max(0.1, scale)
        self.source = source
        widget = PinWidget(image: image)
        let m = PinWidget.margin
        let full = imageFrame.insetBy(dx: -m, dy: -m)
        widget.frame = CGRect(origin: .zero, size: full.size)
        host = PinHost(frame: CGRect(origin: .zero, size: full.size), root: widget)
        super.init(contentRect: full, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        host.pin = self
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        level = .floating
        isReleasedWhenClosed = false
        hidesOnDeactivate = false
        isMovableByWindowBackground = false
        acceptsMouseMovedEvents = true
        alphaValue = CGFloat(max(0.1, Settings.shared.double("pin.opacity")))
        applyAllDesktops(Settings.shared.bool("pin.allDesktops"))
        contentView = host
        widget.layout()
        widget.onButton = { [weak self] name in self?.button(name) }
    }

    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }

    override func becomeKey() { super.becomeKey(); widget.active = true; host.needsDisplay = true }
    override func resignKey() { super.resignKey(); widget.active = false; host.needsDisplay = true }

    var allDesktops = false

    /// 偏好设置里改了圆角大小：已打开的贴图立刻重画
    func refreshAppearance() { host.needsDisplay = true }

    func applyAllDesktops(_ on: Bool) {
        allDesktops = on
        collectionBehavior = on ? [.canJoinAllSpaces, .fullScreenAuxiliary] : [.managed, .fullScreenAuxiliary]
    }

    // MARK: 尺寸

    /// 以屏幕点 anchor 为中心改变缩放。
    func setZoom(_ z: CGFloat, anchor: CGPoint? = nil) {
        let minZ = CGFloat(Theme.f("pin.zoomMin")), maxZ = CGFloat(Theme.f("pin.zoomMax"))
        let nz = max(minZ, min(maxZ, z))
        let minSide = Theme.f("pin.minSize")
        let old = imageFrame
        let newW = CGFloat(image.width) / scale * nz, newH = CGFloat(image.height) / scale * nz
        guard min(newW, newH) >= minSide || nz >= zoom else { return }
        let a = anchor ?? CGPoint(x: old.midX, y: old.midY)
        let fx = old.width > 0 ? (a.x - old.minX) / old.width : 0.5
        let fy = old.height > 0 ? (a.y - old.minY) / old.height : 0.5
        zoom = nz
        let r = CGRect(x: a.x - fx * newW, y: a.y - fy * newH, width: newW, height: newH)
        setImageFrame(r)
        showZoomText()
    }

    func setImageFrame(_ r: CGRect) {
        let m = PinWidget.margin
        let full = r.insetBy(dx: -m, dy: -m)
        setFrame(full, display: false)
        host.frame = CGRect(origin: .zero, size: full.size)
        widget.frame = host.bounds
        widget.layout()
        host.needsDisplay = true
    }

    private func showZoomText() {
        widget.zoomText = "\(Int((zoom * 100).rounded()))%"
        host.needsDisplay = true
        zoomHideWork?.cancel()
        let w = DispatchWorkItem { [weak self] in
            self?.widget.zoomText = nil
            self?.host.needsDisplay = true
        }
        zoomHideWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 1, execute: w)
    }

    func replaceImage(_ img: CGImage, scale s: CGFloat, keepCenter: Bool = true) {
        let c = CGPoint(x: imageFrame.midX, y: imageFrame.midY)
        image = img
        scale = s
        widget.image = img
        let size = imageSize
        setImageFrame(CGRect(x: c.x - size.width / 2, y: c.y - size.height / 2, width: size.width, height: size.height))
    }

    // MARK: 操作

    func copyImage() {
        Export.copy(Export.Result(image: image, scale: scale))
        Toast.show(L("notify.copied"))
    }

    func saveImage() {
        guard let url = Export.save(Export.Result(image: image, scale: scale)) else { return }
        Toast.show(L("notify.saved")) { NSWorkspace.shared.activateFileViewerSelecting([url]) }
    }

    func rotate(clockwise: Bool) {
        guard let src = FrozenImage.from(image, scale: scale) else { return }
        var input = src.image
        var out = ls_image()
        guard ls_image_rotate90(&input, clockwise ? 1 : 0, &out) == 0, let fz = FrozenImage.adopt(out, scale: scale, space: src.colorSpace) else { return }
        replaceImage(fz.cgImage, scale: scale)
    }

    func flip(horizontal: Bool) {
        guard let src = FrozenImage.from(image, scale: scale) else { return }
        var input = src.image
        var out = ls_image()
        guard ls_image_flip(&input, horizontal ? 1 : 0, &out) == 0, let fz = FrozenImage.adopt(out, scale: scale, space: src.colorSpace) else { return }
        replaceImage(fz.cgImage, scale: scale)
    }

    func annotate() {
        guard annotator == nil else { return }
        let a = PinAnnotator(pin: self)
        annotator = a
        a.onFinish = { [weak self] result in
            guard let self = self else { return }
            self.annotator = nil
            if let r = result {
                self.zoom = 1
                self.replaceImage(r.image, scale: r.scale)
            }
            self.orderFront(nil)
        }
        a.begin()
    }

    private func button(_ name: String) {
        switch name {
        case "copy": copyImage()
        case "download": saveImage()
        case "pencil": annotate()
        case "x": PinManager.shared.close(self)
        default: break
        }
    }

    func setButtonsVisible(_ v: Bool) {
        let show = v && Settings.shared.bool("pin.showButtons") && annotator == nil
        guard widget.showButtons != show else { return }
        widget.showButtons = show
        widget.layout()
        host.needsDisplay = true
    }

    func contextMenu() -> NSMenu {
        let m = NSMenu()
        m.autoenablesItems = false
        func item(_ key: String, _ action: @escaping () -> Void, checked: Bool = false) -> NSMenuItem {
            let it = ClosureMenuItem(title: L(key), action: action)
            it.state = checked ? .on : .off
            return it
        }
        m.addItem(item("pin.copy") { [weak self] in self?.copyImage() })
        m.addItem(item("pin.save") { [weak self] in self?.saveImage() })
        m.addItem(item("pin.annotate") { [weak self] in self?.annotate() })
        m.addItem(item("pin.ocr") { [weak self] in
            guard let self = self else { return }
            OCRFeature.recognize(pinImage: self.image, scale: self.scale)
        })
        m.addItem(.separator())
        let op = NSMenuItem(title: L("pin.opacity"), action: nil, keyEquivalent: "")
        let opMenu = NSMenu()
        for p in [100, 80, 60, 40, 20] {
            let it = ClosureMenuItem(title: L("common.percent", ["n": String(p)])) { [weak self] in self?.alphaValue = CGFloat(p) / 100 }
            it.state = abs(alphaValue * 100 - CGFloat(p)) < 1 ? .on : .off
            opMenu.addItem(it)
        }
        op.submenu = opMenu
        m.addItem(op)
        let zm = NSMenuItem(title: L("pin.zoom"), action: nil, keyEquivalent: "")
        let zMenu = NSMenu()
        zMenu.addItem(item("pin.actualSize") { [weak self] in self?.setZoom(1) })
        for p in [50, 200] {
            let it = ClosureMenuItem(title: L("common.percent", ["n": String(p)])) { [weak self] in self?.setZoom(CGFloat(p) / 100) }
            zMenu.addItem(it)
        }
        zm.submenu = zMenu
        m.addItem(zm)
        m.addItem(item("action.rotateLeft") { [weak self] in self?.rotate(clockwise: false) })
        m.addItem(item("action.rotateRight") { [weak self] in self?.rotate(clockwise: true) })
        m.addItem(item("action.flipH") { [weak self] in self?.flip(horizontal: true) })
        m.addItem(item("action.flipV") { [weak self] in self?.flip(horizontal: false) })
        m.addItem(.separator())
        m.addItem(item("pin.corners", { [weak self] in
            guard let self = self else { return }
            self.widget.corners.toggle()
            self.host.needsDisplay = true
        }, checked: widget.corners))
        m.addItem(item("pin.shadow", { [weak self] in
            guard let self = self else { return }
            self.widget.shadow.toggle()
            self.host.needsDisplay = true
        }, checked: widget.shadow))
        m.addItem(item("pin.allDesktops", { [weak self] in
            guard let self = self else { return }
            self.applyAllDesktops(!self.allDesktops)
        }, checked: allDesktops))
        m.addItem(.separator())
        m.addItem(item("pin.close") { [weak self] in if let s = self { PinManager.shared.close(s) } })
        m.addItem(item("pin.closeAll") { PinManager.shared.closeAll() })
        m.addItem(item("pin.hideAll") { PinManager.shared.setHidden(true) })
        return m
    }
}

/// 贴图窗的事件处理：拖动移动、滚轮缩放、主修饰键+滚轮调透明度、方向键、双击/Esc 关闭、主修饰键+C/S。
final class PinHost: WidgetHost {
    weak var pin: PinWindow?
    private var dragStart: CGPoint?
    private var originStart: CGPoint = .zero
    private var scrollAccum: CGFloat = 0

    private func inImage(_ p: CGPoint) -> Bool {
        guard let pin = pin else { return false }
        return pin.widget.imageRect.contains(p)
    }

    override func mouseMoved(with event: NSEvent) {
        pin?.setButtonsVisible(inImage(point(event)))
        super.mouseMoved(with: event)
    }

    override func mouseExited(with event: NSEvent) {
        pin?.setButtonsVisible(false)
        super.mouseExited(with: event)
    }

    override func mouseDown(with event: NSEvent) {
        let p = point(event)
        if let w = root.hitTest(p), w !== root {
            super.mouseDown(with: event)
            return
        }
        guard inImage(p), let pin = pin else { return }
        pin.makeKey()
        if event.clickCount == 2 {
            if Settings.shared.bool("pin.doubleClickCloses") { PinManager.shared.close(pin) }
            return
        }
        dragStart = NSEvent.mouseLocation
        originStart = pin.frame.origin
    }

    override func mouseDragged(with event: NSEvent) {
        guard let s = dragStart, let pin = pin else { super.mouseDragged(with: event); return }
        let m = NSEvent.mouseLocation
        pin.setFrameOrigin(CGPoint(x: originStart.x + m.x - s.x, y: originStart.y + m.y - s.y))
    }

    override func mouseUp(with event: NSEvent) {
        if dragStart != nil { dragStart = nil; return }
        super.mouseUp(with: event)
    }

    override func rightMouseDown(with event: NSEvent) {
        guard let pin = pin, inImage(point(event)) else { return }
        NSMenu.popUpContextMenu(pin.contextMenu(), with: event, for: self)
    }

    override func scrollWheel(with event: NSEvent) {
        guard let pin = pin else { return }
        var dy = event.scrollingDeltaY
        if event.isDirectionInvertedFromDevice { dy = -dy }
        if event.hasPreciseScrollingDeltas { dy /= 10 }
        // 触控板的精确滚动累计满一格再动
        scrollAccum += dy
        guard abs(scrollAccum) >= 1 || !event.hasPreciseScrollingDeltas else { return }
        let steps: CGFloat = scrollAccum > 0 ? 1 : -1
        scrollAccum = 0
        if event.modifierFlags.contains(.command) {
            pin.alphaValue = max(0.1, min(1, pin.alphaValue + Theme.f("pin.opacityStep") * (steps > 0 ? 1 : -1)))
        } else {
            pin.setZoom(pin.zoom + Theme.f("pin.zoomStep") * (steps > 0 ? 1 : -1), anchor: NSEvent.mouseLocation)
        }
    }

    override func keyDown(with event: NSEvent) {
        guard let pin = pin else { return }
        let step: CGFloat = event.modifierFlags.contains(.shift) ? 10 : 1
        var o = pin.frame.origin
        switch event.keyCode {
        case 123: o.x -= step
        case 124: o.x += step
        case 125: o.y -= step
        case 126: o.y += step
        case 53: PinManager.shared.close(pin); return
        default:
            if event.modifierFlags.contains(.command) {
                if event.keyCode == 8 { pin.copyImage(); return }
                if event.keyCode == 1 { pin.saveImage(); return }
            }
            super.keyDown(with: event)
            return
        }
        pin.setFrameOrigin(o)
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if event.type == .keyDown, event.modifierFlags.contains(.command), event.keyCode == 8 || event.keyCode == 1 {
            keyDown(with: event)
            return true
        }
        return super.performKeyEquivalent(with: event)
    }
}

/// 带闭包的菜单项。
final class ClosureMenuItem: NSMenuItem {
    private let handler: () -> Void
    init(title: String, action handler: @escaping () -> Void) {
        self.handler = handler
        super.init(title: title, action: #selector(run), keyEquivalent: "")
        target = self
    }
    required init(coder: NSCoder) { fatalError() }
    @objc private func run() { handler() }
}
