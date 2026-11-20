import AppKit
import LemonShotCore

/// 偏好设置各页（spec 11）。
enum PrefsPages {
    struct Page { let id: String; let icon: String; let titleKey: String }

    static let all: [Page] = [
        Page(id: "general", icon: "settings", titleKey: "pref.tab.general"),
        Page(id: "capture", icon: "crop", titleKey: "pref.tab.capture"),
        Page(id: "annotate", icon: "pencil", titleKey: "pref.tab.annotate"),
        Page(id: "save", icon: "download", titleKey: "pref.tab.save"),
        Page(id: "pin", icon: "pin", titleKey: "pref.tab.pin"),
        Page(id: "color", icon: "pipette", titleKey: "pref.tab.color"),
        Page(id: "ocr", icon: "scan-text", titleKey: "pref.tab.ocr"),
        Page(id: "record", icon: "video", titleKey: "pref.tab.record"),
        Page(id: "audio", icon: "mic", titleKey: "pref.tab.audio"),
        Page(id: "shortcuts", icon: "keyboard", titleKey: "pref.tab.shortcuts"),
        Page(id: "permissions", icon: "shield-check", titleKey: "pref.tab.permissions"),
        Page(id: "about", icon: "info", titleKey: "pref.tab.about"),
    ]

    /// 这些键变化时整页重建（控件显隐、文案随之变化）。
    static func needsRebuild(page: String, key: String) -> Bool {
        if key.isEmpty || key.hasPrefix("general.language") || key.hasPrefix("shortcuts") { return true }
        switch page {
        case "save": return key == "save.format"
        case "ocr": return key == "translate.provider"
        case "annotate": return key == "annotate.palette"
        default: return false
        }
    }

    static func build(_ id: String, window: NSWindow?) -> FormPage {
        let title = L(all.first { $0.id == id }?.titleKey ?? "pref.tab.general")
        let p = FormPage(title: title)
        switch id {
        case "general": general(p, window)
        case "capture": capture(p)
        case "annotate": annotate(p)
        case "save": save(p)
        case "pin": pin(p)
        case "color": color(p)
        case "ocr": ocr(p)
        case "record": record(p)
        case "audio": audio(p)
        case "shortcuts": shortcuts(p)
        case "permissions": permissions(p)
        case "about": about(p)
        default: break
        }
        return p
    }

    private static func ms(_ v: Double) -> String { L("common.ms", ["n": String(Int(v))]) }
    private static func seconds(_ v: Double) -> String { L("common.seconds", ["n": String(Int(v))]) }
    private static func percent(_ v: Double) -> String { L("common.percent", ["n": String(Int(v.rounded()))]) }

    // MARK: 通用

    private static func general(_ p: FormPage, _ window: NSWindow?) {
        p.row(L("pref.general.language"), Bind.dropdown("general.language", [
            Option("system", L("pref.general.language.system")), Option("zh-Hans", L("lang.zh-Hans")), Option("en", L("lang.en")),
        ]))
        p.row(L("pref.general.launchAtLogin"), Bind.toggle("general.launchAtLogin"))
        p.row(L("pref.general.showStatusIcon.mac"), Bind.toggle("general.showStatusIcon"))
        p.row(L("pref.general.playSound"), Bind.toggle("general.playSound"))
        p.row(L("pref.general.theme"), Bind.segmented("general.theme", ["system", "light", "dark"],
                                                       [L("pref.general.theme.system"), L("pref.general.theme.light"), L("pref.general.theme.dark")]))
        p.buttons([
            ButtonW(L("pref.general.export"), icon: "download") {
                let panel = NSSavePanel()
                panel.nameFieldStringValue = "Lemon Shot Settings.json"
                if panel.runModal() == .OK, let url = panel.url { _ = Settings.shared.exportFile(url) }
            },
            ButtonW(L("pref.general.import"), icon: "folder-open") {
                let panel = NSOpenPanel()
                panel.allowsMultipleSelection = false
                panel.canChooseDirectories = false
                if panel.runModal() == .OK, let url = panel.url, !Settings.shared.importFile(url) {
                    Toast.show(L("common.error"), icon: "triangle-alert")
                }
            },
            ButtonW(L("pref.general.reset"), kind: .danger, icon: "rotate-ccw") {
                let a = NSAlert()
                a.messageText = L("pref.general.resetConfirm")
                a.addButton(withTitle: L("common.ok"))
                a.addButton(withTitle: L("common.cancel"))
                let run = { (r: NSApplication.ModalResponse) in if r == .alertFirstButtonReturn { Settings.shared.resetAll() } }
                if let w = window { a.beginSheetModal(for: w, completionHandler: run) } else { run(a.runModal()) }
            },
        ])
    }

