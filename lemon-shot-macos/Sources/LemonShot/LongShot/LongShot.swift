import AppKit
import LemonShotCore

/// 长截图（spec 6）的入口。
enum LongShotFeature {
    /// 菜单「长截图」/ 全局快捷键：进截图层，选好区域后开始。
    static func start() {
        guard LongShotSession.current == nil else { return }
        CaptureController.shared.start(.longshot)
    }

    /// 截图层里选好区域后按 S 或工具栏「长截图」。region 为 Cocoa 全局坐标。
    static func start(region: CGRect, screen: NSScreen) {
        guard LongShotSession.current == nil, region.width >= 8, region.height >= 8 else { return }
        let s = LongShotSession(region: region, screen: screen)
        LongShotSession.current = s
        s.begin()
    }
}

/// 一次长截图：区域边框 + 面板，每 100ms 抓一帧交给内核拼接；可自动滚动；完成后打开结果窗口。
final class LongShotSession {
    static var current: LongShotSession?

    let region: CGRect
    let screen: NSScreen
    let scale: CGFloat
    private let grabber: RegionGrabber
    private let border: LongShotBorderWindow
    private let panel: LongShotPanelWindow
    private let widget: LongShotPanelWidget
    private let panelHost: WidgetHost

    /// 内核拼接器只在这条串行队列上用。
    private let queue = DispatchQueue(label: "cn.lemonit.shot.longshot", qos: .userInitiated)
    private var stitcher: OpaquePointer?
    private var timer: Timer?
    private var inFlight = false
    private var running = false
    private var stopped = false
    private var finished = false
    private var keyMonitor: Any?
    private var lastPreviewAt: TimeInterval = 0

    // 自动滚动
    private var autoScrolling = false
    private var unchangedCount = 0
    private var scrolledSinceMatch: CGFloat = 0
    private var step: CGFloat

