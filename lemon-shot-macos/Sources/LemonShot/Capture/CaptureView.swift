import AppKit
import LemonShotCore

protocol CaptureViewDelegate: AnyObject {
    /// 会话发出的动作或工具栏按钮对应的动作。
    func captureView(_ v: CaptureView, perform action: ls_action)
    /// 本屏开始拖出选区（其他屏要重置）。
    func captureViewDidBeginSelection(_ v: CaptureView)
    /// 鼠标进入本屏。
    func captureViewMouseEntered(_ v: CaptureView)
    /// 连按两下 Option。
    func captureViewDoubleModifier(_ v: CaptureView)
    /// 选区刚确定（拖完、单击窗口、F 全屏等）。全局文字识别/翻译/长截图据此自动开始。
    func captureViewDidFinishSelection(_ v: CaptureView)
}

extension CaptureViewDelegate {
    func captureViewDidFinishSelection(_ v: CaptureView) {}
}

/// 截图层（每屏一个）：冻结画面放在独立图层里不重画；遮罩/标注/选区只按脏区重画；
/// 十字线、尺寸提示、放大镜、左下角提示、工具栏、样式面板、提示气泡各是一个子视图，移动只改 frame。
final class CaptureView: NSView {
    enum Mode { case capture, colorPicker, pinAnnotate, record }

    let mode: Mode
    private(set) var session: Session
    weak var delegate: CaptureViewDelegate?
    let screenScale: CGFloat

    private let frozenView: NSView
    private var mainView: PaintView!
    private let crossH: PaintView
    private let crossV: PaintView
    private var sizeTipView: PaintView!
    private var magnifierView: PaintView!
    private var tipsView: PaintView!
    private var tooltipView: PaintView!
    private var toolbarHost: WidgetHost!
    private var toolbar: CaptureToolbar!
    /// 录屏模式：工具栏换成录屏设置面板（spec 8.1）。
    private var recordPanel: RecordSetupPanel?
    private var popoverHost: WidgetHost?
    private var popover: StylePopover?
    private var popoverIsPreview = false
    /// 上次排版时的当前工具（区分「换了工具」与「只改了样式」）。
    private var panelTool: Int32 = 0
    private var textView: AnnotTextView?
    private var sizeEditor: SizeEditor?

    private var lastState = ls_session_state()
    private var lastShapeSigs: [Int32: ShapeSig] = [:]
    private var lastChrome: [CGRect] = []
    private var trackingArea: NSTrackingArea?
    private var hoverTimer: Timer?
    private var tooltipText = ""
    private var sizeTipText = ""
    private var tipsLines: [String] = []
    private var pixelInfo: CaptureRender.PixelInfo?
    private(set) var mouse = CGPoint(x: -100, y: -100)
    private var mouseInView = false

    // 连按两下修饰键
    private var optionDownAt: TimeInterval = 0
    private var optionUpAt: TimeInterval = 0
    private var optionWasDown = false
    private var keyBetween = false

    private let flags = CaptureLayout.Flags()
    private var showTips: Bool { flags.tips }
    private let tipsColor = Theme.hex(Settings.shared.string("capture.tipsColor"))

