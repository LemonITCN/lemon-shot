import AppKit
import LemonShotCore

/// 工具栏按钮（spec 2.3）。
enum ToolbarItem: Equatable {
    case tool(Int32)
    case undo, redo, longshot, record, ocr, translate, pin, save, cancel, done

    var icon: String {
        switch self {
        case .tool(let t): return AnnotStyles.toolIcons[Int(t)]
        case .undo: return "undo-2"
        case .redo: return "redo-2"
        case .longshot: return "scroll-text"
        case .record: return "video"
        case .ocr: return "scan-text"
        case .translate: return "languages"
        case .pin: return "pin"
        case .save: return "download"
        case .cancel: return "x"
        case .done: return "check"
        }
    }

    var name: String {
        switch self {
        case .tool(let t): return L("tool." + AnnotStyles.key(t))
        case .undo: return L("action.undo")
        case .redo: return L("action.redo")
        case .longshot: return L("action.longshot")
        case .record: return L("action.record")
        case .ocr: return L("action.ocr")
        case .translate: return L("action.translate")
        case .pin: return L("action.pin")
        case .save: return L("action.save")
        case .cancel: return L("action.cancel")
        case .done: return L("action.done")
        }
    }

    var shortcut: String {
        let toolIDs = ["", "toolRect", "toolEllipse", "toolLine", "toolArrow", "toolPen", "toolMosaic", "toolText", "toolStep", "toolHighlight", "toolMagnify"]
        switch self {
        case .tool(let t): return Shortcut.display(ShortcutTable.capture(toolIDs[Int(t)]))
        case .undo: return "⌘Z"
        case .redo: return "⇧⌘Z"
        case .longshot: return Shortcut.display(ShortcutTable.capture("longshot"))
        case .record: return Shortcut.display(ShortcutTable.capture("record"))
        case .ocr: return Shortcut.display(ShortcutTable.capture("ocr"))
        case .translate: return Shortcut.display(ShortcutTable.capture("translate"))
        case .pin: return Shortcut.display(ShortcutTable.capture("pin"))
        case .save: return "⌘S"
        case .cancel: return "⎋"
        case .done: return "↩"
        }
    }

    var tooltip: String {
        let k = shortcut
        return k.isEmpty ? name : name + "　" + k
    }

    var isTool: Bool { if case .tool = self { return true }; return false }
}

final class CaptureToolbar: Widget {
    enum Mode { case capture, pin }
    let mode: Mode
    private(set) var groups: [[ToolbarItem]]
    private(set) var buttons: [(ToolbarItem, IconButton)] = []
    private var separatorXs: [CGFloat] = []
    var onItem: ((ToolbarItem) -> Void)?
    var onHover: ((ToolbarItem?, CGRect) -> Void)?

    static var height: CGFloat { Theme.f("toolbar.height") }

    init(mode: Mode) {
        self.mode = mode
        let tools = (1...10).map { ToolbarItem.tool(Int32($0)) }
        switch mode {
        case .capture: groups = [tools, [.undo, .redo], [.longshot, .record, .ocr, .translate, .pin], [.save, .cancel, .done]]
        case .pin: groups = [tools, [.undo, .redo], [.cancel, .done]]
        }
        super.init()
        let bs = Theme.f("toolbar.button")
        for g in groups {
            for item in g {
                let b = IconButton(icon: item.icon, size: bs, iconSize: Theme.f("toolbar.iconSize"))
                if item == .done { b.iconColor = Theme.color("accent") }
                b.action = { [weak self] in self?.onItem?(item) }
                b.onHoverChange = { [weak self, weak b] inside in
                    guard let self = self, let b = b else { return }
                    self.onHover?(inside ? item : nil, b.frame)
                }
                add(b)
                buttons.append((item, b))
            }
        }
        frame.size = CGCaptureToolbarSize(groups)
    }

    static func size(mode: Mode) -> CGSize { CaptureToolbar(mode: mode).frame.size }

    override func layout() {
        let bs = Theme.f("toolbar.button")
        let sp = Theme.f("toolbar.spacing")
        let sepM = Theme.f("toolbar.separatorMargin")
        let sepW = Theme.f("toolbar.separatorWidth")
        var x = frame.minX + Theme.f("toolbar.paddingX")
        let y = frame.midY - bs / 2
        var idx = 0
        separatorXs.removeAll()
        for (gi, g) in groups.enumerated() {
            if gi > 0 {
                x += sepM
                separatorXs.append(x)
                x += sepW + sepM
            }
            for (bi, _) in g.enumerated() {
                if bi > 0 { x += sp }
                buttons[idx].1.frame = CGRect(x: x, y: y, width: bs, height: bs)
                x += bs
                idx += 1
            }
        }
    }

    func button(_ item: ToolbarItem) -> IconButton? { buttons.first { $0.0 == item }?.1 }

    /// 按会话状态刷新：当前工具、撤销/重做可用、多选窗口时禁用标注与长截图/录屏。
    func update(_ st: ls_session_state) {
        let multi = st.window_count > 0
        for (item, b) in buttons {
            switch item {
            case .tool(let t):
                b.selected = st.tool == t
                b.isEnabled = !multi
            case .undo: b.isEnabled = st.can_undo != 0
            case .redo: b.isEnabled = st.can_redo != 0
            case .longshot, .record: b.isEnabled = !multi
            default: break
            }
        }
    }

    override func draw(_ ctx: CGContext) {
        Draw.panel(ctx, frame, radius: Theme.f("toolbar.radius"), fill: Theme.color("toolbarBg"), border: Theme.color("toolbarBorder"),
                   shadow: Theme.color("shadow"), blur: Theme.f("toolbar.shadowBlur"), offsetY: Theme.f("toolbar.shadowOffsetY"))
        let sh = Theme.f("toolbar.separatorHeight")
        for x in separatorXs {
            Draw.fill(ctx, CGRect(x: x, y: frame.midY - sh / 2, width: Theme.f("toolbar.separatorWidth"), height: sh), Theme.color("divider"))
        }
        drawChildren(ctx)
    }

    override func hitTest(_ p: CGPoint) -> Widget? {
        guard frame.contains(p) else { return nil }
        for (_, b) in buttons where b.frame.contains(p) { return b }
        return self
    }

    override var isInteractive: Bool { true }
}

private func CGCaptureToolbarSize(_ groups: [[ToolbarItem]]) -> CGSize {
    let bs = Theme.f("toolbar.button")
    let sp = Theme.f("toolbar.spacing")
    let sep = 2 * Theme.f("toolbar.separatorMargin") + Theme.f("toolbar.separatorWidth")
    var w = 2 * Theme.f("toolbar.paddingX")
    for (gi, g) in groups.enumerated() {
        if gi > 0 { w += sep }
        w += CGFloat(g.count) * bs + CGFloat(max(0, g.count - 1)) * sp
    }
    return CGSize(width: ceil(w), height: Theme.f("toolbar.height"))
}

/// 承载工具栏/样式面板的视图比面板本身大一圈，留出投影边距。
enum ChromeHost {
    static let margin: CGFloat = 24
}
