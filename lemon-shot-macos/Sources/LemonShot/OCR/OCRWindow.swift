import AppKit

/// 文字识别结果窗口的内容（spec 4）：顶部开关、左侧缩略图（可折叠）、中间文本区、底部状态与按钮。
/// 屏幕与自测共用这一棵控件树；屏幕上文本区由原生 NSTextView 负责输入。
final class OCRContent: Widget {
    let header = ToolHeader(title: L("ocr.title"))
    let keepOnTop: IconButton
    let joinLines: ToggleChip
    let asciiPunct: ToggleChip
    let continuous: ToggleChip
    let fixedRegion: ToggleChip
    let thumb = ImageBoxW()
    let textArea = TextAreaW()
    let footer = ToolFooter()
    let thumbToggle: IconButton
    let status = StatusW()
    let openLink = ButtonW(L("ocr.openLink"), icon: "external-link")
    let translate = ButtonW(L("ocr.translate"))
    let copyClose = ButtonW(L("ocr.copyClose"))
    let copy = ButtonW(L("ocr.copy"), kind: .primary)
    var thumbVisible = true

    static let size = CGSize(width: 520, height: 420)
    static let toolRowH: CGFloat = 44
    static let footerH: CGFloat = 56
    static let thumbW: CGFloat = 120

    override init() {
        let s = Settings.shared
        keepOnTop = IconButton(icon: "pin", size: 28, iconSize: 16)
        keepOnTop.tooltip = L("ocr.keepOnTop")
        keepOnTop.selected = s.bool("ocr.keepOnTop")
        joinLines = ToggleChip(L("ocr.joinLines"), isOn: s.bool("ocr.joinLines"))
        asciiPunct = ToggleChip(L("ocr.forceAscii"), isOn: s.bool("ocr.forceAsciiPunctuation"))
        continuous = ToggleChip(L("ocr.continuous"), isOn: s.bool("ocr.continuous"))
        fixedRegion = ToggleChip(L("ocr.fixedRegion"), isOn: s.bool("ocr.fixedRegion"))
        thumbToggle = IconButton(icon: "image", size: 28, iconSize: 16)
        thumbToggle.tooltip = L("ocr.thumbnail")
        thumbToggle.selected = true
        super.init()
        header.add(keepOnTop)
        add(header)
        for c in [joinLines, asciiPunct, continuous, fixedRegion] { add(c) }
        add(thumb)
        add(textArea)
        for w in [thumbToggle, status, openLink, translate, copyClose, copy] as [Widget] { footer.add(w) }
        add(footer)
        thumb.topAligned = true
        openLink.isHidden = true
    }

    var hasThumb: Bool { thumb.image != nil }

    override func layout() {
        let f = frame
        let th = Theme.f("window.titleBarHeight")
        header.frame = CGRect(x: f.minX, y: f.minY, width: f.width, height: th)
        keepOnTop.frame = CGRect(x: f.maxX - 12 - 28, y: f.minY + (th - 28) / 2, width: 28, height: 28)
        var x = f.minX + 16
        let rowY = f.minY + th
        for c in [joinLines, asciiPunct, continuous, fixedRegion] {
            let s = c.preferredSize()
            c.frame = CGRect(x: x, y: rowY + (OCRContent.toolRowH - s.height) / 2, width: s.width, height: s.height)
            x += s.width + 6
        }
        let top = rowY + OCRContent.toolRowH
        let bottom = f.maxY - OCRContent.footerH
        footer.frame = CGRect(x: f.minX, y: bottom, width: f.width, height: OCRContent.footerH)
        let showThumb = thumbVisible && hasThumb
        thumb.isHidden = !showThumb
        var textX = f.minX + 16
        if showThumb {
            thumb.frame = CGRect(x: f.minX + 16, y: top, width: OCRContent.thumbW, height: bottom - 12 - top)
            textX = thumb.frame.maxX + 12
        }
        textArea.frame = CGRect(x: textX, y: top, width: f.maxX - 16 - textX, height: bottom - 12 - top)

        thumbToggle.isHidden = !hasThumb
        thumbToggle.selected = thumbVisible
        let left = footer.layoutRight([copy, copyClose, translate])
        var lx = f.minX + 12
        if hasThumb {
            thumbToggle.frame = CGRect(x: lx, y: footer.frame.midY - 14, width: 28, height: 28)
            lx += 28 + 8
        } else {
            lx += 4
        }
        if !openLink.isHidden {
            let s = openLink.preferredSize()
            let sw = min(status.preferredSize().width, max(0, left - 12 - lx - s.width - 8))
            status.frame = CGRect(x: lx, y: footer.frame.midY - 10, width: sw, height: 20)
            openLink.frame = CGRect(x: status.frame.maxX + 8, y: footer.frame.midY - s.height / 2, width: s.width, height: s.height)
        } else {
            status.frame = CGRect(x: lx, y: footer.frame.midY - 10, width: max(0, left - 12 - lx), height: 20)
        }
    }
}