    init(frame: CGRect, session: Session, mode: Mode, scale: CGFloat) {
        self.mode = mode
        self.session = session
        screenScale = scale
        frozenView = NSView(frame: CGRect(origin: .zero, size: frame.size))
        crossH = PaintView(frame: .zero) { ctx, r in ctx.setFillColor(Theme.cap("crosshair")); ctx.fill(r) }
        crossV = PaintView(frame: .zero) { ctx, r in ctx.setFillColor(Theme.cap("crosshair")); ctx.fill(r) }
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = CGColor.clear

        frozenView.wantsLayer = true
        frozenView.layerContentsRedrawPolicy = .never
        frozenView.layer?.contents = session.frozen.cgImage
        frozenView.layer?.contentsGravity = .resize
        frozenView.layer?.contentsScale = scale
        frozenView.layer?.magnificationFilter = .nearest
        addSubview(frozenView)

        mainView = PaintView(frame: bounds) { [weak self] ctx, dirty in self?.paintMain(ctx, dirty) }
        addSubview(mainView)
        crossH.isHidden = true
        crossV.isHidden = true
        addSubview(crossH)
        addSubview(crossV)

        sizeTipView = PaintView(frame: .zero) { [weak self] ctx, _ in
            guard let self = self else { return }
            CaptureRender.drawSizeTip(ctx, self.sizeTipView.bounds, self.sizeTipText)
        }
        sizeTipView.isHidden = true
        addSubview(sizeTipView)

        tipsView = PaintView(frame: .zero) { [weak self] ctx, _ in
            guard let self = self else { return }
            CaptureRender.drawTips(ctx, self.tipsView.bounds, self.tipsLines, color: self.tipsColor)
        }
        tipsView.isHidden = true
        addSubview(tipsView)

        magnifierView = PaintView(frame: .zero) { [weak self] ctx, _ in
            guard let self = self, let info = self.pixelInfo else { return }
            CaptureRender.drawMagnifier(ctx, self.magnifierView.bounds, frozen: self.session.frozen, mouse: self.mouse, info: info, showHint: self.showTips)
        }
        magnifierView.isHidden = true
        addSubview(magnifierView)

        toolbar = CaptureToolbar(mode: mode == .pinAnnotate ? .pin : .capture)
        let m = ChromeHost.margin
        var bar: Widget = toolbar
        if mode == .record {
            let p = RecordSetupPanel()
            p.onStart = { [weak self] in
                guard let self = self else { return }
                self.delegate?.captureView(self, perform: LS_ACTION_RECORD)
            }
            recordPanel = p
            bar = p
        }
        bar.frame.origin = CGPoint(x: m, y: m)
        bar.layout()
        toolbarHost = WidgetHost(frame: CGRect(x: 0, y: 0, width: bar.frame.width + 2 * m, height: bar.frame.height + 2 * m), root: bar)
        toolbarHost.tooltipDelay = mode == .record ? 0.5 : 1e9
        toolbarHost.takesFocus = false
        toolbarHost.isHidden = true
        toolbar.onItem = { [weak self] item in self?.toolbarItem(item) }
        toolbar.onHover = { [weak self] item, rect in self?.toolbarHover(item, rect) }
        addSubview(toolbarHost)

        tooltipView = PaintView(frame: .zero) { [weak self] ctx, _ in
            guard let self = self else { return }
            CaptureRender.drawTooltip(ctx, self.tooltipView.bounds, self.tooltipText)
        }
        tooltipView.isHidden = true
        addSubview(tooltipView)

        lastState = session.state
        sync(Int32(LS_EV_REDRAW | LS_EV_SELECTION | LS_EV_TOOL), force: true)
    }

    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    /// 截图层刚出现时按当前光标位置算一次悬停与放大镜。
    func syncInitialMouse(_ p: CGPoint) {
        mouse = p
        mouseInView = true
        sync(session.mouseMove(p, mods: Session.mods(NSEvent.modifierFlags)))
        updateCursor(session.state)
    }

