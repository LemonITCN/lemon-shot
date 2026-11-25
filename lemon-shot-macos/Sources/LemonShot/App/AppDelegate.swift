import AppKit

/// 菜单栏应用（LSUIElement）：状态栏图标 + 原生菜单、全局快捷键、设置变更生效。
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem?
    private let menu = NSMenu()

    func applicationDidFinishLaunching(_ notification: Notification) {
        Settings.shared.load()
        AppState.shared.load()
        I18n.load()
        PinLibrary.shared.load()
        NSApp.appearance = Theme.appearance

        setupStatusItem()
        Hotkeys.shared.handler = { id in Features.run(id) }
        let failed = Hotkeys.shared.registerAll()
        if let first = ShortcutTable.globalIDs.first(where: { failed.contains($0) }) {
            Toast.show(L("notify.hotkeyFailed", ["key": Shortcut.display(ShortcutTable.global(first))]), icon: "triangle-alert")
        }

        NotificationCenter.default.addObserver(self, selector: #selector(settingsChanged(_:)), name: Settings.didChange, object: nil)
        DistributedNotificationCenter.default().addObserver(self, selector: #selector(systemAppearanceChanged), name: NSNotification.Name("AppleInterfaceThemeChangedNotification"), object: nil)

        if !ScreenCapture.hasPermission {
            PrefsWindowController.show(page: "permissions")
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        PrefsWindowController.show(page: nil)
        return false
    }

    private func setupStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = Icons.statusBarImage()
        item.button?.setAccessibilityLabel(L("app.name"))
        menu.delegate = self
        item.menu = menu
        item.isVisible = Settings.shared.bool("general.showStatusIcon")
        statusItem = item
        // 录屏时图标换成红点 + 计时（单击停止）
        RecordStatusIcon.attach(item, menu: menu)
        AppMenu.build(into: menu)
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        AppMenu.build(into: menu)
    }

    @objc private func settingsChanged(_ n: Notification) {
        let key = n.userInfo?["key"] as? String ?? ""
        let all = key.isEmpty
        if all || key.hasPrefix("general.language") { I18n.reloadIfNeeded() }
        if all || key.hasPrefix("general.theme") { applyTheme() }
        if all || key.hasPrefix("general.showStatusIcon") { statusItem?.isVisible = Settings.shared.bool("general.showStatusIcon") }
        // 整体变更只来自「恢复默认」「导入」：开机启动跟着设置走，否则开关显示与系统实际不一致
        if all || key.hasPrefix("general.launchAtLogin") { LoginItem.set(Settings.shared.bool("general.launchAtLogin")) }
        if all || key.hasPrefix("shortcuts") { Hotkeys.shared.registerAll() }
    }

    @objc private func systemAppearanceChanged() {
        if Settings.shared.string("general.theme") == "system" { applyTheme() }
    }

    private func applyTheme() {
        NSApp.appearance = Theme.appearance
        NotificationCenter.default.post(name: Theme.didChange, object: nil)
    }
}
