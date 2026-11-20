import AppKit
import LemonShotCore

/// 截图层窗口：无边框、置顶、所有空间可用、全屏应用之上。
final class CapturePanel: NSPanel {
    init(frame: CGRect, opaque: Bool) {
        super.init(contentRect: frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        level = .screenSaver
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        isOpaque = opaque
        backgroundColor = opaque ? .black : .clear
        hasShadow = false
        acceptsMouseMovedEvents = true
        isReleasedWhenClosed = false
        hidesOnDeactivate = false
        animationBehavior = .none
        isMovable = false
    }

    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { false }
}

/// 截图流程（spec 2.1、2.7、2.9）：每屏冻结画面 + 截图层 + 会话，执行动作，记录上次区域。
final class CaptureController: CaptureViewDelegate {
    static let shared = CaptureController()

    enum Purpose { case normal, colorPicker, ocr, translate, longshot, record }
    /// region：录屏模式沿用的选区（Cocoa 全局坐标）。
    enum Initial { case none, lastArea, preset, region(CGRect) }

    final class Screen {
        let screen: NSScreen
        let displayID: CGDirectDisplayID
        let key: String
        let frozen: FrozenImage
        let windows: [ls_window]
        let panel: CapturePanel
        var view: CaptureView!
        init(screen: NSScreen, displayID: CGDirectDisplayID, key: String, frozen: FrozenImage, windows: [ls_window], panel: CapturePanel) {
            self.screen = screen
            self.displayID = displayID
            self.key = key
            self.frozen = frozen
            self.windows = windows
            self.panel = panel
        }
    }

    private(set) var screens: [Screen] = []
    private var starting = false
    private var purpose: Purpose = .normal
    private var previousApp: NSRunningApplication?
    private var globalWindows: [ScreenCapture.WindowInfo] = []

    var isActive: Bool { starting || !screens.isEmpty }

    // MARK: 进入

    static func ensurePermission() -> Bool {
        if ScreenCapture.hasPermission { return true }
        Toast.show(L("notify.needScreenPermission"), icon: "triangle-alert")
        PrefsWindowController.show(page: "permissions")
        return false
    }

    func start(_ purpose: Purpose = .normal, initial: Initial = .none) {
        guard !isActive, CaptureController.ensurePermission() else { return }
        starting = true
        self.purpose = purpose
        Toast.dismiss(animated: false)
        PopupPanel.closeAll()
        let front = NSWorkspace.shared.frontmostApplication
        previousApp = front?.processIdentifier == ProcessInfo.processInfo.processIdentifier ? nil : front
        let list = NSScreen.screens
        globalWindows = ScreenCapture.windowList()
        let wins = globalWindows
        ScreenCapture.captureScreens(list, cursor: Settings.shared.bool("capture.captureCursor")) { [weak self] images in
            guard let self = self else { return }
            self.starting = false
            self.build(list, images: images, windows: wins, initial: initial)
        }
    }

    private func targetScreenForInitial(_ initial: Initial, _ list: [NSScreen]) -> (NSScreen, CGRect)? {
        let mouse = NSEvent.mouseLocation
        let cursorScreen = ScreenCapture.screen(at: mouse) ?? list.first
        switch initial {
        case .none: return nil
        case .lastArea:
            if let s = cursorScreen, let r = AppState.shared.lastArea(display: ScreenCapture.displayKey(ScreenCapture.displayID(s))) { return (s, r) }
            if let key = AppState.shared.value("lastAreaDisplay")?.string, let s = list.first(where: { ScreenCapture.displayKey(ScreenCapture.displayID($0)) == key }),
               let r = AppState.shared.lastArea(display: key) { return (s, r) }
            return nil
        case .preset:
            guard let s = cursorScreen else { return nil }
            let scale = s.backingScaleFactor
            let w = min(s.frame.width, CGFloat(max(1, Settings.shared.int("capture.presetWidth"))) / scale)
            let h = min(s.frame.height, CGFloat(max(1, Settings.shared.int("capture.presetHeight"))) / scale)
            let lx = mouse.x - s.frame.minX, ly = s.frame.maxY - mouse.y
            var r = CGRect(x: lx - w / 2, y: ly - h / 2, width: w, height: h)
            r.origin.x = max(0, min(r.minX, s.frame.width - w))
            r.origin.y = max(0, min(r.minY, s.frame.height - h))
            return (s, r.pixelAligned(scale))
        case .region(let g):
            guard let s = list.first(where: { $0.frame.contains(CGPoint(x: g.midX, y: g.midY)) }) ?? cursorScreen else { return nil }
            let r = CGRect(x: g.minX - s.frame.minX, y: s.frame.maxY - g.maxY, width: g.width, height: g.height)
            return (s, r.pixelAligned(s.backingScaleFactor))
        }
    }