/// 文字识别结果窗口：可调大小、可置顶；连续识别时新结果追加到末尾。
final class OCRWindowController: ToolWindowController {
    private(set) static var current: OCRWindowController?

    /// 一次识别的原始结果（开关变化时重新整理）。
    struct Block {
        var lines: [OCREngine.Line]
        var codes: [String]
    }

    let content: OCRContent
    let editor = NativeTextArea(editable: true)
    private var blocks: [Block] = []
    private var generation = 0
    private var link: URL?

    init() {
        content = OCRContent()
        super.init(size: OCRContent.size, minSize: CGSize(width: 520, height: 360), title: L("ocr.title"), root: content, resizable: true)
        host.addSubview(editor.scroll)
        placeNative = { [weak self] in
            guard let self = self else { return }
            self.editor.place(in: self.content.textArea.frame)
        }
        editor.onChange = { [weak self] _ in self?.content.textArea.overlay = nil }
        bind()
        applyKeepOnTop()
        NotificationCenter.default.addObserver(self, selector: #selector(settingsChanged(_:)), name: Settings.didChange, object: nil)
        onClose = { [weak self] in
            if OCRWindowController.current === self { OCRWindowController.current = nil }
        }
    }

    private func bind() {
        let c = content
        c.keepOnTop.action = { [weak self] in
            Settings.shared.set("ocr.keepOnTop", !Settings.shared.bool("ocr.keepOnTop"))
            self?.applyKeepOnTop()
        }
        c.joinLines.onChange = { Settings.shared.set("ocr.joinLines", $0) }
        c.asciiPunct.onChange = { Settings.shared.set("ocr.forceAsciiPunctuation", $0) }
        c.continuous.onChange = { Settings.shared.set("ocr.continuous", $0) }
        c.fixedRegion.onChange = { Settings.shared.set("ocr.fixedRegion", $0) }
        c.thumbToggle.action = { [weak self] in
            guard let self = self else { return }
            self.content.thumbVisible.toggle()
            self.relayout()
        }
        c.openLink.action = { [weak self] in
            if let u = self?.link { NSWorkspace.shared.open(u) }
        }
        c.copy.action = { [weak self] in self?.copyText(close: Settings.shared.bool("ocr.closeAfterCopy")) }
        c.copyClose.action = { [weak self] in self?.copyText(close: true) }
        c.translate.action = { [weak self] in
            guard let self = self else { return }
            TranslateFeature.translate(text: self.editor.string)
        }
    }

    override func themeChanged() { editor.applyTheme() }

    private func applyKeepOnTop() {
        let on = Settings.shared.bool("ocr.keepOnTop")
        content.keepOnTop.selected = on
        window.level = on ? .floating : .normal
    }

    @objc private func settingsChanged(_ n: Notification) {
        let key = n.userInfo?["key"] as? String ?? ""
        let s = Settings.shared
        guard key.isEmpty || key.hasPrefix("ocr") else { return }
        content.joinLines.isOn = s.bool("ocr.joinLines")
        content.asciiPunct.isOn = s.bool("ocr.forceAsciiPunctuation")
        content.continuous.isOn = s.bool("ocr.continuous")
        content.fixedRegion.isOn = s.bool("ocr.fixedRegion")
        applyKeepOnTop()
        if key.isEmpty || key == "ocr.joinLines" || key == "ocr.forceAsciiPunctuation" || key == "ocr.preferQRCode" {
            refreshText()
        }
        host.needsDisplay = true
    }

    // MARK: 识别

    /// 打开（或复用）窗口识别一张图。连续识别开启且窗口已有结果时追加，否则替换。
    static func recognize(image: CGImage, scale: CGFloat) {
        let existing = current
        let c = existing ?? OCRWindowController()
        current = c
        let append = existing != nil && c.window.isVisible && Settings.shared.bool("ocr.continuous") && !c.blocks.isEmpty
        c.begin(image: image, scale: scale, append: append)
        c.show(center: existing == nil)
    }

    private func begin(image: CGImage, scale: CGFloat, append: Bool) {
        generation += 1
        let gen = generation
        content.thumb.image = image
        if !append {
            blocks.removeAll()
            editor.string = ""
            link = nil
        }
        setStatus(L("ocr.recognizing"), icon: nil, danger: false)
        if editor.string.isEmpty { content.textArea.overlay = L("ocr.recognizing") }
        relayout()
        OCREngine.recognize(image) { [weak self] out, err in
            guard let self = self, gen == self.generation else { return }
            guard let out = out else {
                self.content.textArea.overlay = nil
                self.setStatus(L("ocr.failed", ["error": err ?? ""]), icon: "triangle-alert", danger: true)
                return
            }
            let b = Block(lines: out.lines, codes: out.codes)
            let piece = OCRWindowController.compose([b])
            if append && !self.editor.string.isEmpty {
                if !piece.isEmpty { self.editor.string = self.editor.string + "\n\n" + piece }
                self.blocks.append(b)
                self.editor.scrollToEnd()
            } else {
                self.blocks = [b]
                self.editor.string = piece
            }
            self.updateStatus()
        }
    }

    /// 一组结果的文本：优先二维码时只显示二维码内容；没有文字时退回二维码内容。
    static func compose(_ blocks: [Block]) -> String {
        let s = Settings.shared
        let join = s.bool("ocr.joinLines"), ascii = s.bool("ocr.forceAsciiPunctuation"), preferQR = s.bool("ocr.preferQRCode")
        var parts: [String] = []
        for b in blocks {
            var t: String
            if preferQR && !b.codes.isEmpty {
                t = b.codes.joined(separator: "\n")
            } else {
                t = OCREngine.layout(b.lines, joinLines: join, asciiPunct: ascii)
                if t.isEmpty { t = b.codes.joined(separator: "\n") }
            }
            if !t.isEmpty { parts.append(t) }
        }
        return parts.joined(separator: "\n\n")
    }

    private func refreshText() {
        guard !blocks.isEmpty else { return }
        editor.string = OCRWindowController.compose(blocks)
        updateStatus()
    }

    /// 底部状态：二维码（可打开链接）/ 没有识别到文字 / 空。
    private func updateStatus() {
        content.textArea.overlay = nil
        link = nil
        let showsCodes = blocks.last.map { b in !b.codes.isEmpty && (Settings.shared.bool("ocr.preferQRCode") || b.lines.isEmpty) } ?? false
        if showsCodes, let b = blocks.last {
            link = b.codes.count == 1 ? OCREngine.link(b.codes[0]) : nil
            setStatus(link == nil ? L("ocr.qrcode") : "", icon: "qr-code", danger: false)
        } else if editor.string.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            content.textArea.overlay = L("ocr.empty")
            setStatus("", icon: nil, danger: false)
        } else {
            setStatus("", icon: nil, danger: false)
        }
        content.openLink.isHidden = link == nil
        relayout()
    }

    private func setStatus(_ text: String, icon: String?, danger: Bool) {
        content.status.text = text
        content.status.icon = icon
        content.status.danger = danger
        content.openLink.isHidden = link == nil
        content.layout()
        host.needsDisplay = true
    }

    private func copyText(close: Bool) {
        let t = editor.string
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(t, forType: .string)
        if close {
            window.close()
        }
        Toast.show(L("common.copied"))
    }

    // MARK: 自测

    /// 自测：假结果画进一张图（与屏幕同一棵控件树，文本区直接画字）。
    static func render(scale: CGFloat, thumbnail: CGImage?, text: String) -> CGImage? {
        let size = OCRContent.size
        let c = OCRContent()
        c.frame = CGRect(origin: .zero, size: size)
        c.thumb.image = thumbnail
        c.textArea.drawsText = true
        c.textArea.text = text
        c.layout()
        return Offscreen.render(size: size, scale: scale) { ctx in
            Draw.fill(ctx, CGRect(origin: .zero, size: size), Theme.color("windowBg"))
            c.draw(ctx)
        }
    }
}
