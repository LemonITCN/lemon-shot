import AppKit

/// 截图翻译窗口内容（spec 5）：顶部「自动 → 目标语言」与翻译服务，上原文（可编辑）下译文，底部复制译文。
final class TranslateContent: Widget {
    let header = ToolHeader(title: L("translate.title"))
    let sourceLang = Label(L("translate.auto"), font: Fonts.ui(Fonts.body), colorKey: "textSecondary")
    let target: DropdownW
    let provider: DropdownW
    let sourceCaption = CaptionW(L("translate.source"))
    let resultCaption = CaptionW(L("translate.result"))
    let source = TextAreaW()
    let result = TextAreaW()
    let footer = ToolFooter()
    let status = StatusW()
    let retranslate = ButtonW(L("action.translate"))
    let copy = ButtonW(L("translate.copyResult"), kind: .primary)
    private var arrowRect = CGRect.zero

    static let size = CGSize(width: 560, height: 420)
    static let rowH: CGFloat = 44
    static let footerH: CGFloat = 56

    static var targetOptions: [Option] { [Option("auto", L("translate.auto"))] + PrefsPages.targetLangs.map { Option($0, L("lang.\($0)")) } }
    static var providerOptions: [Option] { PrefsPages.providers.map { Option($0, L("translate.provider.\($0)")) } }

    override init() {
        let s = Settings.shared
        target = DropdownW(TranslateContent.targetOptions, value: s.string("translate.target"), width: 150)
        provider = DropdownW(TranslateContent.providerOptions, value: s.string("translate.provider"), width: 220)
        super.init()
        add(header)
        for w in [sourceLang, target, provider, sourceCaption, source, resultCaption, result] as [Widget] { add(w) }
        for w in [status, retranslate, copy] as [Widget] { footer.add(w) }
        add(footer)
    }

    override func layout() {
        let f = frame
        let th = Theme.f("window.titleBarHeight")
        header.frame = CGRect(x: f.minX, y: f.minY, width: f.width, height: th)
        let rowY = f.minY + th
        let ch = Theme.f("window.controlHeight")
        let cy = rowY + (TranslateContent.rowH - ch) / 2
        let lw = Draw.textWidth(sourceLang.text, sourceLang.font) + 2
        sourceLang.frame = CGRect(x: f.minX + 16, y: cy, width: lw, height: ch)
        arrowRect = CGRect(x: sourceLang.frame.maxX + 6, y: cy + (ch - 14) / 2, width: 14, height: 14)
        target.frame = CGRect(x: arrowRect.maxX + 6, y: cy, width: target.frame.width, height: ch)
        provider.frame = CGRect(x: f.maxX - 16 - provider.frame.width, y: cy, width: provider.frame.width, height: ch)

        let top = rowY + TranslateContent.rowH
        let bottom = f.maxY - TranslateContent.footerH - 12
        let capH: CGFloat = 20
        let areaH = floor((bottom - top - 2 * capH - 10) / 2)
        sourceCaption.frame = CGRect(x: f.minX + 18, y: top, width: f.width - 36, height: capH)
        source.frame = CGRect(x: f.minX + 16, y: sourceCaption.frame.maxY, width: f.width - 32, height: areaH)
        resultCaption.frame = CGRect(x: f.minX + 18, y: source.frame.maxY + 10, width: f.width - 36, height: capH)
        result.frame = CGRect(x: f.minX + 16, y: resultCaption.frame.maxY, width: f.width - 32, height: bottom - resultCaption.frame.maxY)

        footer.frame = CGRect(x: f.minX, y: f.maxY - TranslateContent.footerH, width: f.width, height: TranslateContent.footerH)
        let left = footer.layoutRight([copy, retranslate])
        status.frame = CGRect(x: f.minX + 16, y: footer.frame.midY - 10, width: max(0, left - 12 - f.minX - 16), height: 20)
    }

    override func draw(_ ctx: CGContext) {
        drawChildren(ctx)
        Icons.draw(ctx, "chevron-right", in: arrowRect, color: Theme.color("textTertiary"), lineWidth: 1.75)
    }
}

/// 截图翻译窗口：图片先识别再翻译；原文改动后点「翻译」或 ⌘↩ 重新翻译。
final class TranslateWindowController: ToolWindowController {
    private(set) static var current: TranslateWindowController?

    let content: TranslateContent
    let sourceEditor = NativeTextArea(editable: true)
    let resultView = NativeTextArea(editable: false)
    private var generation = 0
    private var keyMonitor: Any?