    /// 其他屏开始框选时重置本屏：换一个全新的会话。
    func replaceSession(_ s: Session) {
        endTextEditing()
        session = s
        hidePopover()
        lastShapeSigs.removeAll()
        mainView.needsDisplay = true
        sync(Int32(LS_EV_REDRAW | LS_EV_SELECTION | LS_EV_TOOL), force: true)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = trackingArea { removeTrackingArea(t) }
        let t = NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways, .inVisibleRect], owner: self, userInfo: nil)
        addTrackingArea(t)
        trackingArea = t
    }

    // MARK: 绘制

    private func paintMain(_ ctx: CGContext, _ dirty: CGRect) {
        ctx.clear(dirty)
        if mode == .colorPicker { return }
        var opts = CaptureRender.Options()
        if mode == .pinAnnotate {
            opts.showMask = false
            opts.showSelectionBorder = false
        }
        CaptureRender.drawMain(ctx, session: session, state: session.state, bounds: bounds, dirty: dirty, options: opts)
    }

    private struct ShapeSig: Equatable {
        var bounds: CGRect
        var selected: Int32
        var hovered: Int32
        var editing: Int32
        var fillCount: Int32
        var strokeCount: Int32
        var p0: CGPoint, p1: CGPoint, ctrl: CGPoint
        var color: UInt32
        var width: Float, opacity: Float, variant: Int32, filled: Int32, radius: Float, font: Float, bold: Int32, block: Float, zoom: Float
        var textLen: Int
        var patch: UnsafeRawPointer?
        var patchRect: CGRect
    }

    private func shapeSigs() -> [Int32: ShapeSig] {
        var out: [Int32: ShapeSig] = [:]
        for i in 0..<session.shapeCount {
            let v = session.shape(i)
            let st = v.style
            let c = UInt32(st.color.r) << 24 | UInt32(st.color.g) << 16 | UInt32(st.color.b) << 8 | UInt32(st.color.a)
            let sig = ShapeSig(bounds: v.bounds.cg, selected: v.selected, hovered: v.hovered, editing: v.editing,
                               fillCount: v.fill.map { ls_path_count($0) } ?? -1, strokeCount: v.stroke.map { ls_path_count($0) } ?? -1,
                               p0: v.p0.cg, p1: v.p1.cg, ctrl: v.ctrl.cg, color: c, width: st.line_width, opacity: st.opacity, variant: st.variant,
                               filled: st.filled, radius: st.corner_radius, font: st.font_size, bold: st.bold, block: st.block_size, zoom: st.zoom,
                               textLen: v.text.map { strlen($0) } ?? -1, patch: v.patch.map { UnsafeRawPointer($0.pointee.data) } ?? nil, patchRect: v.patch_rect.cg)
            out[v.id] = sig
        }
        return out
    }

    /// 读取会话状态，刷新各子视图，并按变化局部重画主层。
    func sync(_ ev: Int32, force: Bool = false) {
        let st = session.state
        let old = lastState
        let hs = Theme.f("capture.handleSize") + 6

        // 主层脏区
        if force {
            mainView.needsDisplay = true
            lastShapeSigs = shapeSigs()
        } else {
            var dirty: [CGRect] = []
            let chromeChanged = st.phase != old.phase || st.has_selection != old.has_selection || st.selection.cg != old.selection.cg ||
                st.has_hover != old.has_hover || st.hover.cg != old.hover.cg || st.dragging_selection != old.dragging_selection ||
                st.dragging_shape != old.dragging_shape || st.selected_shape != old.selected_shape || st.window_count != old.window_count
            if chromeChanged {
                let newChrome = CaptureRender.dirtyRects(session: session, state: st)
                dirty += lastChrome + newChrome
                if (st.has_selection != old.has_selection) || st.phase != old.phase || (st.has_hover != old.has_hover) { dirty = [bounds] }
            }
            if ev & Int32(LS_EV_REDRAW | LS_EV_TOOL | LS_EV_HISTORY | LS_EV_SELECTION | LS_EV_TEXT_BEGIN | LS_EV_TEXT_END) != 0 || chromeChanged {
                let sigs = shapeSigs()
                for (id, s) in sigs where lastShapeSigs[id] != s {
                    dirty.append(s.bounds.insetBy(dx: -hs, dy: -hs))
                    if let o = lastShapeSigs[id] { dirty.append(o.bounds.insetBy(dx: -hs, dy: -hs)) }
                }
                for (id, o) in lastShapeSigs where sigs[id] == nil { dirty.append(o.bounds.insetBy(dx: -hs, dy: -hs)) }
                lastShapeSigs = sigs
            }
            for r in dirty where !r.isEmpty { mainView.setNeedsDisplay(r.intersection(bounds.insetBy(dx: -1, dy: -1))) }
        }
        lastChrome = CaptureRender.dirtyRects(session: session, state: st)
        lastState = st

        if ev & Int32(LS_EV_CURSOR) != 0 || force { updateCursor(st) }
        layoutChrome(st, ev: ev, force: force)
        if ev & Int32(LS_EV_TEXT_BEGIN) != 0 { beginTextEditing() }
        if ev & Int32(LS_EV_TEXT_END) != 0 { textView?.finish(notify: false) }
        if st.dragging_selection != 0 && old.dragging_selection == 0 { delegate?.captureViewDidBeginSelection(self) }
        let selectedPhase = Int32(LS_PHASE_SELECTED.rawValue)
        if !force, st.phase == selectedPhase, st.has_selection != 0, st.dragging_selection == 0,
           old.phase != selectedPhase || old.dragging_selection != 0 {
            delegate?.captureViewDidFinishSelection(self)
        }
        if ev & Int32(LS_EV_ACTION) != 0 {
            for a in session.takeActions() { delegate?.captureView(self, perform: a) }
        }
    }

    private func layoutChrome(_ st: ls_session_state, ev: Int32, force: Bool) {
        let l = CaptureLayout.compute(session: session, state: st, mouse: mouse, mouseInView: mouseInView, mode: mode, flags: flags,
                                      bounds: bounds, toolbarSize: recordPanel?.frame.size ?? toolbar.frame.size, editingSize: sizeEditor != nil)
        // 十字线
        crossH.isHidden = l.crosshair == nil
        crossV.isHidden = l.crosshair == nil
        if let p = l.crosshair {
            crossH.frame = CGRect(x: 0, y: floor(p.y), width: bounds.width, height: 1)
            crossV.frame = CGRect(x: floor(p.x), y: 0, width: 1, height: bounds.height)
        }
        // 放大镜
        magnifierView.isHidden = l.magnifier == nil
        if let r = l.magnifier {
            pixelInfo = l.pixelInfo
            magnifierView.frame = r
            magnifierView.needsDisplay = true
        }
        // 尺寸提示
        sizeTipView.isHidden = l.sizeTip == nil
        if let r = l.sizeTip {
            if l.sizeTipText != sizeTipText { sizeTipText = l.sizeTipText; sizeTipView.needsDisplay = true }
            sizeTipView.frame = r
        }
        // 左下角提示
        tipsView.isHidden = l.tips == nil
        if let r = l.tips {
            if l.tipsLines != tipsLines { tipsLines = l.tipsLines; tipsView.needsDisplay = true }
            tipsView.frame = r
        }
        // 工具栏
        if let r = l.toolbar {
            toolbar.update(st)
            let m = ChromeHost.margin
            toolbarHost.frame = CGRect(x: r.minX - m, y: r.minY - m, width: r.width + 2 * m, height: r.height + 2 * m)
            if toolbarHost.isHidden { toolbarHost.isHidden = false }
            toolbarHost.needsDisplay = true
            // 样式面板跟随当前工具。只是样式变了（面板里改、滚轮调）时不换面板，
            // 否则在别的工具的悬停面板里改样式会被换成当前工具的面板（spec 2.3：改样式不切换工具）
            if ev & Int32(LS_EV_TOOL) != 0 || force {
                let toolChanged = force || st.tool != panelTool
                panelTool = st.tool
                if toolChanged {
                    if st.tool != 0 {
                        if popover?.tool != st.tool || popoverIsPreview { showPopover(tool: st.tool, preview: false) } else { popover?.refresh() }
                    } else if !popoverIsPreview {
                        hidePopover()
                    }
                } else {
                    popover?.refresh()
                }
            }
            if let p = popover, let host = popoverHost { placePopover(p, host) }
        } else {
            if !toolbarHost.isHidden {
                toolbarHost.isHidden = true
                hideTooltip()
            }
            hidePopover()
        }
    }

    private var toolbarRect: CGRect { toolbarHost.frame.insetBy(dx: ChromeHost.margin, dy: ChromeHost.margin) }

    // MARK: 光标

    private func updateCursor(_ st: ls_session_state) {
        guard mouseInView, !toolbarHost.frame.contains(mouse) || toolbarHost.isHidden else { return }
        if mode == .colorPicker { NSCursor.crosshair.set(); return }
        CaptureView.cursor(st.cursor).set()
    }

    static func cursor(_ c: Int32) -> NSCursor {
        func priv(_ name: String) -> NSCursor? {
            let sel = NSSelectorFromString(name)
            guard NSCursor.responds(to: sel), let obj = NSCursor.perform(sel)?.takeUnretainedValue() as? NSCursor else { return nil }
            return obj
        }
        switch c {
        case Int32(LS_CURSOR_CROSSHAIR.rawValue): return .crosshair
        case Int32(LS_CURSOR_MOVE.rawValue): return priv("_moveCursor") ?? .openHand
        case Int32(LS_CURSOR_RESIZE_NS.rawValue): return priv("_windowResizeNorthSouthCursor") ?? .resizeUpDown
        case Int32(LS_CURSOR_RESIZE_EW.rawValue): return priv("_windowResizeEastWestCursor") ?? .resizeLeftRight
        case Int32(LS_CURSOR_RESIZE_NWSE.rawValue): return priv("_windowResizeNorthWestSouthEastCursor") ?? .crosshair
        case Int32(LS_CURSOR_RESIZE_NESW.rawValue): return priv("_windowResizeNorthEastSouthWestCursor") ?? .crosshair
        case Int32(LS_CURSOR_IBEAM.rawValue): return .iBeam
        case Int32(LS_CURSOR_POINTER.rawValue): return .pointingHand
        default: return .arrow
        }
    }

    // MARK: 鼠标

    private func loc(_ e: NSEvent) -> CGPoint { convert(e.locationInWindow, from: nil) }

    override func mouseEntered(with event: NSEvent) {
        mouseInView = true
        mouse = loc(event)
        delegate?.captureViewMouseEntered(self)
        sync(session.mouseMove(mouse, mods: Session.mods(event.modifierFlags)))
    }

    override func mouseExited(with event: NSEvent) {
        mouseInView = false
        sync(session.mouseExit())
    }

    override func mouseMoved(with event: NSEvent) {
        mouseInView = true
        mouse = loc(event)
        sync(session.mouseMove(mouse, mods: Session.mods(event.modifierFlags)))
    }

    override func mouseDown(with event: NSEvent) {
        keyBetween = true
        // 编辑文字时点别处只结束编辑
        if textView != nil { endTextEditing(); return }
        if let ed = sizeEditor { ed.commit(); return }
        mouse = loc(event)
        if mode == .colorPicker {
            delegate?.captureView(self, perform: LS_ACTION_COPY_HEX)
            delegate?.captureView(self, perform: LS_ACTION_CANCEL)
            return
        }
        let st = session.state
        if mode == .capture || mode == .record, !sizeTipView.isHidden, st.has_selection != 0, st.phase == Int32(LS_PHASE_SELECTED.rawValue),
           sizeTipView.frame.contains(mouse), event.clickCount == 1 {
            beginSizeEditing()
            return
        }
        if popoverIsPreview { hidePopover() }
        sync(session.mouseDown(mouse, button: Int32(LS_BUTTON_LEFT), mods: Session.mods(event.modifierFlags), clicks: event.clickCount))
    }

    override func mouseDragged(with event: NSEvent) {
        mouse = loc(event)
        mouseInView = true
        sync(session.mouseMove(mouse, mods: Session.mods(event.modifierFlags)))
    }

    override func mouseUp(with event: NSEvent) {
        mouse = loc(event)
        sync(session.mouseUp(mouse, button: Int32(LS_BUTTON_LEFT), mods: Session.mods(event.modifierFlags)))
    }

    override func rightMouseDown(with event: NSEvent) {
        keyBetween = true
        if textView != nil { endTextEditing() }
        sizeEditor?.cancel()
        if mode == .colorPicker { delegate?.captureView(self, perform: LS_ACTION_CANCEL); return }
        sync(session.mouseDown(loc(event), button: Int32(LS_BUTTON_RIGHT), mods: Session.mods(event.modifierFlags), clicks: event.clickCount))
    }

    override func rightMouseUp(with event: NSEvent) {
        sync(session.mouseUp(loc(event), button: Int32(LS_BUTTON_RIGHT), mods: Session.mods(event.modifierFlags)))
    }

    override func scrollWheel(with event: NSEvent) {
        guard mode != .colorPicker else { return }
        var dy = event.scrollingDeltaY
        if event.isDirectionInvertedFromDevice { dy = -dy }
        if event.hasPreciseScrollingDeltas { dy /= 10 }
        guard dy != 0 else { return }
        sync(session.scroll(loc(event), dy: dy, mods: Session.mods(event.modifierFlags)))
    }

    // MARK: 键盘

    override func keyDown(with event: NSEvent) {
        keyBetween = true
        if mode == .colorPicker {
            if event.keyCode == 53 { delegate?.captureView(self, perform: LS_ACTION_CANCEL) }
            return
        }
        guard let key = KeyMap.sessionKey(forKeyCode: event.keyCode) else { return }
        sync(session.keyDown(key, mods: Session.mods(event.modifierFlags)))
    }

    override func keyUp(with event: NSEvent) {
        guard mode != .colorPicker, let key = KeyMap.sessionKey(forKeyCode: event.keyCode) else { return }
        sync(session.keyUp(key, mods: Session.mods(event.modifierFlags)))
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard event.type == .keyDown, textView == nil, sizeEditor == nil, window?.firstResponder === self else { return super.performKeyEquivalent(with: event) }
        if event.modifierFlags.contains(.command) {
            keyDown(with: event)
            return true
        }
        return super.performKeyEquivalent(with: event)
    }

    override func flagsChanged(with event: NSEvent) {
        let flags = event.modifierFlags
        sync(session.modifiers(Session.mods(flags)))
        // 连按两下 Option：按下-松开-按下，间隔 < 350ms，中间无其他按键
        let optionDown = flags.contains(.option)
        let now = ProcessInfo.processInfo.systemUptime
        let others = flags.intersection([.command, .control, .shift])
        if optionDown && !optionWasDown {
            if !keyBetween && others.isEmpty && now - optionUpAt < 0.35 && optionUpAt - optionDownAt < 0.35 {
                optionDownAt = 0
                optionUpAt = 0
                delegate?.captureViewDoubleModifier(self)
            } else {
                optionDownAt = now
            }
            keyBetween = !others.isEmpty
        } else if !optionDown && optionWasDown {
            optionUpAt = now
        } else if !others.isEmpty {
            keyBetween = true
        }
        optionWasDown = optionDown
    }

    // MARK: 工具栏

    private func toolbarItem(_ item: ToolbarItem) {
        hideTooltip()
        switch item {
        case .tool(let t):
            sync(session.setTool(t))
            let st = session.state
            if st.tool == t { showPopover(tool: t, preview: false) } else { hidePopover() }
        case .undo: sync(session.undo())
        case .redo: sync(session.redo())
        case .longshot: delegate?.captureView(self, perform: LS_ACTION_LONGSHOT)
        case .record: delegate?.captureView(self, perform: LS_ACTION_RECORD)
        case .ocr: delegate?.captureView(self, perform: LS_ACTION_OCR)
        case .translate: delegate?.captureView(self, perform: LS_ACTION_TRANSLATE)
        case .pin: delegate?.captureView(self, perform: LS_ACTION_PIN)
        case .save: delegate?.captureView(self, perform: LS_ACTION_SAVE)
        case .cancel: delegate?.captureView(self, perform: LS_ACTION_CANCEL)
        case .done: delegate?.captureView(self, perform: LS_ACTION_DONE)
        }
    }

    private func toolbarHover(_ item: ToolbarItem?, _ rect: CGRect) {
        hoverTimer?.invalidate()
        hoverTimer = nil
        hideTooltip()
        guard let item = item else {
            if popoverIsPreview {
                hoverTimer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: false) { [weak self] _ in
                    guard let self = self, let host = self.popoverHost else { return }
                    let p = host.convert(self.window?.mouseLocationOutsideOfEventStream ?? .zero, from: nil)
                    if !host.bounds.contains(p) { self.restoreCurrentPopover() }
                }
            }
            return
        }
        let delay = Settings.shared.double("capture.toolbarPopupDelayMs") / 1000
        hoverTimer = Timer.scheduledTimer(withTimeInterval: max(0, delay), repeats: false) { [weak self] _ in
            guard let self = self else { return }
            if case .tool(let t) = item {
                guard self.toolbar.button(item)?.isEnabled == true else { return }
                if self.popover?.tool != t { self.showPopover(tool: t, preview: self.session.tool != t) }
            } else {
                self.showTooltip(item.tooltip, buttonRect: rect)
            }
        }
    }

    private func restoreCurrentPopover() {
        let t = session.tool
        if t != 0 { showPopover(tool: t, preview: false) } else { hidePopover() }
    }

    private func showTooltip(_ text: String, buttonRect: CGRect) {
        tooltipText = text
        let br = toolbarHost.convert(buttonRect, to: self)
        tooltipView.frame = CaptureLayout.tooltip(size: CaptureRender.tooltipSize(text), toolbar: toolbarRect, button: br,
                                                  selection: session.state.selection.cg, bounds: bounds, scale: screenScale)
        tooltipView.isHidden = false
        tooltipView.needsDisplay = true
    }

    private func hideTooltip() { tooltipView.isHidden = true }

    // MARK: 样式面板

    func showPopover(tool: Int32, preview: Bool) {
        hidePopover()
        let p = StylePopover(tool: tool, get: { [weak self] in self?.session.style(tool) ?? ls_style() }, set: { [weak self] st in
            guard let self = self else { return }
            self.sync(self.session.setStyle(tool, st))
            AnnotStyles.save(st, for: tool)
        })
        let m = ChromeHost.margin
        p.frame.origin = CGPoint(x: m, y: m)
        p.layout()
        let host = WidgetHost(frame: CGRect(x: 0, y: 0, width: p.frame.width + 2 * m, height: p.frame.height + 2 * m), root: p)
        host.tooltipDelay = 0.6
        host.takesFocus = false
        popover = p
        popoverHost = host
        popoverIsPreview = preview
        p.onResize = { [weak self, weak p, weak host] in
            guard let self = self, let p = p, let host = host else { return }
            let m = ChromeHost.margin
            p.frame.origin = CGPoint(x: m, y: m)
            p.layout()
            self.placePopover(p, host)
            host.needsDisplay = true
        }
        addSubview(host, positioned: .below, relativeTo: tooltipView)
        placePopover(p, host)
    }

    private func placePopover(_ p: StylePopover, _ host: WidgetHost) {
        guard let b = toolbar.button(.tool(p.tool)) else { return }
        let anchor = toolbarHost.convert(b.frame, to: self).midX
        let r = CaptureLayout.popover(session: session, toolbar: toolbarRect, anchorX: anchor, size: p.frame.size)
        let m = ChromeHost.margin
        host.frame = CGRect(x: r.minX - m, y: r.minY - m, width: r.width + 2 * m, height: r.height + 2 * m)
    }

    func hidePopover() {
        popoverHost?.hideTooltip()
        popoverHost?.removeFromSuperview()
        popoverHost = nil
        popover = nil
        popoverIsPreview = false
    }

    // MARK: 文字输入（spec 2.6）

    private func beginTextEditing() {
        guard let te = session.textEdit() else { return }
        // 编辑中改样式会再发一次 TEXT_BEGIN：更新已打开的输入框，不再开新的
        if let tv = textView { tv.apply(te); return }
        let tv = AnnotTextView(edit: te)
        tv.onChange = { [weak self] s in
            guard let self = self else { return }
            self.sync(self.session.setText(s))
            if let t = self.session.textEdit() { self.textView?.apply(t) }
        }
        tv.onEnd = { [weak self] notify in
            guard let self = self else { return }
            self.textView?.removeFromSuperview()
            self.textView = nil
            self.window?.makeFirstResponder(self)
            if notify { self.sync(self.session.endText()) }
        }
        addSubview(tv, positioned: .below, relativeTo: toolbarHost)
        textView = tv
        window?.makeFirstResponder(tv)
    }

    func endTextEditing() { textView?.finish(notify: true) }

    // MARK: 尺寸提示改宽高

    private func beginSizeEditing() {
        let px = session.selectionPixels
        let ed = SizeEditor(frame: sizeTipView.frame.insetBy(dx: 0, dy: 0), width: Int(px.width), height: Int(px.height))
        ed.onCommit = { [weak self] w, h in
            guard let self = self else { return }
            self.sizeEditor = nil
            ed.removeFromSuperview()
            self.window?.makeFirstResponder(self)
            let sel = self.session.state.selection.cg
            let r = CGRect(x: sel.minX, y: sel.minY, width: CGFloat(max(1, w)) / self.session.scale, height: CGFloat(max(1, h)) / self.session.scale)
            self.sync(self.session.setSelection(r))
        }
        ed.onCancel = { [weak self] in
            guard let self = self else { return }
            self.sizeEditor = nil
            ed.removeFromSuperview()
            self.window?.makeFirstResponder(self)
            self.sync(0, force: true)
        }
        sizeTipView.isHidden = true
        addSubview(ed, positioned: .below, relativeTo: toolbarHost)
        sizeEditor = ed
        ed.focus()
    }
}

