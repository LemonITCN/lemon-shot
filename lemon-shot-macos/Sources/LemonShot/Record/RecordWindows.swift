import AppKit

// 录屏时的窗口（spec 8.2）。控制条、边框、倒计时、进度不进画面（ScreenCaptureKit 按应用排除）；
// 画笔层、点击圈、按键浮层、摄像头画中画进画面（见 RecordOverlays.swift）。

/// 录屏用的无边框浮动窗口：不抢焦点、所有空间可见、全屏应用之上。
class RecordPanel: NSPanel {
    var keyable = false

    init(frame: CGRect, level: NSWindow.Level) {
        super.init(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        self.level = level
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        isReleasedWhenClosed = false
        hidesOnDeactivate = false
        animationBehavior = .none
        isMovable = false
    }

    override var canBecomeKey: Bool { keyable }
    override var canBecomeMain: Bool { false }

    /// 控制条、边框等自己的界面窗口：不进系统截图（老系统 CGDisplayStream 也尽量排除）。
    func markPrivate() { sharingType = .none }

    static let chromeLevel = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 1)
    static let overlayLevel = NSWindow.Level.screenSaver
}

/// 屏幕坐标换算：Cocoa 全局（左下原点）。
enum RecordGeometry {
    /// 显示器内的逻辑点矩形（左上原点），给 SCStream.sourceRect / CGDisplayStream 用。
    static func displayLocal(_ r: CGRect, screen: NSScreen) -> CGRect {
        CGRect(x: r.minX - screen.frame.minX, y: screen.frame.maxY - r.maxY, width: r.width, height: r.height)
    }

    static func isFullscreen(_ r: CGRect, screen: NSScreen) -> Bool {
        abs(r.minX - screen.frame.minX) < 1 && abs(r.minY - screen.frame.minY) < 1 &&
            abs(r.width - screen.frame.width) < 1 && abs(r.height - screen.frame.height) < 1
    }

    /// 控制条位置（spec 8.2）：区域下方居中，放不下放上方；全屏放屏幕底部居中。
    /// outside 为真时只接受区域外的位置（老系统无法排除窗口），都放不下返回 nil。
    static func barFrame(size: CGSize, region: CGRect, screen: NSScreen, fullscreen: Bool, outside: Bool) -> CGRect? {
        let vis = screen.visibleFrame
        let sf = screen.frame
        let gap = Theme.f("capture.toolbarGap") + Theme.f("recordBar.regionBorderWidth")
        let margin: CGFloat = 8
        func clampX(_ x: CGFloat) -> CGFloat { max(sf.minX + margin, min(x, sf.maxX - size.width - margin)) }
        if fullscreen {
            if outside { return nil }
            return CGRect(x: clampX(sf.midX - size.width / 2), y: vis.minY + 24, width: size.width, height: size.height)
        }
        let x = clampX(region.midX - size.width / 2)
        let below = region.minY - gap - size.height
        if below >= vis.minY + margin { return CGRect(x: x, y: below, width: size.width, height: size.height) }
        let above = region.maxY + gap
        if above + size.height <= vis.maxY - margin { return CGRect(x: x, y: above, width: size.width, height: size.height) }
        if outside { return nil }
        return CGRect(x: x, y: region.minY + gap, width: size.width, height: size.height)
    }
}

// MARK: - 控制条窗口

/// 承载控制条，四周留投影边距；鼠标进出用于全屏时的自动半透明。
final class RecordBarHost: WidgetHost {
    var onHover: ((Bool) -> Void)?
    override func mouseEntered(with event: NSEvent) {
        super.mouseEntered(with: event)
        onHover?(true)
    }
    override func mouseExited(with event: NSEvent) {
        super.mouseExited(with: event)
        onHover?(false)
    }
}

final class RecordBarWindow {
    static let margin: CGFloat = 18
    let widget = RecordBarWidget()
    let panel: RecordPanel
    private let host: RecordBarHost
    /// 全屏录制：3 秒后自动半透明，鼠标移上去恢复。
    var autoFade = false { didSet { scheduleFade() } }
    private var fadeWork: DispatchWorkItem?
    private var hovering = false

    init() {
        let m = RecordBarWindow.margin
        let size = widget.frame.size
        widget.frame = CGRect(x: m, y: m, width: size.width, height: size.height)
        widget.layout()
        let full = CGRect(x: 0, y: 0, width: size.width + 2 * m, height: size.height + 2 * m)
        host = RecordBarHost(frame: full, root: widget)
        host.tooltipDelay = 0.5
        panel = RecordPanel(frame: full, level: RecordPanel.chromeLevel)
        panel.markPrivate()
        panel.appearance = Theme.appearance
        panel.contentView = host
        host.onHover = { [weak self] inside in
            guard let self = self else { return }
            self.hovering = inside
            if inside {
                self.fadeWork?.cancel()
                self.panel.animator().alphaValue = 1
            } else {
                self.scheduleFade()
            }
        }
    }

    var barSize: CGSize { widget.frame.size }

    /// bar 为控制条本体的屏幕位置。
    func place(_ bar: CGRect) {
        let m = RecordBarWindow.margin
        panel.setFrame(bar.insetBy(dx: -m, dy: -m), display: true)
    }

    var barFrame: CGRect { panel.frame.insetBy(dx: RecordBarWindow.margin, dy: RecordBarWindow.margin) }

