import AppKit

/// 长截图右侧面板（spec 6）：缩略预览（自动滚到底）、高度、提示、自动滚动/完成/取消；
/// 首次使用时先显示三条注意事项和「我知道了」。
final class LongShotPanelWidget: Widget {
    enum Mode { case notes, running }
    var mode: Mode { didSet { relayout() } }
    var preview: CGImage? { didSet { setNeedsDisplay() } }
    /// 拼接结果高度（像素）。
    var heightPixels = 0 { didSet { setNeedsDisplay() } }
    var hint = "" { didSet { setNeedsDisplay() } }
    var hintIsWarning = false
    var autoScrolling = false { didSet { updateAutoButton() } }
    /// 没有辅助功能权限：自动滚动按钮变成「去授权」。
    var needsAccessibility = false { didSet { updateAutoButton() } }

    let autoButton = ButtonW(L("longshot.auto"), icon: "play")
    let finishButton = ButtonW(L("longshot.finish"), kind: .primary, icon: "check")
    let cancelButton = ButtonW(L("longshot.cancel"), icon: "x")
    let gotItButton = ButtonW(L("longshot.gotIt"), kind: .primary)
    let notesCancelButton = ButtonW(L("longshot.cancel"))

    static let width: CGFloat = 160
    static let pad: CGFloat = 12
    static let buttonGap: CGFloat = 8

    init(mode: Mode) {
        self.mode = mode
        super.init()
        for b in [autoButton, finishButton, cancelButton, gotItButton, notesCancelButton] { add(b) }
        relayout()
    }

    private func relayout() {
        let running = mode == .running
        for b in [autoButton, finishButton, cancelButton] { b.isHidden = !running }
        for b in [gotItButton, notesCancelButton] { b.isHidden = running }
        layout()
        host?.needsDisplay = true
    }

    private func updateAutoButton() {
        if needsAccessibility {
            autoButton.title = L("perm.grant")
            autoButton.icon = "hand"
        } else {
            autoButton.title = L(autoScrolling ? "longshot.stopAuto" : "longshot.auto")
            autoButton.icon = autoScrolling ? "pause" : "play"
        }
        autoButton.setNeedsDisplay()
    }

    var inner: CGRect { frame.insetBy(dx: LongShotPanelWidget.pad, dy: LongShotPanelWidget.pad) }
    private var titleRect: CGRect { CGRect(x: inner.minX, y: inner.minY, width: inner.width, height: 22) }
    private let hintLines: CGFloat = 3
    private var hintFont: NSFont { Fonts.ui(Fonts.caption) }
    private var hintHeight: CGFloat { ceil((Draw.lineHeight(hintFont) + 2) * hintLines) }

    /// 预览框：标题下方到高度标签上方。
    var previewRect: CGRect {
        let bh = Theme.f("window.controlHeight")
        let buttonsTop = inner.maxY - 3 * bh - 2 * LongShotPanelWidget.buttonGap
        let bottom = buttonsTop - 10 - hintHeight - 4 - 20 - 8
        return CGRect(x: inner.minX, y: titleRect.maxY + 8, width: inner.width, height: max(40, bottom - titleRect.maxY - 8))
    }

    override func layout() {
        let bh = Theme.f("window.controlHeight")
        var y = inner.maxY - bh
        for b in [cancelButton, finishButton, autoButton] {
            b.frame = CGRect(x: inner.minX, y: y, width: inner.width, height: bh)
            y -= bh + LongShotPanelWidget.buttonGap
        }
        notesCancelButton.frame = CGRect(x: inner.minX, y: inner.maxY - bh, width: inner.width, height: bh)
        gotItButton.frame = CGRect(x: inner.minX, y: inner.maxY - 2 * bh - LongShotPanelWidget.buttonGap, width: inner.width, height: bh)
    }

