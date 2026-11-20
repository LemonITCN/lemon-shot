import AppKit

/// 机型列表：一行一个，选中行 surfaceSelected 底 + 对勾；「自动匹配」行右侧显示匹配到的机型。
final class ShellModelList: Control {
    var selected: String { didSet { setNeedsDisplay() } }
    let autoModel: ShellModel
    var onChange: ((String) -> Void)?
    private var hover = -1
    static let rowH: CGFloat = 28

    var values: [String] { ["auto"] + ShellModel.allCases.map { $0.rawValue } }

    init(selected: String, autoModel: ShellModel) {
        self.selected = selected
        self.autoModel = autoModel
        super.init()
    }

    var contentHeight: CGFloat { CGFloat(values.count) * ShellModelList.rowH }

    private func rowRect(_ i: Int) -> CGRect {
        CGRect(x: frame.minX, y: frame.minY + CGFloat(i) * ShellModelList.rowH, width: frame.width, height: ShellModelList.rowH)
    }

    override func draw(_ ctx: CGContext) {
        let font = Fonts.ui(Fonts.body)
        for (i, v) in values.enumerated() {
            let r = rowRect(i).insetBy(dx: 0, dy: 1)
            let sel = v == selected
            if sel {
                Draw.fill(ctx, r, Theme.color("surfaceSelected"), radius: 6)
            } else if i == hover {
                Draw.fill(ctx, r, Theme.color("surfaceHover"), radius: 6)
            }
            let icon = v == "auto" ? "sparkles" : ShellModelList.icon(ShellModel(rawValue: v) ?? .laptop)
            Icons.draw(ctx, icon, in: CGRect(x: r.minX + 8, y: r.midY - 8, width: 16, height: 16), color: sel ? Theme.color("iconActive") : Theme.color("icon"), lineWidth: 1.6)
            let title = v == "auto" ? L("shell.auto") : L((ShellModel(rawValue: v) ?? .laptop).titleKey)
            let tx = r.minX + 32
            let tf = sel ? Fonts.medium(Fonts.body) : font
            let tw = Draw.textWidth(title, tf)
            Draw.text(ctx, title, font: tf, color: Theme.color("text"), in: CGRect(x: tx, y: r.minY, width: r.maxX - 8 - tx, height: r.height))
            if v == "auto" {
                let sub = L(autoModel.titleKey)
                Draw.text(ctx, sub, font: Fonts.ui(Fonts.caption), color: Theme.color("textTertiary"),
                          in: CGRect(x: tx + tw + 6, y: r.minY + 1, width: max(0, r.maxX - 8 - tx - tw - 6), height: r.height), align: .right)
            }
        }
    }

    static func icon(_ m: ShellModel) -> String {
        switch m {
        case .macbookAir, .macbookPro14, .macbookPro16, .laptop: return "laptop"
        case .imac24, .studioDisplay, .monitor: return "monitor"
        case .iphone: return "smartphone"
        case .ipad: return "tablet"
        }
    }

    override func mouseMoved(_ p: CGPoint) {
        let h = values.indices.first { rowRect($0).contains(p) } ?? -1
        if h != hover { hover = h; setNeedsDisplay() }
    }

    override func mouseExited() { hover = -1; super.mouseExited() }

    override func mouseUp(_ p: CGPoint, inside: Bool, event: NSEvent?) {
        guard inside, let i = values.indices.first(where: { rowRect($0).contains(p) }), values[i] != selected else { return }
        selected = values[i]
        onChange?(selected)
    }
}

/// 带壳预览窗口内容：左侧预览，右侧机型、背景、复制、保存。
final class ShellContent: Widget {
    let header = ToolHeader(title: L("shell.title"))
    let preview = ImageBoxW()
    let modelCaption = CaptionW(L("shell.model"))
    let list: ShellModelList
    let bgCaption = CaptionW(L("shell.background"))
    let background: SegmentedW
    let copy = ButtonW(L("action.copy"), kind: .primary)
    let save = ButtonW(L("action.save"))
    static let size = CGSize(width: 920, height: 600)
    static let sidebarW: CGFloat = 240
    static let backgrounds = ["transparent", "white", "black"]

    init(autoModel: ShellModel) {
        let s = Settings.shared
        var model = s.string("shell.model")
        if model != "auto" && ShellModel(rawValue: model) == nil { model = "auto" }
        list = ShellModelList(selected: model, autoModel: autoModel)
        let bg = ShellContent.backgrounds.firstIndex(of: s.string("shell.background")) ?? 0
        background = SegmentedW([L("shell.transparent"), L("shell.white"), L("shell.black")], selected: bg)
        super.init()
        preview.checker = true
        preview.border = false
        preview.radius = 0
        for w in [header, preview, modelCaption, list, bgCaption, background, copy, save] as [Widget] { add(w) }
    }

    var sidebar: CGRect { CGRect(x: frame.maxX - ShellContent.sidebarW, y: frame.minY + Theme.f("window.titleBarHeight"), width: ShellContent.sidebarW, height: frame.height - Theme.f("window.titleBarHeight")) }

