import AppKit
import LemonShotCore

/// 自测截图（spec 1.1）：用固定假数据离屏渲染界面状态为 PNG 后退出。不弹窗口、不需要任何权限、不读写用户设置。
/// 带标注的状态通过真正调用 ls_session_* 构造，内核占位时标注为空，内核写好后自动正确。
enum SelfTest {
    static var scale: CGFloat = 2
    static let screenSize = CGSize(width: 1280, height: 800)
    /// 假窗口（z 序从上到下）：光标 (400,300) 落在第二个窗口里。
    static let fakeWindows: [CGRect] = [
        CGRect(x: 760, y: 90, width: 440, height: 300),
        CGRect(x: 240, y: 180, width: 600, height: 400),
        CGRect(x: 60, y: 60, width: 520, height: 360),
    ]

    static func run(outputDir: String, args: [String]) -> Int32 {
        func arg(_ name: String) -> String? {
            guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
            return args[i + 1]
        }
        scale = CGFloat(Double(arg("--scale") ?? "2") ?? 2)
        let theme = arg("--theme") ?? "light"
        let lang = arg("--lang") ?? "zh-Hans"

        Settings.shared.persistent = false
        Settings.shared.load()
        AppState.shared.persistent = false
        I18n.forced = lang == "en" ? "en" : "zh-Hans"
        I18n.load()
        Theme.forcedDark = theme == "dark"
        Permissions.fake = [.screen: true, .accessibility: true, .input: false, .microphone: true, .camera: false, .systemAudio: true]
        Devices.fake = []

        let dir = URL(fileURLWithPath: outputDir, isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var count = 0
        func write(_ name: String, _ img: CGImage?) {
            guard let img = img else { FileHandle.standardError.write(Data("selftest: \(name) 渲染失败\n".utf8)); return }
            if Draw.writePNG(img, to: dir.appendingPathComponent(name + ".png")) { count += 1 }
        }

        guard let screen = fakeScreen() else { return 1 }

        // 截图层
        write("capture-selecting", captureSelecting(screen))
        write("capture-selected", captureSelected(screen))
        write("capture-annotated", captureAnnotated(screen))
        for t in Int32(1)...Int32(10) { write("popover-" + AnnotStyles.key(t), popover(screen, tool: t)) }
        write("toolbar-hover", toolbarHover(screen))
        // 真实截图层的点击命中：选区比工具栏窄、宽时，8 个控制点都要落到截图层（工具栏的阴影边不能挡）
        let gripProblems = gripHitCheck(screen)
        for p in gripProblems { FileHandle.standardError.write(Data("selftest: \(p)\n".utf8)) }

        // 贴图
        write("pin", pin(screen))
        write("pin-library", pinLibrary(screen))

        // 文字识别、翻译、长截图（另写 ocr-check.txt：离屏跑一遍真实识别流程）
        Phase2SelfTest.run(screen: screen, dir: dir, write: write)

        // 录屏、录音（另写 record-check/：编码链路离屏检查）
        RecordSelfTest.render(screen, write: write)
        RecordSelfTest.encodeCheck(dir: dir)

        // 提示、菜单、偏好设置
        write("toast", toast(screen))
        write("menu", menu())
        for p in PrefsPages.all { write("prefs-" + p.id, PrefsWindowController.render(page: p.id, scale: scale)) }

        print("selftest: 输出 \(count) 张到 \(dir.path)")
        if !gripProblems.isEmpty { print("selftest: 控制点命中检查失败 \(gripProblems.count) 项"); return 1 }
        print("selftest: 控制点命中检查通过")
        return 0
    }

    /// 在离屏窗口里摆一个真实的截图层，选区分别比工具栏窄、宽、贴着屏幕底（工具栏翻到上方），
    /// 检查 8 个控制点被点中时接到事件的是截图层本身，不是工具栏、样式面板的宿主视图；工具栏按钮本身照样点得到。
    static func gripHitCheck(_ screen: FrozenImage) -> [String] {
        var problems: [String] = []
        let selections: [(String, CGRect)] = [
            ("比工具栏窄", CGRect(x: 600, y: 200, width: 160, height: 120)),
            ("比工具栏宽", CGRect(x: 120, y: 160, width: 900, height: 300)),
            ("贴着屏幕底", CGRect(x: 300, y: 560, width: 220, height: screenSize.height - 560)),
        ]
        for (name, sel) in selections {
            guard let s = makeSession(screen) else { problems.append("\(name)：建会话失败"); continue }
            _ = s.setSelection(sel)
            _ = s.setTool(Int32(LS_TOOL_RECT.rawValue)) // 有样式面板，一起检查
            let win = NSWindow(contentRect: CGRect(x: -30000, y: -30000, width: screenSize.width, height: screenSize.height),
                               styleMask: [.borderless], backing: .buffered, defer: false)
            win.isReleasedWhenClosed = false
            let view = CaptureView(frame: CGRect(origin: .zero, size: screenSize), session: s, mode: .capture, scale: scale)
            win.contentView = view
            view.showPopover(tool: Int32(LS_TOOL_RECT.rawValue), preview: false)
            let r = s.state.selection.cg
            let pts = [CGPoint(x: r.minX, y: r.minY), CGPoint(x: r.midX, y: r.minY), CGPoint(x: r.maxX, y: r.minY), CGPoint(x: r.maxX, y: r.midY),
                       CGPoint(x: r.maxX, y: r.maxY), CGPoint(x: r.midX, y: r.maxY), CGPoint(x: r.minX, y: r.maxY), CGPoint(x: r.minX, y: r.midY)]
            for (i, p) in pts.enumerated() {
                // 控制点中心，以及上下边控制点朝外 6pt（工具栏可能在下方或翻到上方）都要落到截图层；超出屏幕的不算
                let out: CGFloat = (4...6).contains(i) ? 6 : ((0...2).contains(i) ? -6 : 0)
                for q in [p, CGPoint(x: p.x, y: p.y + out)] where view.bounds.contains(q) {
                    let hit = view.hitTest(view.convert(q, to: view.superview))
                    if hit !== view { problems.append("\(name)：控制点 \(i) 在 \(q) 被 \(hit.map { String(describing: type(of: $0)) } ?? "无") 挡住") }
                }
            }
            // 工具栏按钮本身仍然点得到；工具栏四周的透明阴影边不接鼠标
            if let host = view.subviews.first(where: { ($0 as? WidgetHost)?.root is CaptureToolbar }) as? WidgetHost {
                let body = host.convert(host.root.frame, to: view)
                let margin = CGPoint(x: body.midX, y: body.minY - ChromeHost.margin / 2)
                if host.hitTest(host.superview!.convert(margin, from: view)) != nil { problems.append("\(name)：工具栏的透明边还在接鼠标") }
                let c = CGPoint(x: body.midX, y: body.midY)
                if s.selectionGrip(at: c) == nil, view.hitTest(view.convert(c, to: view.superview)) !== host {
                    problems.append("\(name)：工具栏中间点不到工具栏")
                }
            } else {
                problems.append("\(name)：没有工具栏")
            }
            win.contentView = nil
            win.close()
        }
        return problems
    }

    // MARK: 假数据

    /// 1280×800 假屏幕：灰白渐变 + 三个假窗口。
    static func fakeScreen() -> FrozenImage? {
        let w = Int(screenSize.width * scale), h = Int(screenSize.height * scale)
        return FrozenImage.make(width: w, height: h, scale: scale, space: Theme.sRGB) { ctx in
            ctx.scaleBy(x: scale, y: scale)
            let grad = CGGradient(colorsSpace: Theme.sRGB, colors: [Theme.hex("#E4E5EA"), Theme.hex("#F7F7F9")] as CFArray, locations: [0, 1])!
            ctx.drawLinearGradient(grad, start: .zero, end: CGPoint(x: 0, y: screenSize.height), options: [])
            let accents = ["#2F6BFF", "#30A46C", "#F5A524"]
            for (i, r) in fakeWindows.enumerated().reversed() {
                ctx.saveGState()
                Draw.setShadow(ctx, offsetY: 8, blur: 24, color: CGColor(gray: 0, alpha: 0.18))
                Draw.fill(ctx, r, Theme.hex("#FFFFFF"), radius: 10)
                ctx.restoreGState()
                ctx.saveGState()
                ctx.addPath(Draw.rounded(r, 10))
                ctx.clip()
                Draw.fill(ctx, CGRect(x: r.minX, y: r.minY, width: r.width, height: 30), Theme.hex("#F0F0F3"))
                for (j, c) in ["#FF5F57", "#FEBC2E", "#28C840"].enumerated() {
                    Draw.ellipse(ctx, CGRect(x: r.minX + 12 + CGFloat(j) * 18, y: r.minY + 9, width: 12, height: 12), fill: Theme.hex(c))
                }
                let accent = Theme.hex(accents[i])
                Draw.fill(ctx, CGRect(x: r.minX + 20, y: r.minY + 48, width: r.width * 0.45, height: 18), accent, radius: 4)
                var y = r.minY + 82
                var k = 0
                while y < r.maxY - 30 {
                    let lw = (r.width - 40) * [0.92, 0.78, 0.86, 0.6, 0.7][k % 5]
                    Draw.fill(ctx, CGRect(x: r.minX + 20, y: y, width: lw, height: 10), Theme.hex("#D9DBE1"), radius: 5)
                    y += 22
                    k += 1
                }
                let bars = [0.5, 0.8, 0.35, 0.65, 0.9]
                for (b, v) in bars.enumerated() {
                    let bh = CGFloat(v) * 70
                    Draw.fill(ctx, CGRect(x: r.maxX - 150 + CGFloat(b) * 24, y: r.maxY - 30 - bh, width: 16, height: bh), accent.alpha(0.75), radius: 3)
                }
                ctx.restoreGState()
                Draw.stroke(ctx, r, CGColor(gray: 0, alpha: 0.12), width: 1, radius: 10)
            }
        }
    }

    static func makeSession(_ screen: FrozenImage) -> Session? {
        var opts = Session.Options()
        opts.detectWindows = true
        opts.rightClickExits = false
        guard let s = Session(frozen: screen, size: screenSize, options: opts) else { return nil }
        s.setWindows(fakeWindows.enumerated().map { ls_window(id: Int64($0.offset + 1), rect: $0.element.ls) })
        return s
    }

    // MARK: 截图层场景

    struct SceneExtras {
        var hoverItem: ToolbarItem?
        var popoverTool: Int32?
    }

    /// 与屏幕同样的部件与位置（CaptureLayout），依次画进一张图。返回图与各部件区域（用于裁剪）。
    static func scene(_ s: Session, mouse: CGPoint, extras: SceneExtras = SceneExtras()) -> (CGImage?, [String: CGRect]) {
        let bounds = CGRect(origin: .zero, size: screenSize)
        let st = s.state
        let toolbar = CaptureToolbar(mode: .capture)
        let l = CaptureLayout.compute(session: s, state: st, mouse: mouse, mouseInView: true, mode: .capture, flags: CaptureLayout.Flags(),
                                      bounds: bounds, toolbarSize: toolbar.frame.size)
        var regions: [String: CGRect] = [:]
        var popover: StylePopover?
        var tooltip: (CGRect, String)?
        if let tr = l.toolbar {
            toolbar.frame = tr
            toolbar.layout()
            toolbar.update(st)
            regions["toolbar"] = tr
            if let t = extras.popoverTool ?? (st.tool != 0 ? st.tool : nil), let b = toolbar.button(.tool(t)) {
                let p = StylePopover(tool: t, get: { s.style(t) }, set: { _ = s.setStyle(t, $0) })
                p.frame = CaptureLayout.popover(session: s, toolbar: tr, anchorX: b.frame.midX, size: p.frame.size)
                p.layout()
                popover = p
                regions["popover"] = p.frame
            }
            if let item = extras.hoverItem, let b = toolbar.button(item) {
                b.hovered = true
                let r = CaptureLayout.tooltip(size: CaptureRender.tooltipSize(item.tooltip), toolbar: tr, button: b.frame, selection: st.selection.cg, bounds: bounds, scale: s.scale)
                tooltip = (r, item.tooltip)
                regions["tooltip"] = r
            }
        }
        let img = Offscreen.render(size: screenSize, scale: scale) { ctx in
            ctx.interpolationQuality = .none
            Draw.image(ctx, s.frozen.cgImage, in: bounds)
            ctx.interpolationQuality = .high
            CaptureRender.drawMain(ctx, session: s, state: st, bounds: bounds, dirty: bounds)
            if let p = l.crosshair { CaptureRender.drawCrosshair(ctx, at: p, bounds: bounds) }
            if let r = l.sizeTip { CaptureRender.drawSizeTip(ctx, r, l.sizeTipText) }
            if let r = l.tips { CaptureRender.drawTips(ctx, r, l.tipsLines, color: Theme.hex(Settings.shared.string("capture.tipsColor"))) }
            if let r = l.magnifier, let info = l.pixelInfo {
                CaptureRender.drawMagnifier(ctx, r, frozen: s.frozen, mouse: mouse, info: info, showHint: CaptureLayout.Flags().tips)
            }
            if l.toolbar != nil { toolbar.draw(ctx) }
            popover?.draw(ctx)
            if let (r, t) = tooltip { CaptureRender.drawTooltip(ctx, r, t) }
        }
        return (img, regions)
    }

    static func crop(_ img: CGImage?, _ r: CGRect) -> CGImage? {
        guard let img = img else { return nil }
        let px = CGRect(x: r.minX * scale, y: r.minY * scale, width: r.width * scale, height: r.height * scale).integral
        return img.cropping(to: px.intersection(CGRect(x: 0, y: 0, width: img.width, height: img.height)))
    }

    static let sampleSelection = CGRect(x: 200, y: 150, width: 640, height: 400)

    static func captureSelecting(_ screen: FrozenImage) -> CGImage? {
        guard let s = makeSession(screen) else { return nil }
        _ = s.mouseMove(CGPoint(x: 400, y: 300), mods: 0)
        return scene(s, mouse: CGPoint(x: 400, y: 300)).0
    }

    static func selectedSession(_ screen: FrozenImage) -> Session? {
        guard let s = makeSession(screen) else { return nil }
        _ = s.mouseMove(CGPoint(x: 400, y: 300), mods: 0)
        _ = s.setSelection(sampleSelection)
        _ = s.mouseMove(CGPoint(x: 520, y: 360), mods: 0)
        return s
    }

    static func captureSelected(_ screen: FrozenImage) -> CGImage? {
        guard let s = selectedSession(screen) else { return nil }
        return scene(s, mouse: CGPoint(x: 520, y: 360)).0
    }

    private static func drag(_ s: Session, _ pts: [CGPoint], mods: Int32 = 0) {
        guard let first = pts.first, let last = pts.last else { return }
        _ = s.mouseMove(first, mods: mods)
        _ = s.mouseDown(first, button: Int32(LS_BUTTON_LEFT), mods: mods, clicks: 1)
        for p in pts.dropFirst() { _ = s.mouseMove(p, mods: mods) }
        _ = s.mouseUp(last, button: Int32(LS_BUTTON_LEFT), mods: mods)
    }

    private static func click(_ s: Session, _ p: CGPoint) {
        _ = s.mouseMove(p, mods: 0)
        _ = s.mouseDown(p, button: Int32(LS_BUTTON_LEFT), mods: 0, clicks: 1)
        _ = s.mouseUp(p, button: Int32(LS_BUTTON_LEFT), mods: 0)
    }

    /// 先取消选中（无工具时单击选区内空白处），否则改样式会连带改掉刚画好的同类标注。
    private static func use(_ s: Session, _ tool: Int32, _ f: (inout ls_style) -> Void = { _ in }) {
        if s.state.selected_shape != 0 {
            _ = s.setTool(0)
            click(s, CGPoint(x: 820, y: 530))
        }
        var st = s.style(tool)
        f(&st)
        _ = s.setStyle(tool, st)
        if s.tool != tool { _ = s.setTool(tool) }
    }

    private static func line(_ a: CGPoint, _ b: CGPoint, steps: Int = 6) -> [CGPoint] {
        (0...steps).map { i in
            let t = CGFloat(i) / CGFloat(steps)
            return CGPoint(x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t)
        }
    }

    static func captureAnnotated(_ screen: FrozenImage) -> CGImage? {
        guard let s = selectedSession(screen) else { return nil }
        let pal = AnnotStyles.palette()
        // 第一行：矩形、椭圆、直线、放大镜、局部高亮
        use(s, Int32(LS_TOOL_RECT.rawValue))
        drag(s, line(CGPoint(x: 220, y: 172), CGPoint(x: 330, y: 245)))
        use(s, Int32(LS_TOOL_ELLIPSE.rawValue)) { $0.color = Theme.lsColor(hex: pal[5]) }
        drag(s, line(CGPoint(x: 350, y: 175), CGPoint(x: 455, y: 245)))
        use(s, Int32(LS_TOOL_LINE.rawValue)) { $0.color = Theme.lsColor(hex: pal[3]) }
        drag(s, line(CGPoint(x: 475, y: 240), CGPoint(x: 560, y: 180)))
        use(s, Int32(LS_TOOL_MAGNIFY.rawValue))
        drag(s, line(CGPoint(x: 620, y: 212), CGPoint(x: 656, y: 212)))
        use(s, Int32(LS_TOOL_HIGHLIGHT.rawValue))
        drag(s, line(CGPoint(x: 690, y: 172), CGPoint(x: 820, y: 250)))
        // 第二行：四种箭头、画笔、荧光笔
        for v in 0..<4 {
            use(s, Int32(LS_TOOL_ARROW.rawValue)) { $0.variant = Int32(v); $0.color = Theme.lsColor(hex: pal[v == 3 ? 6 : 0]) }
            let x = 222 + CGFloat(v) * 68
            drag(s, line(CGPoint(x: x, y: 345), CGPoint(x: x + 48, y: 278)))
        }
        use(s, Int32(LS_TOOL_PEN.rawValue)) { $0.variant = 0; $0.color = Theme.lsColor(hex: pal[7]) }
        drag(s, (0...24).map { i in CGPoint(x: 500 + CGFloat(i) * 5, y: 312 + sin(CGFloat(i) / 3) * 22) })
        use(s, Int32(LS_TOOL_PEN.rawValue)) { $0.variant = 1; $0.color = Theme.lsColor(hex: pal[2]) }
        drag(s, line(CGPoint(x: 650, y: 300), CGPoint(x: 815, y: 300), steps: 12))
        // 第三行：三种文字、序号 1-3
        for v in 0..<3 {
            use(s, Int32(LS_TOOL_TEXT.rawValue)) { $0.variant = Int32(v); $0.font_size = 18; $0.color = Theme.lsColor(hex: pal[v == 2 ? 5 : 0]) }
            click(s, CGPoint(x: 225 + CGFloat(v) * 140, y: 395))
            if s.textEdit() != nil {
                _ = s.setText(L("style.text.\(v)") + " Lemon")
                _ = s.endText()
            }
        }
        use(s, Int32(LS_TOOL_STEP.rawValue)) { $0.next_number = 1 }
        for i in 0..<3 { click(s, CGPoint(x: 690 + CGFloat(i) * 50, y: 395)) }
        // 第四行：像素马赛克
        use(s, Int32(LS_TOOL_MOSAIC.rawValue)) { $0.variant = 0; $0.block_size = 10 }
        drag(s, line(CGPoint(x: 250, y: 455), CGPoint(x: 470, y: 535)))
        // 选中矩形：回到无工具，单击矩形的边
        _ = s.setTool(0)
        click(s, CGPoint(x: 220, y: 210))
        _ = s.mouseMove(CGPoint(x: 600, y: 500), mods: 0)
        return scene(s, mouse: CGPoint(x: 600, y: 500)).0
    }

    static func popover(_ screen: FrozenImage, tool: Int32) -> CGImage? {
        guard let s = selectedSession(screen) else { return nil }
        _ = s.setTool(tool)
        let (img, regions) = scene(s, mouse: CGPoint(x: 520, y: 360), extras: SceneExtras(hoverItem: nil, popoverTool: tool))
        guard let tb = regions["toolbar"] else { return img }
        let r = tb.union(regions["popover"] ?? tb).insetBy(dx: -24, dy: -24)
        return crop(img, r)
    }

    static func toolbarHover(_ screen: FrozenImage) -> CGImage? {
        guard let s = selectedSession(screen) else { return nil }
        let (img, regions) = scene(s, mouse: CGPoint(x: 520, y: 360), extras: SceneExtras(hoverItem: .save, popoverTool: nil))
        guard let tb = regions["toolbar"] else { return img }
        return crop(img, tb.union(regions["tooltip"] ?? tb).insetBy(dx: -24, dy: -24))
    }

    // MARK: 贴图

    static func crop(of screen: FrozenImage, _ r: CGRect) -> CGImage? {
        screen.cgImage.cropping(to: CGRect(x: r.minX * scale, y: r.minY * scale, width: r.width * scale, height: r.height * scale).integral)
    }

    static func pin(_ screen: FrozenImage) -> CGImage? {
        guard let img = crop(of: screen, CGRect(x: 230, y: 170, width: 420, height: 280)) else { return nil }
        let w = PinWidget(image: img)
        let m = PinWidget.margin
        let size = CGSize(width: 420 + 2 * m, height: 280 + 2 * m)
        w.frame = CGRect(origin: .zero, size: size)
        w.active = true
        w.showButtons = true
        w.layout()
        w.buttons[1].hovered = true
        let bg = crop(of: screen, CGRect(x: 600, y: 300, width: size.width, height: size.height))
        return Offscreen.render(size: size, scale: scale) { ctx in
            if let bg = bg { Draw.image(ctx, bg, in: CGRect(origin: .zero, size: size)) }
            w.draw(ctx)
        }
    }

    static func pinLibrary(_ screen: FrozenImage) -> CGImage? {
        let regions = [CGRect(x: 240, y: 180, width: 600, height: 400), CGRect(x: 760, y: 90, width: 440, height: 300), CGRect(x: 60, y: 60, width: 520, height: 360),
                       CGRect(x: 250, y: 220, width: 200, height: 300), CGRect(x: 0, y: 0, width: 1280, height: 800), CGRect(x: 900, y: 200, width: 300, height: 120)]
        var items: [(PinLibrary.Entry, CGImage)] = []
        for (i, r) in regions.enumerated() {
            guard let img = crop(of: screen, r) else { continue }
            items.append((PinLibrary.Entry(file: "fake-\(i).png", source: "capture", hash: "\(i)", scale: Double(scale), created: Double(i)), img))
        }
        PinLibrary.shared.setFake(items)
        return PinLibraryWindowController.render(size: PinLibraryWindowController.size, scale: scale, hover: 1)
    }

    // MARK: 提示、菜单

    static func toast(_ screen: FrozenImage) -> CGImage? {
        let w = ToastWidget(text: L("notify.copied"), icon: "circle-check")
        let s = w.contentSize()
        let size = CGSize(width: s.width + 48, height: s.height + 48)
        w.frame = CGRect(x: 24, y: 24, width: s.width, height: s.height)
        let bg = crop(of: screen, CGRect(x: 300, y: 260, width: size.width, height: size.height))
        return Offscreen.render(size: size, scale: scale) { ctx in
            if let bg = bg { Draw.image(ctx, bg, in: CGRect(origin: .zero, size: size)) }
            w.draw(ctx)
        }
    }

    static func menu() -> CGImage? {
        let entries = AppMenu.entries()
        let w = MenuRenderWidget(entries: entries)
        let h = MenuRenderWidget.height(entries)
        let size = CGSize(width: MenuRenderWidget.width + 48, height: h + 48)
        w.frame = CGRect(x: 24, y: 24, width: MenuRenderWidget.width, height: h)
        return Offscreen.render(size: size, scale: scale) { ctx in
            Draw.fill(ctx, CGRect(origin: .zero, size: size), Theme.color("windowBg"))
            w.draw(ctx)
        }
    }
}
