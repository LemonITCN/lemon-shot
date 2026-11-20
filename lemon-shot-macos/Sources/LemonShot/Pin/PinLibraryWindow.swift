import AppKit

/// 贴图库标题栏：居中标题 + 张数，右侧「清空」。
final class LibraryHeader: Widget {
    let clear: ButtonW
    init(onClear: @escaping () -> Void) {
        clear = ButtonW(L("pin.libraryClear"), kind: .normal, icon: "trash-2", action: onClear)
        super.init()
        add(clear)
    }

    override func layout() {
        let s = clear.preferredSize()
        clear.frame = CGRect(x: frame.maxX - 16 - s.width, y: frame.midY - s.height / 2, width: s.width, height: s.height)
        clear.isEnabled = !PinLibrary.shared.entries.isEmpty
    }

    override func draw(_ ctx: CGContext) {
        Draw.fill(ctx, CGRect(x: frame.minX, y: frame.maxY - 1, width: frame.width, height: 1), Theme.color("divider"))
        let title = L("pin.library")
        let tf = Fonts.semibold(Fonts.title - 2)
        let count = L("pin.libraryCount", ["n": String(PinLibrary.shared.entries.count)])
        let cf = Fonts.ui(Fonts.caption + 1)
        let tw = Draw.textWidth(title, tf), cw = Draw.textWidth(count, cf)
        let x = frame.midX - (tw + 8 + cw) / 2
        Draw.text(ctx, title, font: tf, color: Theme.color("text"), in: CGRect(x: x, y: frame.minY, width: tw + 1, height: frame.height), truncate: false)
        Draw.text(ctx, count, font: cf, color: Theme.color("textSecondary"), in: CGRect(x: x + tw + 8, y: frame.minY + 1, width: cw + 1, height: frame.height), truncate: false)
        drawChildren(ctx)
    }
}

/// 贴图库网格：160×120 卡片，单击重新贴图，悬停右上角删除。
final class LibraryGrid: Widget {
    static let card = CGSize(width: 160, height: 120)
    static let gap: CGFloat = 12
    static let pad: CGFloat = 20
    private var hover = -1
    private var hoverTrash = false
    var onChange: (() -> Void)?

    override var isInteractive: Bool { true }

    var columns: Int { max(1, Int((frame.width - 2 * LibraryGrid.pad + LibraryGrid.gap) / (LibraryGrid.card.width + LibraryGrid.gap))) }

    func contentHeight(width: CGFloat) -> CGFloat {
        let cols = max(1, Int((width - 2 * LibraryGrid.pad + LibraryGrid.gap) / (LibraryGrid.card.width + LibraryGrid.gap)))
        let rows = (PinLibrary.shared.entries.count + cols - 1) / cols
        return CGFloat(rows) * (LibraryGrid.card.height + LibraryGrid.gap) - LibraryGrid.gap + 2 * LibraryGrid.pad
    }

    func cardRect(_ i: Int) -> CGRect {
        let cols = columns
        let total = CGFloat(cols) * LibraryGrid.card.width + CGFloat(cols - 1) * LibraryGrid.gap
        let x0 = frame.minX + (frame.width - total) / 2
        let c = i % cols, r = i / cols
        return CGRect(x: x0 + CGFloat(c) * (LibraryGrid.card.width + LibraryGrid.gap), y: frame.minY + LibraryGrid.pad + CGFloat(r) * (LibraryGrid.card.height + LibraryGrid.gap),
                      width: LibraryGrid.card.width, height: LibraryGrid.card.height)
    }

    func trashRect(_ i: Int) -> CGRect {
        let r = cardRect(i)
        return CGRect(x: r.maxX - 30, y: r.minY + 6, width: 24, height: 24)
    }

    override func draw(_ ctx: CGContext) {
        let entries = PinLibrary.shared.entries
        if entries.isEmpty {
            let c = CGPoint(x: frame.midX, y: frame.minY + 150)
            Icons.draw(ctx, "images", in: CGRect(x: c.x - 20, y: c.y - 44, width: 40, height: 40), color: Theme.color("textTertiary"), lineWidth: 1.5)
            Draw.text(ctx, L("pin.libraryEmpty"), font: Fonts.ui(Fonts.body), color: Theme.color("textSecondary"), in: CGRect(x: frame.minX, y: c.y + 4, width: frame.width, height: 20), align: .center)
            return
        }
        for (i, e) in entries.enumerated() {
            let r = cardRect(i)
            Draw.fill(ctx, r, Theme.color("surface"), radius: 8)
            Draw.stroke(ctx, r, i == hover ? Theme.color("accent") : Theme.color("border"), width: 1, radius: 8)
            if let t = PinLibrary.shared.thumbnail(e) {
                let inner = r.insetBy(dx: 8, dy: 8)
                let s = min(inner.width / CGFloat(t.width), inner.height / CGFloat(t.height))
                let w = CGFloat(t.width) * s, h = CGFloat(t.height) * s
                let ir = CGRect(x: inner.midX - w / 2, y: inner.midY - h / 2, width: w, height: h)
                ctx.saveGState()
                ctx.addPath(Draw.rounded(ir, 4))
                ctx.clip()
                ctx.interpolationQuality = .high
                Draw.image(ctx, t, in: ir)
                ctx.restoreGState()
            }
            if i == hover {
                let tr = trashRect(i)
                Draw.fill(ctx, tr, hoverTrash ? Theme.color("danger") : Theme.cap("tipBg"), radius: 6)
                Icons.draw(ctx, "trash-2", in: tr.insetBy(dx: 5, dy: 5), color: CGColor(gray: 1, alpha: 1), lineWidth: 1.75)
            }
        }
    }

