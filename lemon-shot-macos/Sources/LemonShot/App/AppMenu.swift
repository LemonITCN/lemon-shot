import AppKit

/// 菜单栏菜单（spec 12）。同一份数据建原生 NSMenu，也给自测画一份。
struct MenuEntry {
    var id: String
    var title: String
    /// 全局快捷键 id（右侧显示）。
    var shortcutID: String?
    var action: () -> Void
}

enum AppMenu {
    /// nil 表示分隔线。
    static func entries() -> [MenuEntry?] {
        var out: [MenuEntry?] = []
        if RecordFeature.isRecording {
            out.append(MenuEntry(id: "stopRecording", title: L("menu.stopRecording"), shortcutID: nil) { RecordFeature.stop() })
            out.append(MenuEntry(id: "pauseRecording", title: L(RecordFeature.isPaused ? "menu.resumeRecording" : "menu.pauseRecording"), shortcutID: "recordPause") { RecordFeature.togglePause() })
            out.append(nil)
        }
        out += [
            MenuEntry(id: "capture", title: L("menu.capture"), shortcutID: "capture") { Features.run("capture") },
            MenuEntry(id: "captureWindow", title: L("menu.captureWindow"), shortcutID: "captureWindow") { Features.run("captureWindow") },
            MenuEntry(id: "captureFullscreen", title: L("menu.captureFullscreen"), shortcutID: "captureFullscreen") { Features.run("captureFullscreen") },
            MenuEntry(id: "captureLastArea", title: L("menu.captureLastArea"), shortcutID: "captureLastArea") { Features.run("captureLastArea") },
            MenuEntry(id: "captureDelayed", title: L("menu.captureDelayed"), shortcutID: "captureDelayed") { Features.run("captureDelayed") },
            MenuEntry(id: "captureShell", title: L("menu.captureShell"), shortcutID: "captureShell") { Features.run("captureShell") },
            MenuEntry(id: "longshot", title: L("menu.longshot"), shortcutID: "longshot") { Features.run("longshot") },
            nil,
            MenuEntry(id: "record", title: L("menu.record"), shortcutID: "record") { Features.run("record") },
            MenuEntry(id: "recordAudio", title: L("menu.recordAudio"), shortcutID: "recordAudio") { Features.run("recordAudio") },
            nil,
            MenuEntry(id: "ocr", title: L("menu.ocr"), shortcutID: "ocr") { Features.run("ocr") },
            MenuEntry(id: "translate", title: L("menu.translate"), shortcutID: "translate") { Features.run("translate") },
            MenuEntry(id: "colorPicker", title: L("menu.colorPicker"), shortcutID: "colorPicker") { Features.run("colorPicker") },
            nil,
            MenuEntry(id: "pinClipboard", title: L("menu.pinClipboard"), shortcutID: "pinClipboard") { Features.run("pinClipboard") },
            MenuEntry(id: "pinLibrary", title: L("menu.pinLibrary"), shortcutID: "pinLibrary") { Features.run("pinLibrary") },
            MenuEntry(id: "togglePins", title: L("menu.togglePins"), shortcutID: "togglePins") { Features.run("togglePins") },
            MenuEntry(id: "closePins", title: L("menu.closePins"), shortcutID: nil) { PinManager.shared.closeAll() },
            nil,
            MenuEntry(id: "openFolder", title: L("menu.openFolder"), shortcutID: nil) { Features.openScreenshotsFolder() },
            MenuEntry(id: "preferences", title: L("menu.preferences"), shortcutID: nil) { PrefsWindowController.show(page: nil) },
            MenuEntry(id: "about", title: L("menu.about"), shortcutID: nil) { PrefsWindowController.show(page: "about") },
            nil,
            MenuEntry(id: "quit", title: L("menu.quit"), shortcutID: nil) { NSApp.terminate(nil) },
        ]
        return out
    }

    static func build(into menu: NSMenu) {
        menu.removeAllItems()
        menu.autoenablesItems = false
        for e in entries() {
            guard let e = e else { menu.addItem(.separator()); continue }
            let item = ClosureMenuItem(title: e.title, action: e.action)
            if let sid = e.shortcutID, let (k, mods) = Shortcut.menuEquivalent(ShortcutTable.global(sid)) {
                item.keyEquivalent = k
                item.keyEquivalentModifierMask = mods
            }
            if e.id == "togglePins" || e.id == "closePins" { item.isEnabled = !PinManager.shared.pins.isEmpty }
            menu.addItem(item)
        }
    }
}

/// 自测：把菜单画成一张图（系统菜单风格，便于两端对照）。
final class MenuRenderWidget: Widget {
    static let rowH: CGFloat = 22
    static let sepH: CGFloat = 11
    static let padY: CGFloat = 5
    static let width: CGFloat = 300

    static func height(_ entries: [MenuEntry?]) -> CGFloat {
        entries.reduce(2 * padY) { $0 + ($1 == nil ? sepH : rowH) }
    }

    let entries: [MenuEntry?]
    var hoverIndex = 0
    init(entries: [MenuEntry?]) { self.entries = entries }

    override func draw(_ ctx: CGContext) {
        Draw.panel(ctx, frame, radius: 8, fill: Theme.color("popoverBg"), border: Theme.color("border"), shadow: Theme.color("shadow"), blur: 16, offsetY: 4)
        var y = frame.minY + MenuRenderWidget.padY
        let font = Fonts.ui(Fonts.body)
        for (i, e) in entries.enumerated() {
            guard let e = e else {
                Draw.fill(ctx, CGRect(x: frame.minX + 10, y: y + MenuRenderWidget.sepH / 2, width: frame.width - 20, height: 1), Theme.color("divider"))
                y += MenuRenderWidget.sepH
                continue
            }
            let r = CGRect(x: frame.minX + 5, y: y, width: frame.width - 10, height: MenuRenderWidget.rowH)
            let hot = i == hoverIndex
            if hot { Draw.fill(ctx, r, Theme.color("accent"), radius: 4) }
            let tc = hot ? Theme.color("accentText") : Theme.color("text")
            Draw.text(ctx, e.title, font: font, color: tc, in: CGRect(x: r.minX + 10, y: r.minY, width: r.width - 90, height: r.height))
            if let sid = e.shortcutID {
                let k = Shortcut.display(ShortcutTable.global(sid))
                Draw.text(ctx, k, font: font, color: hot ? tc : Theme.color("textSecondary"), in: CGRect(x: r.maxX - 90, y: r.minY, width: 80, height: r.height), align: .right)
            }
            y += MenuRenderWidget.rowH
        }
    }
}