    override func draw(_ ctx: CGContext) {
        Draw.panel(ctx, frame, radius: Theme.f("window.radius"), fill: Theme.color("popoverBg"), border: Theme.color("border"),
                   shadow: Theme.color("shadow"), blur: Theme.f("toolbar.shadowBlur"), offsetY: Theme.f("toolbar.shadowOffsetY"))
        switch mode {
        case .notes: drawNotes(ctx)
        case .running: drawRunning(ctx)
        }
        drawChildren(ctx)
    }

    private func drawRunning(_ ctx: CGContext) {
        Draw.text(ctx, L("longshot.title"), font: Fonts.semibold(Fonts.body), color: Theme.color("text"), in: titleRect)
        let pr = previewRect
        Draw.fill(ctx, pr, Theme.color("inputBg"), radius: 6)
        if let img = preview, img.width > 0 {
            // 等宽缩放，太高时显示底部（自动滚到底）
            let s = pr.width / CGFloat(img.width)
            let h = CGFloat(img.height) * s
            let r = CGRect(x: pr.minX, y: h > pr.height ? pr.maxY - h : pr.minY, width: pr.width, height: h)
            ctx.saveGState()
            ctx.addPath(Draw.rounded(pr, 6))
            ctx.clip()
            ctx.interpolationQuality = .high
            Draw.image(ctx, img, in: r)
            ctx.restoreGState()
        } else {
            Icons.draw(ctx, "scroll-text", in: CGRect(x: pr.midX - 14, y: pr.midY - 14, width: 28, height: 28), color: Theme.color("textTertiary"), lineWidth: 1.5)
        }
        Draw.stroke(ctx, pr, Theme.color("border"), width: 1, radius: 6)
        let hr = CGRect(x: inner.minX, y: pr.maxY + 8, width: inner.width, height: 20)
        Draw.text(ctx, L("longshot.height", ["h": String(heightPixels)]), font: Fonts.digits(Fonts.body, .medium), color: Theme.color("text"), in: hr, align: .center)
        if !hint.isEmpty {
            ctx.saveGState()
            let tr = CGRect(x: inner.minX, y: hr.maxY + 4, width: inner.width, height: hintHeight)
            ctx.clip(to: tr)
            Draw.wrapped(ctx, hint, font: hintFont, color: hintIsWarning ? Theme.color("warning") : Theme.color("textSecondary"),
                         x: tr.minX, y: tr.minY, width: tr.width, lineSpacing: 2, align: .center)
            ctx.restoreGState()
        }
    }

    private func drawNotes(_ ctx: CGContext) {
        Draw.text(ctx, L("longshot.notes"), font: Fonts.semibold(Fonts.body), color: Theme.color("text"), in: titleRect)
        var y = titleRect.maxY + 10
        let font = Fonts.ui(Fonts.caption + 1)
        for i in 1...3 {
            let badge = CGRect(x: inner.minX, y: y + 1, width: 16, height: 16)
            Draw.ellipse(ctx, badge, fill: Theme.color("surfaceSelected"))
            Draw.text(ctx, String(i), font: Fonts.digits(Fonts.caption, .semibold), color: Theme.color("iconActive"), in: badge, align: .center, truncate: false)
            let h = Draw.wrapped(ctx, L("longshot.note\(i)"), font: font, color: Theme.color("text"), x: inner.minX + 24, y: y, width: inner.width - 24, lineSpacing: 2)
            y += h + 12
        }
    }
}

/// 区域边框（画在区域外侧，不进画面）。
final class LongShotBorderWindow: NSPanel {
    init(region: CGRect) {
        let bw = Theme.f("recordBar.regionBorderWidth")
        let frame = region.insetBy(dx: -bw, dy: -bw)
        super.init(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        ignoresMouseEvents = true
        level = .floating
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        sharingType = .none
        isReleasedWhenClosed = false
        contentView = PaintView(frame: CGRect(origin: .zero, size: frame.size)) { ctx, _ in
            let r = CGRect(origin: .zero, size: frame.size)
            Draw.stroke(ctx, r, Theme.tokenColor("recordBar.regionBorder"), width: bw)
        }
    }
}

/// 面板窗口：不抢应用激活，但能接收 Esc/回车。
final class LongShotPanelWindow: NSPanel {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
}