    init() {
        content = TranslateContent()
        super.init(size: TranslateContent.size, minSize: CGSize(width: 520, height: 360), title: L("translate.title"), root: content, resizable: true)
        host.addSubview(sourceEditor.scroll)
        host.addSubview(resultView.scroll)
        placeNative = { [weak self] in
            guard let self = self else { return }
            self.sourceEditor.place(in: self.content.source.frame)
            self.resultView.place(in: self.content.result.frame)
        }
        sourceEditor.onChange = { [weak self] _ in self?.content.source.overlay = nil }
        content.target.onChange = { [weak self] v in
            Settings.shared.set("translate.target", v)
            self?.run()
        }
        content.provider.onChange = { [weak self] v in
            Settings.shared.set("translate.provider", v)
            self?.run()
        }
        content.retranslate.action = { [weak self] in self?.run() }
        content.copy.action = { [weak self] in
            guard let self = self else { return }
            let pb = NSPasteboard.general
            pb.clearContents()
            pb.setString(self.resultView.string, forType: .string)
            Toast.show(L("common.copied"))
        }
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] e in
            guard let self = self, e.window === self.window else { return e }
            if e.keyCode == 36 || e.keyCode == 76, e.modifierFlags.contains(.command) { self.run(); return nil }
            return e
        }
        NotificationCenter.default.addObserver(self, selector: #selector(settingsChanged(_:)), name: Settings.didChange, object: nil)
        onClose = { [weak self] in
            guard let self = self else { return }
            if let m = self.keyMonitor { NSEvent.removeMonitor(m) }
            self.keyMonitor = nil
            self.generation += 1
            if TranslateWindowController.current === self { TranslateWindowController.current = nil }
        }
    }

    override func themeChanged() {
        sourceEditor.applyTheme()
        resultView.applyTheme()
    }

    @objc private func settingsChanged(_ n: Notification) {
        let key = n.userInfo?["key"] as? String ?? ""
        guard key.isEmpty || key.hasPrefix("translate") else { return }
        content.target.value = Settings.shared.string("translate.target")
        content.provider.value = Settings.shared.string("translate.provider")
    }

    private static func obtain() -> TranslateWindowController {
        if let c = current { return c }
        let c = TranslateWindowController()
        current = c
        c.show(center: true)
        return c
    }

    /// 识别图片文字后翻译。
    static func translate(image: CGImage) {
        let c = obtain()
        c.show(center: false)
        c.generation += 1
        let gen = c.generation
        c.sourceEditor.string = ""
        c.resultView.string = ""
        c.content.source.overlay = L("ocr.recognizing")
        c.content.result.overlay = nil
        c.setStatus(L("ocr.recognizing"), danger: false)
        OCREngine.recognize(image) { [weak c] out, err in
            guard let c = c, gen == c.generation else { return }
            c.content.source.overlay = nil
            guard let out = out else {
                c.setStatus(L("ocr.failed", ["error": err ?? ""]), danger: true)
                return
            }
            // 翻译时同一段落的行合并，译文更通顺
            var text = OCREngine.layout(out.lines, joinLines: true, asciiPunct: false)
            if text.isEmpty { text = out.codes.joined(separator: "\n") }
            c.sourceEditor.string = text
            if text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                c.content.source.overlay = L("ocr.empty")
                c.setStatus("", danger: false)
                return
            }
            c.run()
        }
    }

    static func translate(text: String) {
        let c = obtain()
        c.show(center: false)
        c.sourceEditor.string = text
        c.content.source.overlay = nil
        c.run()
    }

    /// 用当前原文、目标语言、服务翻译。
    func run() {
        generation += 1
        let gen = generation
        let text = sourceEditor.string
        guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        let provider = Settings.shared.string("translate.provider")
        resultView.string = ""
        content.result.overlay = L("translate.translating")
        setStatus(L("translate.translating"), danger: false)
        TranslateService.translate(text: text, provider: provider, target: Settings.shared.string("translate.target")) { [weak self] outcome in
            guard let self = self, gen == self.generation else { return }
            self.content.result.overlay = nil
            switch outcome {
            case .success(let s):
                self.resultView.string = s
                self.setStatus("", danger: false)
            case .needKey:
                self.setStatus(TranslateService.needKeyMessage(provider), danger: true)
            case .failure(let e):
                self.setStatus(L("translate.failed", ["error": e]), danger: true)
            }
        }
    }

    private func setStatus(_ s: String, danger: Bool) {
        content.status.text = s
        content.status.icon = danger ? "triangle-alert" : nil
        content.status.danger = danger
        host.needsDisplay = true
    }

    // MARK: 自测

    static func render(scale: CGFloat, source: String, result: String) -> CGImage? {
        let size = TranslateContent.size
        let c = TranslateContent()
        c.frame = CGRect(origin: .zero, size: size)
        c.source.drawsText = true
        c.source.text = source
        c.result.drawsText = true
        c.result.text = result
        c.layout()
        return Offscreen.render(size: size, scale: scale) { ctx in
            Draw.fill(ctx, CGRect(origin: .zero, size: size), Theme.color("windowBg"))
            c.draw(ctx)
        }
    }
}
