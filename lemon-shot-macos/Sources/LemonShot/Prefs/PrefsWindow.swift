import AppKit

/// 偏好设置侧边栏：图标 + 文字，选中 surfaceSelected 圆角底。
final class PrefsSidebar: Widget {
    var selected: String
    var onSelect: ((String) -> Void)?
    private var hover = -1

    init(selected: String) { self.selected = selected }
    override var isInteractive: Bool { true }

    var itemH: CGFloat { Theme.f("window.sidebarItemHeight") }
    var top: CGFloat { frame.minY + Theme.f("window.titleBarHeight") + 4 }

    func itemRect(_ i: Int) -> CGRect {
        CGRect(x: frame.minX + 10, y: top + CGFloat(i) * (itemH + 2), width: frame.width - 20, height: itemH)
    }

    override func draw(_ ctx: CGContext) {
        Draw.fill(ctx, frame, Theme.color("windowBg"))
        Draw.fill(ctx, CGRect(x: frame.maxX - 1, y: frame.minY, width: 1, height: frame.height), Theme.color("border"))
        let iconS = Theme.f("window.sidebarIcon")
        for (i, p) in PrefsPages.all.enumerated() {
            let r = itemRect(i)
            let sel = p.id == selected
            if sel {
                Draw.fill(ctx, r, Theme.color("surfaceSelected"), radius: Theme.f("window.sidebarItemRadius"))
            } else if i == hover {
                Draw.fill(ctx, r, Theme.color("surfaceHover"), radius: Theme.f("window.sidebarItemRadius"))
            }
            Icons.draw(ctx, p.icon, in: CGRect(x: r.minX + 10, y: r.midY - iconS / 2, width: iconS, height: iconS),
                       color: sel ? Theme.color("iconActive") : Theme.color("icon"), lineWidth: 1.6)
            Draw.text(ctx, L(p.titleKey), font: sel ? Fonts.medium(Fonts.body) : Fonts.ui(Fonts.body), color: Theme.color("text"),
                      in: CGRect(x: r.minX + 10 + iconS + 10, y: r.minY, width: r.width - iconS - 30, height: r.height))
        }
    }

    override func mouseMoved(_ p: CGPoint) {
        let h = PrefsPages.all.indices.first { itemRect($0).contains(p) } ?? -1
        if h != hover { hover = h; host?.needsDisplay = true }
    }

    override func mouseExited() { hover = -1; host?.needsDisplay = true }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard let i = PrefsPages.all.indices.first(where: { itemRect($0).contains(p) }) else { return }
        onSelect?(PrefsPages.all[i].id)
    }
}

/// 偏好设置窗口：固定 760×520，左侧边栏，右侧可滚动内容。
final class PrefsWindowController: NSObject, NSWindowDelegate {
    static let size = CGSize(width: 760, height: 520)
    private(set) static var current: PrefsWindowController?

    let window: NSWindow
    private var page = "general"
    private let sidebar: PrefsSidebar
    private let sidebarHost: WidgetHost
    private let scroll: NSScrollView
    private var contentHost: WidgetHost?
    private var form: FormPage?

    static func show(page: String?) {
        let c = current ?? PrefsWindowController()
        current = c
        if let p = page { c.select(p) }
        NSApp.activate(ignoringOtherApps: true)
        if !c.window.isVisible { c.window.center() }
        c.window.makeKeyAndOrderFront(nil)
    }