/// 标注文字的原生输入框：透明背景、无边框、字体字号颜色与标注一致，支持输入法。
final class AnnotTextView: NSTextView, NSTextViewDelegate {
    var onChange: ((String) -> Void)?
    var onEnd: ((Bool) -> Void)?
    private var ended = false

    init(edit te: ls_text_edit) {
        super.init(frame: te.rect.cg, textContainer: nil)
        delegate = self
        drawsBackground = false
        isRichText = false
        allowsUndo = true
        isAutomaticQuoteSubstitutionEnabled = false
        isAutomaticDashSubstitutionEnabled = false
        isAutomaticTextReplacementEnabled = false
        isContinuousSpellCheckingEnabled = false
        textContainerInset = .zero
        textContainer?.lineFragmentPadding = 0
        textContainer?.widthTracksTextView = false
        textContainer?.containerSize = CGSize(width: 1e6, height: 1e6)
        isHorizontallyResizable = true
        isVerticallyResizable = true
        string = te.text.map { String(cString: $0) } ?? ""
        apply(te)
    }

    required init?(coder: NSCoder) { fatalError() }

    override init(frame frameRect: NSRect, textContainer container: NSTextContainer?) {
        super.init(frame: frameRect, textContainer: container)
    }

    private var styleKey = ""