    // MARK: 截图

    static func actionOptions(_ keys: [String]) -> [Option] {
        keys.map { Option($0, L("pref.capture.action.\($0)")) }
    }

    private static func capture(_ p: FormPage) {
        p.row(L("pref.capture.detectWindows"), Bind.toggle("capture.detectWindows"))
        p.row(L("pref.capture.captureCursor"), Bind.toggle("capture.captureCursor"))
        p.row(L("pref.capture.showCrosshair"), Bind.toggle("capture.showCrosshair"))
        p.row(L("pref.capture.showMagnifier"), Bind.toggle("capture.showMagnifier"))
        p.row(L("pref.capture.showTips"), [Bind.toggle("capture.showTips"), Bind.color("capture.tipsColor")])
        p.row(L("pref.capture.rightClickExits"), Bind.toggle("capture.rightClickExits"))
        p.row(L("pref.capture.returnToPreviousApp"), Bind.toggle("capture.returnToPreviousApp"))
        p.row(L("pref.capture.toolbarPopupDelay"), Bind.slider("capture.toolbarPopupDelayMs", min: 0, max: 1500, step: 50, format: ms))
        p.row(L("pref.capture.doubleClickAction"), Bind.dropdown("capture.doubleClickAction", actionOptions(["copy", "save", "pin", "none"])))
        p.row(L("pref.capture.doubleModifierAction", ["key": "Option"]), Bind.dropdown("capture.doubleModifierAction", actionOptions(["copy", "save", "pin", "saveAndOpen", "none"])))
        p.row(L("pref.capture.openWithApp"), openWithDropdown())
        let w = Bind.stepper("capture.presetWidth", min: 16, max: 10000, step: 10)
        let x = Label("×", font: Fonts.ui(Fonts.body), colorKey: "textSecondary")
        x.frame.size = CGSize(width: 10, height: 28)
        let h = Bind.stepper("capture.presetHeight", min: 16, max: 10000, step: 10)
        let px = Label(L("common.pixels", ["n": ""]).trimmingCharacters(in: .whitespaces), font: Fonts.ui(Fonts.body), colorKey: "textSecondary")
        px.frame.size = CGSize(width: Draw.textWidth(px.text, px.font) + 2, height: 28)
        p.row(L("pref.capture.preset"), [w, x, h, px])
        p.row(L("pref.capture.delay"), Bind.stepper("capture.delaySeconds", min: 1, max: 60, format: seconds))
        p.row(L("pref.capture.delaySound"), Bind.toggle("capture.delaySound"))

        p.group(L("pref.capture.window"))
        p.row(L("pref.capture.windowCorners"), [Bind.toggle("capture.windowCorners"), Bind.stepper("capture.windowCornerRadius", min: 0, max: 40)])
        p.row(L("pref.capture.windowShadow"), [Bind.toggle("capture.windowShadow"), Bind.stepper("capture.windowShadowRadius", min: 0, max: 80)])
        p.row(L("pref.capture.shadowColor"), Bind.color("capture.windowShadowColor", alpha: true))

        p.group(L("pref.capture.multiWindow"))
        p.row(L("pref.capture.windowCorners"), Bind.toggle("capture.multiWindowCorners"))
        p.row(L("pref.capture.windowShadow"), Bind.toggle("capture.multiWindowShadow"))
    }