    private func makeSession(frozen: FrozenImage, screen: NSScreen, windows: [ls_window], key: String, initial: CGRect?) -> Session? {
        var opts = Session.Options()
        opts.initialSelection = initial
        if purpose == .colorPicker { opts.detectWindows = false }
        guard let s = Session(frozen: frozen, size: screen.frame.size, options: opts) else { return nil }
        if purpose == .record { RecordUI.configure(s) }
        s.setWindows(windows)
        if let la = AppState.shared.lastArea(display: key) { s.setLastArea(la) }
        return s
    }

    private func build(_ list: [NSScreen], images: [CGDirectDisplayID: CGImage], windows: [ScreenCapture.WindowInfo], initial: Initial) {
        let target = targetScreenForInitial(initial, list)
        for s in list {
            let id = ScreenCapture.displayID(s)
            guard let cg = images[id] else { continue }
            let scale = CGFloat(cg.width) / s.frame.width
            guard let fz = FrozenImage.from(cg, scale: scale) else { continue }
            let cgb = CGDisplayBounds(id)
            let local: [ls_window] = windows.compactMap { w in
                guard w.frame.intersects(cgb) else { return nil }
                let r = CGRect(x: w.frame.minX - cgb.minX, y: w.frame.minY - cgb.minY, width: w.frame.width, height: w.frame.height)
                return ls_window(id: Int64(w.id), rect: r.ls)
            }
            let key = ScreenCapture.displayKey(id)
            let initialRect = target?.0 == s ? target?.1 : nil
            guard let session = makeSession(frozen: fz, screen: s, windows: local, key: key, initial: initialRect) else { continue }
            let panel = CapturePanel(frame: s.frame, opaque: true)
            let entry = Screen(screen: s, displayID: id, key: key, frozen: fz, windows: local, panel: panel)
            let mode: CaptureView.Mode = purpose == .colorPicker ? .colorPicker : (purpose == .record ? .record : .capture)
            let view = CaptureView(frame: CGRect(origin: .zero, size: s.frame.size), session: session, mode: mode, scale: scale)
            view.delegate = self
            panel.contentView = view
            entry.view = view
            panel.setFrame(s.frame, display: false)
            screens.append(entry)
        }
        guard !screens.isEmpty else { return }
        NSApp.activate(ignoringOtherApps: true)
        for e in screens { e.panel.orderFrontRegardless() }
        let mouse = NSEvent.mouseLocation
        let keyEntry = screens.first { target != nil && $0.screen == target!.0 } ?? screens.first { NSMouseInRect(mouse, $0.screen.frame, false) } ?? screens[0]
        keyEntry.panel.makeKey()
        keyEntry.panel.makeFirstResponder(keyEntry.view)
        // 立即按当前光标位置算一次悬停与放大镜
        if let e = screens.first(where: { NSMouseInRect(mouse, $0.screen.frame, false) }) {
            let p = CGPoint(x: mouse.x - e.screen.frame.minX, y: e.screen.frame.maxY - mouse.y)
            e.view.syncInitialMouse(p)
        }
    }