    init(region: CGRect, screen: NSScreen) {
        self.region = region
        self.screen = screen
        scale = screen.backingScaleFactor
        grabber = RegionGrabber(region: region, screen: screen)
        border = LongShotBorderWindow(region: region)
        let speed = max(1, CGFloat(Settings.shared.double("longshot.scrollSpeed")))
        step = max(10, min(speed * 40, region.height * 0.4))

        let seen = Settings.shared.bool("longshot.tutorialSeen")
        widget = LongShotPanelWidget(mode: seen ? .running : .notes)
        let m = PopupPanel.shadowMargin
        let frame = LongShotSession.panelFrame(region: region, screen: screen)
        widget.frame = CGRect(x: m, y: m, width: frame.width, height: frame.height)
        widget.layout()
        let full = frame.insetBy(dx: -m, dy: -m)
        panelHost = WidgetHost(frame: CGRect(origin: .zero, size: full.size), root: widget)
        panel = LongShotPanelWindow(contentRect: full, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.level = .floating
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.isReleasedWhenClosed = false
        panel.hidesOnDeactivate = false
        panel.appearance = Theme.appearance
        panel.sharingType = .none
        panel.contentView = panelHost
    }

    /// 面板放在区域右侧，放不下放左侧，再放不下贴在区域内右侧；高度跟区域走（至少 380）。
    static func panelFrame(region: CGRect, screen: NSScreen) -> CGRect {
        let vis = screen.visibleFrame
        let w = LongShotPanelWidget.width
        let gap: CGFloat = 10 + Theme.f("recordBar.regionBorderWidth")
        let h = min(max(region.height, 380), vis.height - 16)
        var x: CGFloat
        if region.maxX + gap + w <= vis.maxX - 8 {
            x = region.maxX + gap
        } else if region.minX - gap - w >= vis.minX + 8 {
            x = region.minX - gap - w
        } else {
            x = region.maxX - w - 12
        }
        var y = region.maxY - h
        y = max(vis.minY + 8, min(y, vis.maxY - 8 - h))
        return CGRect(x: x.rounded(), y: y.rounded(), width: w, height: h.rounded())
    }

    func begin() {
        widget.autoButton.action = { [weak self] in self?.toggleAuto() }
        widget.finishButton.action = { [weak self] in self?.finish() }
        widget.cancelButton.action = { [weak self] in self?.cancel() }
        widget.notesCancelButton.action = { [weak self] in self?.cancel() }
        widget.gotItButton.action = { [weak self] in
            Settings.shared.set("longshot.tutorialSeen", true)
            self?.widget.mode = .running
            self?.startCapturing()
        }
        widget.hint = L("longshot.hint")
        widget.needsAccessibility = !AXIsProcessTrusted()
        border.orderFrontRegardless()
        panel.orderFrontRegardless()
        panel.makeKey()
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] e in
            guard let self = self else { return e }
            if e.keyCode == 53 { self.cancel(); return nil }
            if (e.keyCode == 36 || e.keyCode == 76), self.widget.mode == .running { self.finish(); return nil }
            return e
        }
        if widget.mode == .running { startCapturing() }
    }

    private func startCapturing() {
        guard !running, !finished else { return }
        running = true
        queue.async { [weak self] in
            guard let self = self else { return }
            self.stitcher = ls_stitch_create(Int32(max(1000, Settings.shared.int("longshot.maxHeight"))))
        }
        // 截图层刚收起：等它真正离开屏幕再开始抓
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
            guard let self = self, !self.finished else { return }
            self.grabber.start { [weak self] _ in
                guard let self = self, !self.finished else { return }
                let t = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in self?.tick() }
                RunLoop.main.add(t, forMode: .common)
                self.timer = t
                self.tick()
                if Settings.shared.bool("longshot.autoScroll") && AXIsProcessTrusted() {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [weak self] in
                        guard let self = self, !self.finished, !self.autoScrolling else { return }
                        self.setAuto(true)
                    }
                }
            }
        }
    }

    // MARK: 抓帧

    private func tick() {
        guard running, !inFlight, !stopped, !finished else { return }
        if widget.needsAccessibility && AXIsProcessTrusted() { widget.needsAccessibility = false }
        inFlight = true
        let pw = grabber.pixelWidth, ph = grabber.pixelHeight, sc = scale
        let previewW = Int((widget.previewRect.width * sc).rounded())
        grabber.grab { [weak self] img in
            guard let self = self else { return }
            self.queue.async {
                guard let st = self.stitcher, let img = img,
                      let frame = FrozenImage.make(width: pw, height: ph, scale: sc, space: img.colorSpace, draw: { ctx in
                          ctx.interpolationQuality = img.width == pw && img.height == ph ? .none : .high
                          Draw.image(ctx, img, in: CGRect(x: 0, y: 0, width: pw, height: ph))
                      }) else {
                    DispatchQueue.main.async { self.inFlight = false }
                    return
                }
                var input = frame.image
                var r = ls_stitch_result()
                _ = ls_stitch_push(st, &input, &r)
                var preview: CGImage?
                let now = Date().timeIntervalSince1970
                let grew = r.status == Int32(LS_STITCH_FIRST.rawValue) || r.status == Int32(LS_STITCH_APPENDED.rawValue)
                if grew && (now - self.lastPreviewAt > 0.25 || r.status == Int32(LS_STITCH_FIRST.rawValue)) {
                    self.lastPreviewAt = now
                    var out = ls_image()
                    if ls_stitch_preview(st, Int32(max(1, previewW)), 1_000_000, &out) == 0 {
                        preview = FrozenImage.adopt(out, scale: sc, space: frame.colorSpace)?.cgImage
                    }
                }
                DispatchQueue.main.async {
                    self.inFlight = false
                    self.handle(r, preview: preview)
                }
            }
        }
    }

    private func handle(_ r: ls_stitch_result, preview: CGImage?) {
        guard !finished else { return }
        if let p = preview { widget.preview = p }
        widget.heightPixels = Int(r.total_height)
        switch r.status {
        case Int32(LS_STITCH_FIRST.rawValue), Int32(LS_STITCH_APPENDED.rawValue):
            unchangedCount = 0
            scrolledSinceMatch = 0
            setHint(L("longshot.hint"), warning: false)
        case Int32(LS_STITCH_UNCHANGED.rawValue):
            scrolledSinceMatch = 0
            if autoScrolling {
                unchangedCount += 1
                if unchangedCount >= 5 {
                    setHint(L("longshot.reachedEnd"), warning: false)
                    finish()
                    return
                }
            }
        case Int32(LS_STITCH_NO_MATCH.rawValue):
            setHint(L("longshot.noMatch"), warning: true)
            if autoScrolling && scrolledSinceMatch > 0 {
                // 滚过头了：退回一半，之后步长减半
                postScroll(scrolledSinceMatch / 2)
                scrolledSinceMatch /= 2
                step = max(10, step / 2)
                return
            }
        case Int32(LS_STITCH_MAX.rawValue):
            stopped = true
            setAuto(false)
            setHint(L("longshot.reachedMax"), warning: true)
            return
        default:
            break
        }
        if autoScrolling {
            postScroll(-step)
            scrolledSinceMatch += step
        }
    }

    private func setHint(_ s: String, warning: Bool) {
        widget.hintIsWarning = warning
        widget.hint = s
    }

    // MARK: 自动滚动

    private func toggleAuto() {
        if !AXIsProcessTrusted() {
            widget.needsAccessibility = true
            setHint(L("longshot.needAccessibility"), warning: true)
            Permissions.request(.accessibility) {}
            return
        }
        setAuto(!autoScrolling)
    }

    private func setAuto(_ on: Bool) {
        guard on != autoScrolling, !(on && stopped) else { return }
        autoScrolling = on
        widget.autoScrolling = on
        unchangedCount = 0
        scrolledSinceMatch = 0
        if on {
            // 滚轮事件发给光标下的窗口：光标不在区域里时先移到区域中心
            let m = NSEvent.mouseLocation
            if !region.contains(m) {
                let primaryH = NSScreen.screens.first?.frame.height ?? screen.frame.height
                CGWarpMouseCursorPosition(CGPoint(x: region.midX, y: primaryH - region.midY))
            }
        }
    }

    /// 像素单位滚轮事件（负数向下），发在区域中心。
    private func postScroll(_ dy: CGFloat) {
        guard let e = CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 1, wheel1: Int32(dy.rounded()), wheel2: 0, wheel3: 0) else { return }
        let primaryH = NSScreen.screens.first?.frame.height ?? screen.frame.height
        e.location = CGPoint(x: region.midX, y: primaryH - region.midY)
        e.post(tap: .cghidEventTap)
    }

    // MARK: 结束

    private func teardownUI() {
        timer?.invalidate()
        timer = nil
        running = false
        autoScrolling = false
        grabber.stop()
        if let m = keyMonitor { NSEvent.removeMonitor(m) }
        keyMonitor = nil
        panel.orderOut(nil)
        border.orderOut(nil)
        DispatchQueue.main.async { [panel, border] in
            panel.contentView = nil
            panel.close()
            border.close()
        }
        if LongShotSession.current === self { LongShotSession.current = nil }
    }

    func cancel() {
        guard !finished else { return }
        finished = true
        teardownUI()
        queue.async { [self] in
            if let st = stitcher { ls_stitch_destroy(st) }
            stitcher = nil
        }
    }

    func finish() {
        guard !finished else { return }
        finished = true
        let sc = scale
        let space = FrozenImage.rgbSpace(widget.preview?.colorSpace)
        teardownUI()
        queue.async { [self] in
            var result: CGImage?
            if let st = stitcher {
                var out = ls_image()
                if ls_stitch_height(st) > 0, ls_stitch_render(st, &out) == 0 {
                    result = FrozenImage.adopt(out, scale: sc, space: space)?.cgImage
                }
                ls_stitch_destroy(st)
            }
            stitcher = nil
            DispatchQueue.main.async {
                guard let img = result else { return }
                Export.playSound()
                LongShotResultController.show(image: img, scale: sc, screen: self.screen)
            }
        }
    }

    // MARK: 自测

    /// 自测：假屏幕上画区域边框与面板（假预览）。
    static func render(scale: CGFloat, screen: FrozenImage, region: CGRect, preview: CGImage?, heightPixels: Int) -> CGImage? {
        let size = screen.logicalSize
        let w = LongShotPanelWidget(mode: .running)
        let h = min(max(region.height, 380), size.height - 16)
        var y = region.minY
        y = max(8, min(y, size.height - 8 - h))
        w.frame = CGRect(x: region.maxX + 10 + Theme.f("recordBar.regionBorderWidth"), y: y, width: LongShotPanelWidget.width, height: h)
        w.layout()
        w.preview = preview
        w.heightPixels = heightPixels
        w.hint = L("longshot.hint")
        w.autoScrolling = true
        return Offscreen.render(size: size, scale: scale) { ctx in
            ctx.interpolationQuality = .none
            Draw.image(ctx, screen.cgImage, in: CGRect(origin: .zero, size: size))
            ctx.interpolationQuality = .high
            let bw = Theme.f("recordBar.regionBorderWidth")
            Draw.stroke(ctx, region.insetBy(dx: -bw, dy: -bw), Theme.tokenColor("recordBar.regionBorder"), width: bw)
            w.draw(ctx)
        }
    }
}