    private static func openWithDropdown() -> DropdownW {
        let cur = Settings.shared.string("capture.openWithApp")
        var opts = [Option("", L("pref.capture.openWithApp.default")), Option("folder", L("pref.capture.openWithApp.folder"))]
        if !cur.isEmpty && cur != "folder" {
            opts.append(Option(cur, FileManager.default.displayName(atPath: cur).replacingOccurrences(of: ".app", with: "")))
        }
        opts.append(Option("__choose__", L("common.choose")))
        let d = DropdownW(opts, value: cur, width: 220)
        d.onChange = { [weak d] v in
            guard v == "__choose__" else { Settings.shared.set("capture.openWithApp", v); return }
            let panel = NSOpenPanel()
            panel.directoryURL = URL(fileURLWithPath: "/Applications")
            panel.allowedFileTypes = ["app"]
            panel.allowsMultipleSelection = false
            if panel.runModal() == .OK, let url = panel.url {
                Settings.shared.set("capture.openWithApp", url.path)
                var o = d?.options ?? []
                o.removeAll { $0.value != "" && $0.value != "folder" && $0.value != "__choose__" }
                o.insert(Option(url.path, FileManager.default.displayName(atPath: url.path).replacingOccurrences(of: ".app", with: "")), at: 2)
                d?.options = o
                d?.value = url.path
            } else {
                d?.value = Settings.shared.string("capture.openWithApp")
            }
        }
        return d
    }

    // MARK: 标注

    private static func annotate(_ p: FormPage) {
        p.row(L("style.color"), PaletteEditW())
        for t in Int32(1)...Int32(10) {
            let controls = StyleControls(tool: t, get: { AnnotStyles.style(for: t) }, set: { AnnotStyles.save($0, for: t) }, includeNext: false)
            // 组间分隔换成留白
            let row1: [Widget] = controls.row1.map { w -> Widget in
                if let w = w { return w }
                let gap = Widget()
                gap.frame.size = CGSize(width: 10, height: 28)
                return gap
            }
            if t == Int32(LS_TOOL_MOSAIC.rawValue) {
                p.row(L("tool." + AnnotStyles.key(t)), row1)
                let extra = controls.row2Extra
                if extra.count >= 4 {
                    p.row("", [extra[0], extra[1]])
                    extra[2].isHidden = false
                    extra[3].isHidden = false
                    p.row("", [extra[2], extra[3]])
                }
            } else {
                p.row(L("tool." + AnnotStyles.key(t)), row1)
            }
            p.keepAlive.append(controls)
        }
    }

    // MARK: 保存

    private static func save(_ p: FormPage) {
        p.row(L("pref.save.enterAction"), Bind.dropdown("save.enterAction", ["copy", "save", "both"].map { Option($0, L("pref.save.enterAction.\($0)")) }))
        p.row(L("pref.save.alsoCopy"), Bind.toggle("save.alsoCopyWhenSaving"))
        p.row(L("pref.save.folder"), Bind.path("save.folder"))
        p.row(L("pref.save.askLocation"), Bind.toggle("save.askLocation"))
        p.row(L("pref.save.format"), Bind.dropdown("save.format", [
            Option("png", "PNG"), Option("jpg", "JPG"), Option("webp", "WebP"), Option("tiff", "TIFF"), Option("bmp", "BMP"), Option("pdf", "PDF"), Option("heic", "HEIC"),
        ], width: 140))
        let fmt = Settings.shared.string("save.format")
        if fmt == "jpg" || fmt == "heic" {
            p.row(L("pref.save.jpegQuality"), Bind.slider("save.jpegQuality", min: 10, max: 100, step: 1, format: percent))
        }
        p.row(L("pref.save.resolution"), Bind.segmented("save.resolution", ["native", "standard"], [L("pref.save.resolution.native"), L("pref.save.resolution.standard")]))
        let example = Label("", font: Fonts.ui(Fonts.caption), colorKey: "textSecondary")
        func refresh() { example.text = Export.fileName(template: Settings.shared.string("save.fileName")) + "." + Export.fileExtension(Settings.shared.string("save.format")) }
        refresh()
        p.row(L("pref.save.fileName"), Bind.text("save.fileName", width: 300) { _ in refresh() })
        p.value(example)
        p.row(L("pref.save.colorProfile"), Bind.segmented("save.colorProfile", ["display", "srgb"], [L("pref.save.colorProfile.display"), L("pref.save.colorProfile.srgb")]))
    }