    override func layout() {
        let f = frame
        let th = Theme.f("window.titleBarHeight")
        header.frame = CGRect(x: f.minX, y: f.minY, width: f.width, height: th)
        let sb = sidebar
        preview.frame = CGRect(x: f.minX + 24, y: f.minY + th + 24, width: sb.minX - 48 - f.minX, height: f.maxY - th - 48 - f.minY)
        let x = sb.minX + 16, w = sb.width - 32
        var y = sb.minY + 16
        modelCaption.frame = CGRect(x: x + 2, y: y, width: w, height: 20)
        y += 22
        list.frame = CGRect(x: x, y: y, width: w, height: list.contentHeight)
        y = list.frame.maxY + 18
        bgCaption.frame = CGRect(x: x + 2, y: y, width: w, height: 20)
        y += 24
        background.frame = CGRect(x: x, y: y, width: w, height: Theme.f("window.controlHeight"))
        let bh = Theme.f("window.controlHeight")
        let bw = floor((w - 8) / 2)
        save.frame = CGRect(x: x, y: f.maxY - 16 - bh, width: bw, height: bh)
        copy.frame = CGRect(x: x + bw + 8, y: f.maxY - 16 - bh, width: w - bw - 8, height: bh)
    }

    override func draw(_ ctx: CGContext) {
        let sb = sidebar
        Draw.fill(ctx, sb, Theme.color("surface"))
        Draw.fill(ctx, CGRect(x: sb.minX, y: sb.minY, width: 1, height: sb.height), Theme.color("divider"))
        drawChildren(ctx)
    }
}

/// 带壳预览窗口。换机型或背景时在后台重新合成。
final class ShellWindowController: ToolWindowController {
    private(set) static var current: ShellWindowController?
    let content: ShellContent
    private var shot: CGImage
    private var scale: CGFloat
    private var autoModel: ShellModel
    private var result: CGImage?
    private var generation = 0
    private static let queue = DispatchQueue(label: "cn.lemonit.shot.shell", qos: .userInitiated)

    static func show(image: CGImage, scale: CGFloat, auto: ShellModel) {
        current?.window.close()
        let c = ShellWindowController(image: image, scale: scale, auto: auto)
        current = c
        c.show(center: true)
        c.rerender()
    }

    init(image: CGImage, scale: CGFloat, auto: ShellModel) {
        shot = image
        self.scale = scale
        autoModel = auto
        content = ShellContent(autoModel: auto)
        super.init(size: ShellContent.size, minSize: CGSize(width: 760, height: 520), title: L("shell.title"), root: content, resizable: true)
        content.list.onChange = { [weak self] v in
            Settings.shared.set("shell.model", v)
            self?.rerender()
        }
        content.background.onChange = { [weak self] i in
            Settings.shared.set("shell.background", ShellContent.backgrounds[i])
            self?.rerender()
        }
        content.copy.action = { [weak self] in
            guard let self = self, let r = self.result else { return }
            Export.copy(Export.Result(image: r, scale: self.scale))
            Toast.show(L("notify.copied"))
        }
        content.save.action = { [weak self] in
            guard let self = self, let r = self.result, let url = Export.save(Export.Result(image: r, scale: self.scale)) else { return }
            Toast.show(L("notify.saved")) { NSWorkspace.shared.activateFileViewerSelecting([url]) }
        }
        onClose = { [weak self] in
            self?.generation += 1
            if ShellWindowController.current === self { ShellWindowController.current = nil }
        }
    }

    var model: ShellModel {
        let v = content.list.selected
        return v == "auto" ? autoModel : (ShellModel(rawValue: v) ?? autoModel)
    }

    func rerender() {
        generation += 1
        let gen = generation
        let shot = self.shot, model = self.model, bg = ShellContent.backgrounds[content.background.selectedIndex]
        ShellWindowController.queue.async { [weak self] in
            let img = ShellRenderer.render(shot, model: model, background: bg)
            DispatchQueue.main.async {
                guard let self = self, gen == self.generation else { return }
                self.result = img
                self.content.preview.image = img
                self.content.copy.isEnabled = img != nil
                self.content.save.isEnabled = img != nil
                self.host.needsDisplay = true
            }
        }
    }

    // MARK: 自测

    /// 自测（额外输出，不在契约状态表里）：一个机型的预览窗口。
    static func render(scale: CGFloat, shot: CGImage, model: String, background: String) -> CGImage? {
        let size = ShellContent.size
        let c = ShellContent(autoModel: .macbookPro14)
        c.list.selected = model
        c.background.selectedIndex = ShellContent.backgrounds.firstIndex(of: background) ?? 0
        c.frame = CGRect(origin: .zero, size: size)
        c.layout()
        let m = model == "auto" ? ShellModel.macbookPro14 : (ShellModel(rawValue: model) ?? .laptop)
        c.preview.image = ShellRenderer.render(shot, model: m, background: background)
        return Offscreen.render(size: size, scale: scale) { ctx in
            Draw.fill(ctx, CGRect(origin: .zero, size: size), Theme.color("windowBg"))
            c.draw(ctx)
        }
    }
}