    // MARK: 结束

    private func close() {
        let old = screens
        screens.removeAll()
        for e in old {
            e.view.endTextEditing()
            e.view.hidePopover()
            e.panel.orderOut(nil)
        }
        // 关闭往往发生在截图层自己的事件处理里：等这次事件处理完再释放视图和会话
        DispatchQueue.main.async {
            for e in old {
                e.panel.contentView = nil
                e.panel.close()
            }
        }
        ColorPanelBridge.shared.detach()
        NSCursor.arrow.set()
    }

    private func restorePreviousApp() {
        if Settings.shared.bool("capture.returnToPreviousApp"), let app = previousApp, !app.isTerminated {
            app.activate(options: [])
        }
        previousApp = nil
    }

    func cancel() {
        close()
        restorePreviousApp()
    }

    private func screen(of view: CaptureView) -> Screen? { screens.first { $0.view === view } }

    private func recordLastArea(_ e: Screen) {
        let st = e.view.session.state
        guard st.has_selection != 0 else { return }
        AppState.shared.setLastArea(st.selection.cg, display: e.key)
    }

    /// 选区在屏幕上的位置（Cocoa 全局坐标）。
    private func globalRect(_ r: CGRect, on e: Screen) -> CGRect {
        CGRect(x: e.screen.frame.minX + r.minX, y: e.screen.frame.maxY - r.maxY, width: r.width, height: r.height)
    }

    private func output(_ kind: Export.Output, from view: CaptureView) {
        guard let e = screen(of: view) else { return }
        let st = view.session.state
        guard st.has_selection != 0 else { return }
        recordLastArea(e)
        if st.window_count > 0 {
            let ids = view.session.selectedWindows()
            let infos = ids.compactMap { id in globalWindows.first { Int64($0.id) == id } }
            let scale = view.session.scale
            close()
            CaptureModes.composeWindows(infos, scale: scale) { [weak self] result in
                if let r = result { Export.output(r, kind, pinFrame: nil) }
                self?.restorePreviousApp()
            }
            return
        }
        guard let r = Export.render(session: view.session) else { cancel(); return }
        let frame = globalRect(st.selection.cg, on: e)
        close()
        DispatchQueue.main.async { [weak self] in
            Export.output(r, kind, pinFrame: frame)
            self?.restorePreviousApp()
        }
    }

    private func copyColor(_ action: ls_action, from view: CaptureView) {
        let c = ColorValue.converted(view.session.pixel(at: view.mouse), frozen: view.session.frozen)
        let value: String
        switch action {
        case LS_ACTION_COPY_RGB: value = ColorValue.rgb(c)
        case LS_ACTION_COPY_CUSTOM: value = ColorValue.custom(c)
        default: value = ColorValue.hex(c)
        }
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(value, forType: .string)
        Toast.show(L("capture.copiedColor", ["value": value]), icon: "pipette")
    }

    // MARK: CaptureViewDelegate

    func captureView(_ v: CaptureView, perform action: ls_action) {
        let s = Settings.shared
        if purpose == .record {
            // 录屏模式：回车、V、双击、「开始录制」都开始录制，其余动作无效
            switch action {
            case LS_ACTION_CANCEL: cancel()
            case LS_ACTION_DONE, LS_ACTION_RECORD, LS_ACTION_DOUBLE_CLICK: handOff(LS_ACTION_RECORD, from: v)
            default: break
            }
            return
        }
        switch action {
        case LS_ACTION_CANCEL: cancel()
        case LS_ACTION_DONE:
            if let a = purposeAction { handOff(a, from: v); return }
            output(Export.output(kindFor: s.string("save.enterAction")) ?? .copy, from: v)
        case LS_ACTION_DOUBLE_CLICK:
            if let k = Export.output(kindFor: s.string("capture.doubleClickAction")) { output(k, from: v) }
        case LS_ACTION_COPY: output(.copy, from: v)
        case LS_ACTION_SAVE: output(.save, from: v)
        case LS_ACTION_PIN: output(.pin, from: v)
        case LS_ACTION_OCR, LS_ACTION_TRANSLATE, LS_ACTION_LONGSHOT, LS_ACTION_RECORD: handOff(action, from: v)
        case LS_ACTION_COPY_RGB, LS_ACTION_COPY_HEX, LS_ACTION_COPY_CUSTOM: copyColor(action, from: v)
        default: break
        }
    }