    // MARK: 贴图

    private static func pin(_ p: FormPage) {
        p.row(L("pref.pin.corners"), [Bind.toggle("pin.corners"), Bind.stepper("pin.cornerRadius", min: 0, max: 40)])
        p.row(L("pref.pin.shadow"), Bind.toggle("pin.shadow"))
        p.row(L("pref.pin.opacity"), Bind.slider("pin.opacity", min: 10, max: 100, step: 5, scale: 100, format: percent))
        p.row(L("pref.pin.doubleClickCloses"), Bind.toggle("pin.doubleClickCloses"))
        p.row(L("pref.pin.allDesktops"), Bind.toggle("pin.allDesktops") { v in for w in PinManager.shared.pins { w.applyAllDesktops(v) } })
        p.row(L("pref.pin.showButtons"), Bind.toggle("pin.showButtons"))
        p.row(L("pref.pin.libraryMax"), Bind.stepper("pin.libraryMax", min: 10, max: 500, step: 10, format: { L("common.items", ["n": String(Int($0))]) }))
    }

    // MARK: 取色

    private static func color(_ p: FormPage) {
        p.row(L("pref.color.space"), Bind.dropdown("color.space", ["native", "srgb", "p3", "adobe", "generic"].map { Option($0, L("pref.color.space.\($0)")) }))
        p.row(L("pref.color.hexPrefix"), Bind.toggle("color.hexPrefix"))
        p.row(L("pref.color.hexUppercase"), Bind.toggle("color.hexUppercase"))
        let example = Label("", font: Fonts.mono(Fonts.caption), colorKey: "textSecondary")
        let sample = ls_color(r: 47, g: 107, b: 255, a: 255)
        func refresh() { example.text = ColorValue.custom(sample) }
        refresh()
        p.row(L("pref.color.customFormat"), [Bind.text("color.customFormat", width: 260) { _ in refresh() }], info: L("pref.color.customFormatHint"))
        p.value(example)
    }

    // MARK: 识别与翻译

    static let providers = ["microsoft", "baidu", "tencent", "youdao", "deepl", "openai"]
    static let targetLangs = ["zh-Hans", "zh-Hant", "en", "ja", "ko", "fr", "de", "es", "ru", "pt", "it", "ar", "th", "vi"]

    private static func ocr(_ p: FormPage) {
        p.group(L("ocr.title"))
        p.row(L("pref.ocr.preferQRCode"), Bind.toggle("ocr.preferQRCode"))
        p.row(L("pref.ocr.joinLines"), Bind.toggle("ocr.joinLines"))
        p.row(L("pref.ocr.forceAscii"), Bind.toggle("ocr.forceAsciiPunctuation"))
        p.row(L("pref.ocr.continuous"), Bind.toggle("ocr.continuous"))
        p.row(L("pref.ocr.keepOnTop"), Bind.toggle("ocr.keepOnTop"))
        p.row(L("pref.ocr.closeAfterCopy"), Bind.toggle("ocr.closeAfterCopy"))
        p.group(L("pref.ocr.translate"))
        p.row(L("translate.provider"), Bind.dropdown("translate.provider", providers.map { Option($0, L("translate.provider.\($0)")) }, width: 260))
        p.row(L("pref.translate.target"), Bind.dropdown("translate.target", [Option("auto", L("translate.auto"))] + targetLangs.map { Option($0, L("lang.\($0)")) }))
        let provider = Settings.shared.string("translate.provider")
        let fields: [(String, String, Bool)]
        switch provider {
        case "baidu": fields = [("appId", "pref.translate.appId", false), ("secret", "pref.translate.secret", true)]
        case "tencent": fields = [("secretId", "pref.translate.secretId", false), ("secretKey", "pref.translate.secretKey", true), ("region", "pref.translate.region", false)]
        case "youdao": fields = [("appKey", "pref.translate.appKey", false), ("appSecret", "pref.translate.appSecret", true)]
        case "deepl": fields = [("authKey", "pref.translate.authKey", true)]
        case "openai": fields = [("baseUrl", "pref.translate.baseUrl", false), ("apiKey", "pref.translate.apiKey", true), ("model", "pref.translate.model", false)]
        default: fields = []
        }
        for (k, label, secure) in fields {
            p.row(L(label), Bind.text("translate.\(provider).\(k)", secure: secure, width: 300))
        }
        p.row("", ButtonW(L("translate.test")) { TranslateFeature.testProvider(provider) })
    }