    func apply(_ te: ls_text_edit) {
        let font = AnnotText.font(CGFloat(te.font_size), bold: te.bold != 0)
        let lh = AnnotText.lineHeight(font)
        let r = te.rect.cg
        frame = CGRect(x: r.minX, y: r.minY, width: max(r.width, lh) + 4, height: max(r.height, lh))
        // 只有样式变了才重设属性：输入法组字时改属性会打断组字
        let key = "\(te.font_size)-\(te.bold)-\(Theme.hexString(te.color, alpha: true))-\(Theme.hexString(te.outline, alpha: true))"
        guard key != styleKey else { return }
        styleKey = key
        let ps = NSMutableParagraphStyle()
        ps.minimumLineHeight = lh
        ps.maximumLineHeight = lh
        var attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: NSColor(cgColor: Theme.cg(te.color)) ?? .red, .paragraphStyle: ps]
        if te.outline.a > 0 {
            attrs[.strokeColor] = NSColor(cgColor: Theme.cg(te.outline))
            attrs[.strokeWidth] = -12.5
        }
        typingAttributes = attrs
        if let ts = textStorage, ts.length > 0 { ts.setAttributes(attrs, range: NSRange(location: 0, length: ts.length)) }
        insertionPointColor = NSColor(cgColor: Theme.cg(te.color)) ?? .red
    }

    func textDidChange(_ notification: Notification) { onChange?(string) }

    override func keyDown(with event: NSEvent) {
        if event.keyCode == 53 { finish(notify: true); return }
        if (event.keyCode == 36 || event.keyCode == 76) && event.modifierFlags.contains(.command) { finish(notify: true); return }
        super.keyDown(with: event)
    }

    override func resignFirstResponder() -> Bool {
        let ok = super.resignFirstResponder()
        if ok && !ended { DispatchQueue.main.async { [weak self] in self?.finish(notify: true) } }
        return ok
    }

    func finish(notify: Bool) {
        guard !ended else { return }
        ended = true
        onEnd?(notify)
    }
}