    /// 计时位数变化（满一小时）时重新算宽度，保持居中。
    func relayoutIfNeeded() {
        let size = widget.contentSize()
        guard abs(size.width - widget.frame.width) > 0.5 else { return }
        let old = barFrame
        let m = RecordBarWindow.margin
        widget.frame = CGRect(x: m, y: m, width: size.width, height: size.height)
        widget.layout()
        host.frame = CGRect(x: 0, y: 0, width: size.width + 2 * m, height: size.height + 2 * m)
        place(CGRect(x: old.midX - size.width / 2, y: old.minY, width: size.width, height: size.height))
        host.needsDisplay = true
    }

    func refresh() { host.needsDisplay = true }

    func show() {
        panel.alphaValue = 1
        panel.orderFrontRegardless()
        scheduleFade()
    }

    func hide() { panel.orderOut(nil) }

    private func scheduleFade() {
        fadeWork?.cancel()
        guard autoFade, !hovering else {
            if !autoFade { panel.alphaValue = 1 }
            return
        }
        let w = DispatchWorkItem { [weak self] in
            guard let self = self, self.autoFade, !self.hovering else { return }
            NSAnimationContext.runAnimationGroup { c in
                c.duration = Theme.f("animation.normalMs") / 1000
                self.panel.animator().alphaValue = 0.35
            }
        }
        fadeWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 3, execute: w)
    }

    func close() {
        fadeWork?.cancel()
        host.hideTooltip()
        panel.orderOut(nil)
        panel.close()
    }
}

// MARK: - 区域边框

/// 区域外侧的红色虚线边框（recordBar.regionBorder，regionBorderWidth），点击穿透。
final class RegionBorderWindow {
    let panel: RecordPanel
    static let pad: CGFloat = 4

    init(region: CGRect) {
        let pad = RegionBorderWindow.pad
        let frame = region.insetBy(dx: -pad, dy: -pad)
        panel = RecordPanel(frame: frame, level: RecordPanel.chromeLevel)
        panel.ignoresMouseEvents = true
        panel.markPrivate()
        let size = frame.size
        panel.contentView = PaintView(frame: CGRect(origin: .zero, size: size)) { ctx, _ in
            let bw = Theme.f("recordBar.regionBorderWidth")
            let inner = CGRect(x: pad, y: pad, width: size.width - 2 * pad, height: size.height - 2 * pad)
            ctx.setStrokeColor(Theme.tokenColor("recordBar.regionBorder"))
            ctx.setLineWidth(bw)
            ctx.setLineDash(phase: 0, lengths: [6, 4])
            // 画在区域外侧，不压住被录的像素
            ctx.stroke(inner.insetBy(dx: -bw / 2, dy: -bw / 2))
        }
    }

    func show() { panel.orderFrontRegardless() }

    func close() {
        panel.orderOut(nil)
        panel.close()
    }
}

// MARK: - 倒计时

/// 区域中央的大号倒计时（countdownSize 圆形 tipBg 底），可按 Esc 取消。
final class RecordCountdown {
    private let panel: RecordPanel
    private let widget: CountdownWidget
    private let host: WidgetHost
    private var timer: Timer?
    private var monitor: Any?
    private var remaining: Int
    private var finished = false

    init(seconds: Int, region: CGRect) {
        remaining = seconds
        let size = Theme.f("recordBar.countdownSize")
        widget = CountdownWidget(number: seconds)
        widget.frame = CGRect(x: 0, y: 0, width: size, height: size)
        host = WidgetHost(frame: widget.frame, root: widget)
        panel = RecordPanel(frame: CGRect(x: region.midX - size / 2, y: region.midY - size / 2, width: size, height: size), level: RecordPanel.chromeLevel)
        panel.keyable = true
        panel.ignoresMouseEvents = true
        panel.markPrivate()
        panel.contentView = host
    }

    /// 走完调用 done，按 Esc 调用 cancelled。
    func start(done: @escaping () -> Void, cancelled: @escaping () -> Void) {
        panel.orderFrontRegardless()
        panel.makeKey()
        monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] e in
            guard let self = self, !self.finished else { return e }
            if e.keyCode == 53 {
                self.close()
                cancelled()
                return nil
            }
            return e
        }
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] t in
            guard let self = self else { t.invalidate(); return }
            self.remaining -= 1
            if self.remaining <= 0 {
                self.close()
                done()
                return
            }
            self.widget.number = self.remaining
            self.host.needsDisplay = true
        }
    }

    func close() {
        guard !finished else { return }
        finished = true
        timer?.invalidate()
        timer = nil
        if let m = monitor { NSEvent.removeMonitor(m) }
        monitor = nil
        panel.orderOut(nil)
        panel.close()
    }
}

// MARK: - 进度

/// 生成 GIF 的进度：屏幕底部居中（与 toast 同一位置），不抢焦点。
final class RecordProgressWindow {
    private let panel: RecordPanel
    let widget: RecordProgressWidget
    private let host: WidgetHost

    init(text: String, screen: NSScreen?) {
        let size = RecordProgressWidget.size
        widget = RecordProgressWidget(text: text)
        widget.frame = CGRect(origin: .zero, size: size)
        host = WidgetHost(frame: widget.frame, root: widget)
        let sf = screen?.frame ?? NSScreen.main?.frame ?? CGRect(x: 0, y: 0, width: 1440, height: 900)
        panel = RecordPanel(frame: CGRect(x: (sf.midX - size.width / 2).rounded(), y: sf.minY + 80, width: size.width, height: size.height), level: RecordPanel.chromeLevel)
        panel.ignoresMouseEvents = true
        panel.markPrivate()
        panel.contentView = host
        panel.orderFrontRegardless()
    }

    var progress: Double {
        get { widget.progress }
        set {
            widget.progress = newValue
            host.needsDisplay = true
        }
    }

    func close() {
        panel.orderOut(nil)
        panel.close()
    }
}