    // MARK: 录屏

    static func deviceOptions(audio: Bool) -> [Option] {
        [Option("", L("record.default"))] + Devices.list(audio: audio).map { Option($0.0, $0.1) }
    }

    private static func record(_ p: FormPage) {
        p.row(L("pref.record.folder"), Bind.path("record.folder"))
        p.row(L("record.format"), Bind.segmented("record.format", ["mp4", "gif"], ["MP4", "GIF"]))
        p.row(L("record.quality"), Bind.segmented("record.quality", ["ultra", "high", "standard"], ["ultra", "high", "standard"].map { L("record.quality.\($0)") }))
        p.row(L("record.fps"), Bind.segmented("record.fps", ["15", "24", "30", "60"], ["15", "24", "30", "60"]))
        p.row(L("pref.record.gifFps"), Bind.stepper("record.gifFps", min: 5, max: 30, format: { L("common.fps", ["n": String(Int($0))]) }))
        p.row(L("pref.record.gifScale"), Bind.segmented("record.gifScale", ["1", "0.75", "0.5"], ["100%", "75%", "50%"].map { L("common.percent", ["n": String($0.dropLast())]) }))
        p.row(L("pref.record.microphoneDevice"), Bind.dropdown("record.microphoneDevice", deviceOptions(audio: true), width: 260))
        p.row(L("record.systemAudio"), RecordUI.systemAudioSwitch("record.systemAudio"))
        p.row(L("pref.record.cameraDevice"), Bind.dropdown("record.cameraDevice", deviceOptions(audio: false), width: 260))
        p.row(L("pref.record.cameraShape"), Bind.segmented("record.cameraShape", ["circle", "rounded"], [L("pref.record.cameraShape.circle"), L("pref.record.cameraShape.rounded")]))
        p.row(L("record.showCursor"), Bind.toggle("record.showCursor"))
        p.row(L("record.showClicks"), Bind.toggle("record.showClicks"))

        p.group(L("pref.record.keys"))
        p.row(L("record.showKeys"), Bind.toggle("record.showKeys"))
        p.row(L("pref.record.keys.theme"), Bind.segmented("record.keys.theme", ["auto", "light", "dark"], [L("common.auto"), L("pref.general.theme.light"), L("pref.general.theme.dark")]))
        p.row(L("pref.record.keys.size"), Bind.segmented("record.keys.size", ["small", "medium", "large"], ["small", "medium", "large"].map { L("pref.record.keys.size.\($0)") }))
        p.row(L("pref.record.keys.fade"), Bind.slider("record.keys.fadeMs", min: 500, max: 5000, step: 100, format: ms))
        p.row(L("pref.record.keys.max"), Bind.stepper("record.keys.maxKeys", min: 1, max: 10))
        p.row(L("pref.record.keys.onlyShortcuts"), Bind.toggle("record.keys.onlyShortcuts"))
        p.row(L("pref.record.keys.hideRepeats"), Bind.toggle("record.keys.hideRepeats"))
        p.row(L("pref.record.keys.position"), Bind.dropdown("record.keys.position", ["bottomCenter", "bottomLeft", "bottomRight", "topCenter"].map { Option($0, L("pref.record.keys.position.\($0)")) }))

        p.group("")
        p.row(L("record.countdown"), Bind.stepper("record.countdown", min: 0, max: 10, format: seconds))
        p.row(L("record.maxDuration"), Bind.stepper("record.maxDurationSec", min: 0, max: 180, step: 1, scale: 60, width: 150, format: { $0 == 0 ? L("record.unlimited") : L("common.minutes", ["n": String(Int($0))]) }))
        p.row(L("pref.record.afterAction"), Bind.dropdown("record.afterAction", ["none", "play", "reveal"].map { Option($0, L("pref.record.afterAction.\($0)")) }))
    }

