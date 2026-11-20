import AppKit

/// 提示（spec 13）：屏幕底部居中上方 80pt 的胶囊，淡入 120ms、停留 1.5 秒、淡出 200ms。不抢焦点。
final class ToastWidget: Widget {
    let text: String
    let icon: String
    var clickable = false

    init(text: String, icon: String) {
        self.text = text
        self.icon = icon
    }

    override var isInteractive: Bool { true }

    static let height: CGFloat = 36
    static let padX: CGFloat = 16
    static let iconSize: CGFloat = 16
    static let iconGap: CGFloat = 8
    var font: NSFont { Fonts.ui(13) }

    func contentSize() -> CGSize {
        CGSize(width: ceil(ToastWidget.padX * 2 + ToastWidget.iconSize + ToastWidget.iconGap + Draw.textWidth(text, font)), height: ToastWidget.height)
    }

    override func draw(_ ctx: CGContext) {
        let r = frame
        Draw.fill(ctx, r, Theme.cap("tipBg"), radius: r.height / 2)
        let white = Theme.cap("tipText")
        Icons.draw(ctx, icon, in: CGRect(x: r.minX + ToastWidget.padX, y: r.midY - ToastWidget.iconSize / 2, width: ToastWidget.iconSize, height: ToastWidget.iconSize), color: white, lineWidth: 1.75)
        let tx = r.minX + ToastWidget.padX + ToastWidget.iconSize + ToastWidget.iconGap
        Draw.text(ctx, text, font: font, color: white, in: CGRect(x: tx, y: r.minY, width: r.maxX - ToastWidget.padX - tx + 1, height: r.height), truncate: false)
    }

    var onClick: (() -> Void)?
    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) { if inside { onClick?() } }
}

final class Toast {
    private static var current: NSPanel?
    private static var hideWork: DispatchWorkItem?

    /// icon：check（完成）、triangle-alert（出错）等 icons.json 里的名字。
    static func show(_ text: String, icon: String = "circle-check", onClick: (() -> Void)? = nil) {
        dismiss(animated: false)
        let w = ToastWidget(text: text, icon: icon)
        let size = w.contentSize()
        w.frame = CGRect(origin: .zero, size: size)
        w.clickable = onClick != nil
        if onClick != nil { w.hoverCursor = .pointingHand }
        let host = WidgetHost(frame: CGRect(origin: .zero, size: size), root: w)
        let panel = NSPanel(contentRect: CGRect(origin: .zero, size: size), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 1)
        panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        panel.ignoresMouseEvents = onClick == nil
        panel.sharingType = .none
        panel.isReleasedWhenClosed = false
        panel.contentView = host
        w.onClick = {
            onClick?()
            dismiss(animated: true)
        }
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { $0.frame.contains(mouse) } ?? NSScreen.main
        let sf = screen?.frame ?? CGRect(x: 0, y: 0, width: 1440, height: 900)
        panel.setFrameOrigin(CGPoint(x: (sf.midX - size.width / 2).rounded(), y: sf.minY + 80))
        panel.alphaValue = 0
        panel.orderFrontRegardless()
        current = panel
        NSAnimationContext.runAnimationGroup { c in
            c.duration = Theme.f("animation.fastMs") / 1000
            panel.animator().alphaValue = 1
        }
        let work = DispatchWorkItem { dismiss(animated: true) }
        hideWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12 + 1.5, execute: work)
    }

    static func dismiss(animated: Bool) {
        hideWork?.cancel()
        hideWork = nil
        guard let p = current else { return }
        current = nil
        if animated {
            NSAnimationContext.runAnimationGroup({ c in
                c.duration = Theme.f("animation.normalMs") / 1000
                p.animator().alphaValue = 0
            }, completionHandler: { p.close() })
        } else {
            p.close()
        }
    }
}