    /// 文字识别、翻译、长截图、录屏：把选区交给对应模块。
    private func handOff(_ action: ls_action, from v: CaptureView) {
        guard let e = screen(of: v) else { return }
        let st = v.session.state
        guard st.has_selection != 0 else { return }
        let region = globalRect(st.selection.cg, on: e)
        let screen = e.screen
        switch action {
        case LS_ACTION_OCR, LS_ACTION_TRANSLATE:
            guard let r = Export.render(session: v.session, applyResolution: false) else { cancel(); return }
            recordLastArea(e)
            close()
            if action == LS_ACTION_OCR { OCRFeature.recognize(image: r.image, scale: r.scale, region: region) } else { TranslateFeature.translate(image: r.image, scale: r.scale, region: region) }
            restorePreviousApp()
        case LS_ACTION_LONGSHOT:
            if st.window_count > 0 {
                Toast.show(L("longshot.windowMode"), icon: "triangle-alert")
                return
            }
            close()
            LongShotFeature.start(region: region, screen: screen)
        case LS_ACTION_RECORD:
            if purpose == .record {
                // 录屏模式下 Shift 多选窗口：录它们的外接矩形
                let prev = previousApp
                previousApp = nil
                close()
                RecordFeature.begin(region: region, screen: screen, previousApp: prev)
                return
            }
            // 截图工具栏「录屏」：换成录屏模式的截图层，沿用当前选区（多选窗口时按钮已禁用）
            if st.window_count > 0 { return }
            let prev = previousApp
            previousApp = nil
            close()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
                guard let self = self, !self.isActive else { return }
                self.start(.record, initial: .region(region))
                if self.isActive { self.previousApp = prev }
            }
        default: break
        }
    }

    func captureViewDidBeginSelection(_ v: CaptureView) {
        for e in screens where e.view !== v {
            let st = e.view.session.state
            guard st.has_selection != 0 || st.has_hover != 0 else { continue }
            if let s = makeSession(frozen: e.frozen, screen: e.screen, windows: e.windows, key: e.key, initial: nil) {
                _ = s.mouseExit()
                e.view.replaceSession(s)
            }
        }
    }

    func captureViewMouseEntered(_ v: CaptureView) {
        guard let e = screen(of: v), !e.panel.isKeyWindow else { return }
        let others = screens.contains { $0.view !== v && $0.view.session.state.has_selection != 0 }
        if !others {
            e.panel.makeKey()
            e.panel.makeFirstResponder(v)
        }
    }

    func captureViewDoubleModifier(_ v: CaptureView) {
        guard purpose != .record, let k = Export.output(kindFor: Settings.shared.string("capture.doubleModifierAction")) else { return }
        output(k, from: v)
    }

    /// 全局文字识别、翻译、长截图进来的截图层：选完区域直接交给对应模块。
    private var purposeAction: ls_action? {
        switch purpose {
        case .ocr: return LS_ACTION_OCR
        case .translate: return LS_ACTION_TRANSLATE
        case .longshot: return LS_ACTION_LONGSHOT
        default: return nil
        }
    }

    func captureViewDidFinishSelection(_ v: CaptureView) {
        guard let a = purposeAction else { return }
        // 等这次事件处理完再收起截图层
        DispatchQueue.main.async { [weak self, weak v] in
            guard let self = self, let v = v, self.screen(of: v) != nil else { return }
            self.handOff(a, from: v)
        }
    }
}