    // MARK: 录音

    private static func audio(_ p: FormPage) {
        p.row(L("pref.audio.folder"), Bind.path("audio.folder"))
        p.row(L("pref.audio.format"), Bind.dropdown("audio.format", [Option("m4a", "M4A (AAC)"), Option("wav", "WAV"), Option("mp3", "MP3", enabled: false)], width: 160))
        p.row(L("pref.record.microphoneDevice"), Bind.dropdown("audio.microphoneDevice", deviceOptions(audio: true), width: 260))
        p.row(L("record.systemAudio"), RecordUI.systemAudioSwitch("audio.systemAudio"))
    }

    // MARK: 快捷键

    private static func shortcuts(_ p: FormPage) {
        p.group(L("pref.shortcuts.global"))
        let failed = Hotkeys.shared.failed
        for id in ShortcutTable.globalIDs {
            let h = HotkeyW(ShortcutTable.global(id)) { v in ShortcutTable.setGlobal(id, v) }
            h.conflict = failed.contains(id)
            var row: [Widget] = [h]
            if h.conflict {
                let l = Label(L("pref.shortcuts.conflict"), font: Fonts.ui(Fonts.caption + 1), colorKey: "danger")
                l.frame.size = CGSize(width: Draw.textWidth(l.text, l.font) + 2, height: 28)
                row.append(l)
            }
            p.row(L("shortcut.\(id)"), row)
        }
        p.group(L("pref.shortcuts.capture"))
        for id in ShortcutTable.captureIDs {
            p.row(L("shortcut.\(id)"), HotkeyW(ShortcutTable.capture(id), singleKey: true, width: 110) { v in ShortcutTable.setCapture(id, v) })
        }
        p.buttons([ButtonW(L("pref.shortcuts.reset"), icon: "rotate-ccw") { ShortcutTable.resetAll() }])
    }

    // MARK: 权限

    private static func permissions(_ p: FormPage) {
        p.group(L("perm.intro"), info: L("perm.restartHint"))
        for k in Permissions.Kind.allCases {
            let ok = Permissions.granted(k)
            let status = Label(L(ok ? "perm.granted" : "perm.denied"), font: Fonts.medium(Fonts.body), colorKey: ok ? "success" : "textSecondary")
            status.frame.size = CGSize(width: 80, height: 28)
            var row: [Widget] = [status]
            if !ok {
                row.append(ButtonW(L("perm.grant")) {
                    Permissions.request(k) { PrefsWindowController.current?.rebuild() }
                })
            }
            p.row(L(k.titleKey), row, info: L(k.descKey))
        }
    }

    // MARK: 关于

    static let thirdParty = [
        "Lucide icons — ISC",
        "libwebp — BSD-3-Clause",
        "zxing-cpp — Apache-2.0",
        "ncnn — BSD-3-Clause",
        "PaddleOCR models — Apache-2.0",
    ]

    private static func about(_ p: FormPage) {
        p.custom(AboutWidget(), height: 230)
        func link(_ key: String, _ icon: String, _ url: String) -> ButtonW {
            ButtonW(L(key), icon: icon) { if let u = URL(string: url) { NSWorkspace.shared.open(u) } }
        }
        let third = ButtonW(L("about.thirdParty"), icon: "layers") {
            let a = NSAlert()
            a.messageText = L("about.thirdParty")
            a.informativeText = thirdParty.joined(separator: "\n")
            a.addButton(withTitle: L("common.ok"))
            a.runModal()
        }
        p.custom(CenteredRow([
            link("about.website", "globe", "https://lemonit.cn"),
            link("about.source", "external-link", "https://github.com/LemonITCN/lemon-shot"),
            link("about.license", "scroll-text", "https://github.com/LemonITCN/lemon-shot/blob/main/LICENSE"),
            third,
        ]), height: Theme.f("window.controlHeight"))
    }
}