/// 尺寸提示单击后的宽、高输入框。
final class SizeEditor: NSView, NSTextFieldDelegate {
    var onCommit: ((Int, Int) -> Void)?
    var onCancel: (() -> Void)?
    private let wField = NSTextField()
    private let hField = NSTextField()
    private var done = false

    init(frame: CGRect, width: Int, height: Int) {
        let font = CaptureRender.tipFont
        let labelW = max(Draw.textWidth(L("capture.width"), font), Draw.textWidth(L("capture.height"), font)) + 4
        let fieldW = Draw.textWidth("00000", font) + 6
        let total = Theme.f("capture.tipPaddingX") * 2 + 2 * (labelW + fieldW) + 10
        super.init(frame: CGRect(x: frame.minX, y: frame.minY, width: total, height: Theme.f("capture.tipHeight")))
        wantsLayer = true
        var x = Theme.f("capture.tipPaddingX")
        for (f, v, key) in [(wField, width, "capture.width"), (hField, height, "capture.height")] {
            let l = NSTextField(labelWithString: L(key))
            l.font = font
            l.textColor = NSColor(cgColor: Theme.cap("tipText"))?.withAlphaComponent(0.7)
            l.frame = CGRect(x: x, y: (bounds.height - 16) / 2, width: labelW, height: 16)
            addSubview(l)
            x += labelW
            f.stringValue = String(v)
            f.font = font
            f.isBordered = false
            f.drawsBackground = true
            f.backgroundColor = NSColor(white: 1, alpha: 0.15)
            f.textColor = .white
            f.focusRingType = .none
            f.alignment = .center
            f.delegate = self
            f.frame = CGRect(x: x, y: (bounds.height - 18) / 2, width: fieldW, height: 18)
            addSubview(f)
            x += fieldW + 10
        }
    }

    required init?(coder: NSCoder) { fatalError() }
    override var isFlipped: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        Draw.fill(ctx, bounds, Theme.cap("tipBg"), radius: Theme.f("capture.tipRadius"))
    }

    func focus() {
        window?.makeFirstResponder(wField)
        wField.currentEditor()?.selectAll(nil)
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy sel: Selector) -> Bool {
        if sel == #selector(NSResponder.insertNewline(_:)) { commit(); return true }
        if sel == #selector(NSResponder.cancelOperation(_:)) { cancel(); return true }
        if sel == #selector(NSResponder.insertTab(_:)) {
            window?.makeFirstResponder(control === wField ? hField : wField)
            return true
        }
        return false
    }

    func commit() {
        guard !done else { return }
        done = true
        onCommit?(Int(wField.stringValue.trimmingCharacters(in: .whitespaces)) ?? 1, Int(hField.stringValue.trimmingCharacters(in: .whitespaces)) ?? 1)
    }

    func cancel() {
        guard !done else { return }
        done = true
        onCancel?()
    }
}