    override init() {
        let size = PrefsWindowController.size
        window = NSWindow(contentRect: CGRect(origin: .zero, size: size), styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.title = L("pref.title")
        window.isReleasedWhenClosed = false
        window.isMovableByWindowBackground = false
        let sw = Theme.f("window.sidebarWidth")
        sidebar = PrefsSidebar(selected: "general")
        sidebar.frame = CGRect(x: 0, y: 0, width: sw, height: size.height)
        sidebarHost = WidgetHost(frame: sidebar.frame, root: sidebar)
        scroll = NSScrollView(frame: CGRect(x: sw, y: 0, width: size.width - sw, height: size.height))
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.scrollerStyle = .overlay
        scroll.autohidesScrollers = true
        let content = PrefsContentView(frame: CGRect(origin: .zero, size: size))
        content.addSubview(scroll)
        content.addSubview(sidebarHost)
        super.init()
        // 从系统设置切回来时刷新权限页的授权状态
        NotificationCenter.default.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            guard let self = self, self.page == "permissions", self.window.isVisible else { return }
            self.rebuild()
        }
        window.contentView = content
        window.delegate = self
        sidebar.onSelect = { [weak self] id in self?.select(id) }
        applyTheme()
        rebuild()
        NotificationCenter.default.addObserver(self, selector: #selector(settingsChanged(_:)), name: Settings.didChange, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(themeChanged), name: Theme.didChange, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(hotkeysChanged), name: Hotkeys.didChange, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(languageChanged), name: I18n.didChange, object: nil)
    }

    func select(_ id: String) {
        guard PrefsPages.all.contains(where: { $0.id == id }) else { return }
        page = id
        sidebar.selected = id
        sidebarHost.needsDisplay = true
        rebuild(resetScroll: true)
    }

    func rebuild(resetScroll: Bool = false) {
        NativeEditor.end()
        // 先让正在录入的快捷键框结束录入（恢复全局快捷键）
        contentHost?.setFocus(nil)
        let oldOffset = scroll.contentView.bounds.origin
        let w = scroll.frame.width
        let f = PrefsPages.build(page, window: window)
        f.frame = CGRect(x: 0, y: 0, width: w, height: 0)
        let h = max(scroll.frame.height, f.layoutPage(width: w))
        f.frame.size.height = h
        _ = f.layoutPage(width: w)
        let host = WidgetHost(frame: CGRect(x: 0, y: 0, width: w, height: h), root: f)
        host.background = { ctx, r in Draw.fill(ctx, r, Theme.color("surface")) }
        form = f
        contentHost = host
        scroll.documentView = host
        if !resetScroll { scroll.contentView.scroll(to: oldOffset); scroll.reflectScrolledClipView(scroll.contentView) }
        window.title = L("pref.title")
    }

    private func applyTheme() {
        window.appearance = Theme.appearance
        window.backgroundColor = NSColor(cgColor: Theme.color("surface"))
        sidebarHost.needsDisplay = true
        contentHost?.needsDisplay = true
    }

    @objc private func settingsChanged(_ n: Notification) {
        let key = n.userInfo?["key"] as? String ?? ""
        if PrefsPages.needsRebuild(page: page, key: key) {
            DispatchQueue.main.async { [weak self] in self?.rebuild() }
        }
    }

    @objc private func themeChanged() { applyTheme() }
    @objc private func hotkeysChanged() { if page == "shortcuts" { DispatchQueue.main.async { [weak self] in self?.rebuild() } } }
    @objc private func languageChanged() {
        sidebarHost.needsDisplay = true
        rebuild()
    }

    func windowDidBecomeKey(_ notification: Notification) {
        if page == "permissions" { rebuild() }
    }

    func windowWillClose(_ notification: Notification) {
        NativeEditor.end()
        contentHost?.setFocus(nil)
        PopupPanel.closeAll()
    }

    // MARK: 自测

    /// 整窗画进一张图：侧边栏 + 当前页。内容超出 520 时加高，便于对照全部控件。
    static func render(page: String, scale: CGFloat) -> CGImage? {
        let size = PrefsWindowController.size
        let sw = Theme.f("window.sidebarWidth")
        let f = PrefsPages.build(page, window: nil)
        f.frame = CGRect(x: sw, y: 0, width: size.width - sw, height: 0)
        let h = max(size.height, f.layoutPage(width: size.width - sw))
        f.frame.size.height = h
        _ = f.layoutPage(width: size.width - sw)
        let sidebar = PrefsSidebar(selected: page)
        sidebar.frame = CGRect(x: 0, y: 0, width: sw, height: h)
        return Offscreen.render(size: CGSize(width: size.width, height: h), scale: scale) { ctx in
            Draw.fill(ctx, CGRect(x: sw, y: 0, width: size.width - sw, height: h), Theme.color("surface"))
            sidebar.draw(ctx)
            f.draw(ctx)
        }
    }
}

final class PrefsContentView: NSView {
    override var isFlipped: Bool { true }
}