/// 关于页上半部分：图标、名称、版本、一句话介绍。
final class AboutWidget: Widget {
    override func draw(_ ctx: CGContext) {
        let size: CGFloat = 96
        let icon = CGRect(x: frame.midX - size / 2, y: frame.minY + 8, width: size, height: size)
        AppIcon.draw(ctx, icon)
        var y = icon.maxY + 16
        Draw.text(ctx, L("app.name"), font: Fonts.semibold(Fonts.header + 2), color: Theme.color("text"), in: CGRect(x: frame.minX, y: y, width: frame.width, height: 30), align: .center)
        y += 32
        let v = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? String(cString: ls_version())
        Draw.text(ctx, L("about.version", ["version": v]), font: Fonts.ui(Fonts.caption + 1), color: Theme.color("textSecondary"), in: CGRect(x: frame.minX, y: y, width: frame.width, height: 18), align: .center)
        y += 26
        Draw.text(ctx, L("about.tagline"), font: Fonts.ui(Fonts.body), color: Theme.color("text"), in: CGRect(x: frame.minX, y: y, width: frame.width, height: 20), align: .center)
    }
}

/// 一行居中的按钮。
final class CenteredRow: Widget {
    init(_ ws: [Widget]) {
        super.init()
        for w in ws { add(w) }
    }

    override func layout() {
        let sizes = children.map { $0.preferredSize() }
        let total = sizes.reduce(0) { $0 + $1.width } + CGFloat(max(0, sizes.count - 1)) * 10
        var x = frame.midX - total / 2
        for (c, s) in zip(children, sizes) {
            c.frame = CGRect(x: x, y: frame.minY, width: s.width, height: s.height)
            x += s.width + 10
        }
    }
}

/// 应用图标（矢量自绘）：accent 圆角方底 + 白色取景框图标。
enum AppIcon {
    static func draw(_ ctx: CGContext, _ r: CGRect) {
        ctx.saveGState()
        Draw.setShadow(ctx, offsetY: 3, blur: 10, color: CGColor(gray: 0, alpha: 0.25))
        Draw.fill(ctx, r, Theme.tokenColor("color.light.accent"), radius: r.width * 0.225)
        ctx.restoreGState()
        Icons.draw(ctx, "scan", in: r.insetBy(dx: r.width * 0.2, dy: r.width * 0.2), color: CGColor(gray: 1, alpha: 1), lineWidth: r.width / 24 * 1.9)
        Icons.draw(ctx, "circle-dot", in: r.insetBy(dx: r.width * 0.36, dy: r.width * 0.36), color: CGColor(gray: 1, alpha: 1), lineWidth: r.width / 24 * 1.5)
    }
}

/// 标注页的调色板：10 色逐个可改（点开系统取色面板）。
final class PaletteEditW: Control {
    private var hover = -1
    override init() {
        super.init()
        let n = CGFloat(AnnotStyles.palette().count)
        frame.size = CGSize(width: n * 24 + (n - 1) * 8, height: 28)
    }

    private func rect(_ i: Int) -> CGRect { CGRect(x: frame.minX + CGFloat(i) * 32, y: frame.midY - 12, width: 24, height: 24) }

    override func draw(_ ctx: CGContext) {
        for (i, h) in AnnotStyles.palette().enumerated() {
            let r = rect(i)
            if i == hover { Draw.ellipse(ctx, r.insetBy(dx: -3, dy: -3), stroke: Theme.color("accent"), width: 2) }
            Draw.ellipse(ctx, r, fill: Theme.hex(h))
            Draw.ellipse(ctx, r, stroke: Theme.color("border"), width: 1)
        }
    }

    override func mouseMoved(_ p: CGPoint) {
        let h = AnnotStyles.palette().indices.first { rect($0).contains(p) } ?? -1
        if h != hover { hover = h; setNeedsDisplay() }
    }

    override func mouseExited() { hover = -1; super.mouseExited() }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        let pal = AnnotStyles.palette()
        guard let i = pal.indices.first(where: { rect($0).contains(p) }) else { return }
        ColorPanelBridge.shared.open(initial: pal[i], alpha: false) { [weak self] hex in
            var arr = AnnotStyles.palette()
            guard i < arr.count else { return }
            arr[i] = hex
            Settings.shared.set("annotate.palette", .array(arr.map { .string($0) }))
            self?.setNeedsDisplay()
        }
    }
}