    /// 自测：指定悬停的卡片。
    func setHover(_ i: Int) { hover = i }

    override func mouseMoved(_ p: CGPoint) {
        let n = PinLibrary.shared.entries.count
        let h = (0..<n).first { cardRect($0).contains(p) } ?? -1
        let t = h >= 0 && trashRect(h).contains(p)
        if h != hover || t != hoverTrash {
            hover = h
            hoverTrash = t
            host?.needsDisplay = true
        }
        hoverCursor = h >= 0 ? .pointingHand : nil
    }

    override func mouseExited() {
        hover = -1
        host?.needsDisplay = true
    }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        let entries = PinLibrary.shared.entries
        guard let i = entries.indices.first(where: { cardRect($0).contains(p) }) else { return }
        if trashRect(i).contains(p) {
            PinLibrary.shared.remove(entries[i].file)
            hover = -1
            onChange?()
        } else {
            PinLibrary.shared.repin(entries[i])
        }
    }
}

final class PinLibraryWindowController: NSObject, NSWindowDelegate {
    static var current: PinLibraryWindowController?
    let window: NSWindow
    private let header: LibraryHeader
    private let grid: LibraryGrid
    private let headerHost: WidgetHost
    private let gridHost: WidgetHost
    private let scroll: NSScrollView
    static let size = CGSize(width: 720, height: 480)

    static func show() {
        if let c = current {
            c.reload()
            NSApp.activate(ignoringOtherApps: true)
            c.window.makeKeyAndOrderFront(nil)
            return
        }
        let c = PinLibraryWindowController()
        current = c
        NSApp.activate(ignoringOtherApps: true)
        c.window.center()
        c.window.makeKeyAndOrderFront(nil)
    }

    override init() {
        let size = PinLibraryWindowController.size
        window = NSWindow(contentRect: CGRect(origin: .zero, size: size), styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.title = L("pin.library")
        window.isReleasedWhenClosed = false
        window.appearance = Theme.appearance
        window.backgroundColor = NSColor(cgColor: Theme.color("windowBg"))
        let th = Theme.f("window.titleBarHeight")
        var clearAction: (() -> Void)?
        header = LibraryHeader { clearAction?() }
        header.frame = CGRect(x: 0, y: 0, width: size.width, height: th)
        headerHost = WidgetHost(frame: header.frame, root: header)
        grid = LibraryGrid()
        grid.frame = CGRect(x: 0, y: 0, width: size.width, height: max(size.height - th, grid.contentHeight(width: size.width)))
        gridHost = WidgetHost(frame: grid.frame, root: grid)
        scroll = NSScrollView(frame: CGRect(x: 0, y: th, width: size.width, height: size.height - th))
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
        scroll.documentView = gridHost
        let content = FlippedView(frame: CGRect(origin: .zero, size: size))
        content.addSubview(scroll)
        content.addSubview(headerHost)
        super.init()
        window.contentView = content
        window.delegate = self
        header.layout()
        clearAction = { [weak self] in self?.confirmClear() }
        grid.onChange = { [weak self] in self?.reload() }
        NotificationCenter.default.addObserver(self, selector: #selector(libraryChanged), name: PinLibrary.didChange, object: nil)
    }

    @objc private func libraryChanged() { reload() }

    func reload() {
        let w = PinLibraryWindowController.size.width
        grid.frame = CGRect(x: 0, y: 0, width: w, height: max(scroll.frame.height, grid.contentHeight(width: w)))
        gridHost.frame = grid.frame
        header.layout()
        headerHost.needsDisplay = true
        gridHost.needsDisplay = true
    }

    private func confirmClear() {
        let a = NSAlert()
        a.messageText = L("pin.libraryClearConfirm")
        a.addButton(withTitle: L("pin.libraryClear"))
        a.addButton(withTitle: L("common.cancel"))
        a.buttons.first?.hasDestructiveAction = true
        a.beginSheetModal(for: window) { [weak self] resp in
            if resp == .alertFirstButtonReturn {
                PinLibrary.shared.clear()
                self?.reload()
            }
        }
    }

    func windowWillClose(_ notification: Notification) {
        NotificationCenter.default.removeObserver(self)
        PinLibraryWindowController.current = nil
    }

    /// 自测：整窗画进一张图。
    static func render(size: CGSize, scale: CGFloat, hover: Int) -> CGImage? {
        let th = Theme.f("window.titleBarHeight")
        let header = LibraryHeader {}
        header.frame = CGRect(x: 0, y: 0, width: size.width, height: th)
        header.layout()
        let grid = LibraryGrid()
        grid.frame = CGRect(x: 0, y: th, width: size.width, height: size.height - th)
        grid.setHover(hover)
        return Offscreen.render(size: size, scale: scale) { ctx in
            Draw.fill(ctx, CGRect(origin: .zero, size: size), Theme.color("windowBg"))
            header.draw(ctx)
            ctx.saveGState()
            ctx.clip(to: grid.frame)
            grid.draw(ctx)
            ctx.restoreGState()
        }
    }
}

final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}
