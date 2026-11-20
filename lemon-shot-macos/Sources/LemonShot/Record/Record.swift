import AppKit
import AVFoundation

/// 录屏（spec 8）的入口。
enum RecordFeature {
    static let didChange = Notification.Name("cn.lemonit.shot.recordingDidChange")

    private static var session: RecordSession?

    /// 录制中（含倒计时）。停止后生成文件期间为假。
    static var isRecording: Bool { session?.isActive == true }
    static var isPaused: Bool { session?.paused == true }

    static func notify() { NotificationCenter.default.post(name: didChange, object: nil) }

    /// 全局 record / 菜单「录屏」：进入录屏模式的截图层。录制中再按一次 = 停止。
    static func start() {
        if let s = session {
            if s.isActive { s.stop() }
            return
        }
        guard !CaptureController.shared.isActive else { return }
        CaptureController.shared.start(.record)
    }

    /// 截图层工具栏「录屏」：沿用已选区域，换成录屏模式的截图层（工具栏换成录屏设置面板）。region 为 Cocoa 全局坐标。
    static func start(region: CGRect, screen: NSScreen) {
        guard session == nil else { return }
        // 等截图层真正离开屏幕再重新冻结画面
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            guard session == nil, !CaptureController.shared.isActive else { return }
            CaptureController.shared.start(.record, initial: .region(region))
        }
    }

    /// 录屏模式截图层「开始录制」（回车、V、按钮）。
    static func begin(region: CGRect, screen: NSScreen, previousApp: NSRunningApplication?) {
        guard session == nil, region.width >= 2, region.height >= 2 else { return }
        let s = RecordSession(config: RecordConfig.load(), region: region, screen: screen, previousApp: previousApp)
        session = s
        s.onEnd = { [weak s] in
            if session === s { session = nil }
            notify()
        }
        s.begin()
    }

    static func stop() { session?.stop() }

    /// 全局 recordPause。
    static func togglePause() { session?.togglePause() }

    /// 全局 recordPen。
    static func togglePen() { session?.togglePen() }
}

/// 一次录屏：权限 → 浮层与采集 → 倒计时 → 录制（暂停、画笔、定时）→ 停止 → 写文件 → 录完之后。
final class RecordSession {
    enum Phase { case preparing, countdown, recording, finishing, ended }

    let config: RecordConfig
    let region: CGRect
    let screen: NSScreen
    let displayID: CGDirectDisplayID
    let fullscreen: Bool
    let startDate = Date()
    let queue = DispatchQueue(label: "cn.lemonit.shot.record", qos: .userInitiated)
    private(set) var phase = Phase.preparing
    private(set) var paused = false
    var onEnd: (() -> Void)?

    var isActive: Bool { phase == .preparing || phase == .countdown || phase == .recording }

    private var pipeline: RecordPipeline?
    private var source: ScreenSource?
    private var mic: MicSource?
    private var tempURL: URL?
    private var bar: RecordBarWindow?
    private var barOnlyWhenPaused = false
    private var border: RegionBorderWindow?
    private var countdown: RecordCountdown?
    private var pen: PenOverlay?
    private var clicks: ClickOverlay?
    private var keys: KeysOverlay?
    private var camera: CameraOverlay?
    private var tick: Timer?
    private var previousApp: NSRunningApplication?
    private var appBeforePen: NSRunningApplication?
    private var sourceReady = false
    private var countdownDone = false
    private var useMic = false
    private var useCamera = false
    private var useKeys = false
    private var statusIconShown = false

    init(config: RecordConfig, region: CGRect, screen: NSScreen, previousApp: NSRunningApplication?) {
        self.config = config
        self.screen = screen
        displayID = ScreenCapture.displayID(screen)
        // 区域对齐到像素并夹在屏内
        let scale = screen.backingScaleFactor
        let r = region.intersection(screen.frame)
        self.region = CGRect(x: (r.minX * scale).rounded() / scale, y: (r.minY * scale).rounded() / scale,
                             width: max(2, (r.width * scale).rounded()) / scale, height: max(2, (r.height * scale).rounded()) / scale)
        fullscreen = RecordGeometry.isFullscreen(self.region, screen: screen)
        self.previousApp = previousApp
    }

    // MARK: 开始

    func begin() {
        do {
            try FileManager.default.createDirectory(at: config.folder, withIntermediateDirectories: true)
        } catch {
            Toast.show(L("notify.folderMissing"), icon: "triangle-alert")
            finishEnded(restoreApp: true)
            return
        }
        RecordFeature.notify()
        requestPermissions { [weak self] in self?.prepare() }
    }

    /// 麦克风、摄像头没决定过时先请求；被拒绝或没有输入监控权限时继续录制但提示去授权（点 toast 打开权限页）。
    private func requestPermissions(_ done: @escaping () -> Void) {
        var warn: String?
        let finish = { [weak self] in
            guard let self = self, self.phase == .preparing else { return }
            if self.config.showKeys {
                self.useKeys = KeysOverlay.hasPermission
                if !self.useKeys { warn = warn ?? "record.needInputPermission" }
            }
            if let w = warn { Toast.show(L(w), icon: "triangle-alert") { PrefsWindowController.show(page: "permissions") } }
            done()
        }
        let cameraStep = { [weak self] in
            guard let self = self else { return }
            guard self.config.camera else { finish(); return }
            CapturePermission.ensure(.video) { ok in
                self.useCamera = ok
                if !ok { warn = warn ?? "record.needCameraPermission" }
                finish()
            }
        }
        if config.wantsMicrophone {
            CapturePermission.ensure(.audio) { [weak self] ok in
                self?.useMic = ok
                if !ok { warn = "record.needMicPermission" }
                cameraStep()
            }
        } else {
            cameraStep()
        }
    }

    private func prepare() {
        guard phase == .preparing else { return }
        let canExclude = RecordConfig.canExcludeWindows
        let width: Int, height: Int, pixelFormat: OSType
        if config.isGIF {
            (width, height) = RecordConfig.gifSize(points: region.size, gifScale: config.gifScale)
            pixelFormat = kCVPixelFormatType_32BGRA
        } else {
            (width, height) = RecordConfig.videoSize(points: region.size, scale: screen.backingScaleFactor, quality: config.quality)
            pixelFormat = canExclude ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange : kCVPixelFormatType_32BGRA
        }

        // 声音来源
        if useMic {
            mic = MicSource(deviceID: config.microphoneDevice, queue: queue)
            if mic == nil { Toast.show(L("record.noMicrophone"), icon: "triangle-alert") }
        }
        var audioSources = 0
        let micIndex = mic != nil ? audioSources : -1
        if mic != nil { audioSources += 1 }
        let sysIndex = config.wantsSystemAudio ? audioSources : -1
        if config.wantsSystemAudio { audioSources += 1 }

        // 写文件
        let p: RecordPipeline
        if config.isGIF {
            guard let g = GifFrameStore(width: width, height: height) else { fail(RecordError.encoder("gif")); return }
            p = RecordPipeline(queue: queue, movie: nil, gif: g, audioSources: 0)
        } else {
            let url = FileManager.default.temporaryDirectory.appendingPathComponent("lemonshot-rec-\(UUID().uuidString).mp4")
            tempURL = url
            do {
                let spec = MovieWriter.VideoSpec(width: width, height: height, fps: config.fps, bitrate: RecordConfig.bitrate(width: width, height: height, fps: config.fps))
                let movie = try MovieWriter(url: url, fileType: .mp4, video: spec, audio: audioSources > 0 ? .aac : nil)
                p = RecordPipeline(queue: queue, movie: movie, gif: nil, audioSources: audioSources)
            } catch {
                fail(error)
                return
            }
        }
        pipeline = p

        // 浮层（进画面）与边框（不进画面）
        if !fullscreen {
            let b = RegionBorderWindow(region: region)
            b.show()
            border = b
        }
        let penLayer = PenOverlay(region: region)
        penLayer.onEscape = { [weak self] in self?.setPen(false) }
        penLayer.show()
        pen = penLayer
        if config.showClicks {
            let c = ClickOverlay(screen: screen)
            c.show()
            clicks = c
        }
        if useKeys {
            let k = KeysOverlay(region: region)
            if k.show() { keys = k } else { k.close() }
        }
        if useCamera {
            if let c = CameraOverlay(deviceID: config.cameraDevice, region: region, shape: config.cameraShape) {
                c.show()
                camera = c
            } else {
                Toast.show(L("record.noCamera"), icon: "triangle-alert")
            }
        }
        let include = [pen?.windowID, clicks?.windowID, keys?.windowID, camera?.windowID].compactMap { $0 }.filter { $0 != 0 }

        // 控制条（倒计时结束后显示）
        let barWin = RecordBarWindow()
        barWin.widget.onPause = { [weak self] in self?.togglePause() }
        barWin.widget.onPen = { [weak self] in self?.togglePen() }
        barWin.widget.onStop = { [weak self] in self?.stop() }
        barWin.widget.onCancel = { [weak self] in self?.cancel() }
        bar = barWin

        // 采集
        let src = ScreenSources.make(queue: queue)
        src.onVideo = { [weak p] pb, h in p?.video(pb, host: h) }
        if sysIndex >= 0 { src.onAudio = { [weak p] sb, h in p?.audio(sysIndex, sb, host: h) } }
        src.onError = { [weak self] e in self?.sourceFailed(e) }
        source = src
        if let m = mic, micIndex >= 0 {
            m.onAudio = { [weak p] sb, h in p?.audio(micIndex, sb, host: h) }
            m.start()
        }
        let params = ScreenSourceParams(displayID: displayID, sourceRect: RecordGeometry.displayLocal(region, screen: screen), width: width, height: height,
                                        fps: config.isGIF ? config.gifFps : config.fps, pixelFormat: pixelFormat, showsCursor: config.showCursor,
                                        captureAudio: config.wantsSystemAudio, audioOnly: false, includeWindows: include)
        // 倒计时与采集启动并行（老系统的 CGDisplayStream 会同步回调）
        phase = .countdown
        src.start(params) { [weak self] err in
            guard let self = self, self.isActive else { return }
            if let e = err { self.fail(e); return }
            self.sourceReady = true
            self.armIfReady()
        }
        guard isActive else { return }
        if config.countdown > 0 {
            let cd = RecordCountdown(seconds: config.countdown, region: region)
            countdown = cd
            cd.start(done: { [weak self] in
                self?.countdown = nil
                self?.countdownDone = true
                self?.armIfReady()
            }, cancelled: { [weak self] in
                self?.countdown = nil
                self?.cancel()
            })
        } else {
            countdownDone = true
            armIfReady()
        }
        RecordFeature.notify()
    }

    /// 倒计时走完且采集已开始：从这一刻之后的第一帧开始计时。
    private func armIfReady() {
        guard phase == .countdown, sourceReady, countdownDone, let p = pipeline else { return }
        phase = .recording
        let h = HostClock.now()
        queue.async { p.arm(at: h) }
        showBar()
        RecordStatusIcon.shared.begin()
        statusIconShown = true
        restorePreviousApp()
        let t = Timer(timeInterval: 0.25, repeats: true) { [weak self] _ in self?.update() }
        RunLoop.main.add(t, forMode: .common)
        tick = t
        RecordFeature.notify()
    }

    private func showBar() {
        guard let b = bar else { return }
        let outsideOnly = !RecordConfig.canExcludeWindows
        if let f = RecordGeometry.barFrame(size: b.barSize, region: region, screen: screen, fullscreen: fullscreen, outside: outsideOnly) {
            b.place(f)
            b.autoFade = fullscreen
            b.show()
        } else if let f = RecordGeometry.barFrame(size: b.barSize, region: region, screen: screen, fullscreen: true, outside: false) {
            // 老系统排除不了窗口、区域外又放不下：控制条只在暂停时显示
            barOnlyWhenPaused = true
            b.place(f)
        }
    }

    private func restorePreviousApp() {
        if let app = previousApp, !app.isTerminated {
            app.activate(options: [])
        } else {
            NSApp.deactivate()
        }
    }

    // MARK: 录制中

    private var elapsed: Double {
        guard let p = pipeline else { return 0 }
        let now = HostClock.now()
        return queue.sync { p.timeline.elapsed(at: now) }
    }

    private func update() {
        guard phase == .recording else { return }
        let e = elapsed
        let clock = RecordConfig.clock(e)
        if let b = bar {
            b.widget.clock = clock
            b.relayoutIfNeeded()
            b.refresh()
        }
        RecordStatusIcon.shared.update(clock: clock, paused: paused)
        if config.maxDurationSec > 0 && e >= Double(config.maxDurationSec) { stop() }
    }

    func togglePause() {
        guard phase == .recording, let p = pipeline else { return }
        paused.toggle()
        let h = HostClock.now()
        let nowPaused = paused
        queue.async { if nowPaused { p.timeline.pause(at: h) } else { p.timeline.resume(at: h) } }
        if let b = bar {
            b.widget.paused = paused
            b.refresh()
            if barOnlyWhenPaused { if paused { b.show() } else { b.hide() } }
        }
        update()
        RecordFeature.notify()
    }

    func togglePen() {
        guard phase == .recording, let p = pen else { return }
        setPen(!p.active)
    }

    private func setPen(_ on: Bool) {
        guard let p = pen, p.active != on else { return }
        if on {
            let front = NSWorkspace.shared.frontmostApplication
            appBeforePen = front?.processIdentifier == ProcessInfo.processInfo.processIdentifier ? nil : front
            p.activate()
        } else {
            p.deactivate()
            if let app = appBeforePen ?? previousApp, !app.isTerminated { app.activate(options: []) }
            appBeforePen = nil
        }
        bar?.widget.penOn = on
        bar?.refresh()
    }

    // MARK: 结束

    func stop() {
        switch phase {
        case .preparing, .countdown:
            cancel()
            return
        case .recording: break
        default: return
        }
        phase = .finishing
        let h = HostClock.now()
        if let p = pipeline { queue.async { p.timeline.stop(at: h) } }
        if pen?.active == true { setPen(false) }
        teardownUI()
        RecordFeature.notify()
        // 先停采集（排队中的样本处理完），再收尾写文件
        let group = DispatchGroup()
        if let s = source {
            group.enter()
            s.stop { group.leave() }
        }
        if let m = mic {
            group.enter()
            m.stop { group.leave() }
        }
        group.notify(queue: .main) { [weak self] in self?.finalize() }
    }

    func cancel() {
        guard isActive else { return }
        let wasCounting = phase != .recording
        phase = .ended
        if pen?.active == true { setPen(false) }
        teardownUI()
        source?.stop {}
        mic?.stop()
        if let p = pipeline { queue.async { p.cancel() } }
        if wasCounting { restorePreviousApp() }
        finishEnded(restoreApp: false)
    }

    private func fail(_ error: Error) {
        guard isActive else { return }
        cancel()
        Toast.show(L("record.failed", ["error": error.localizedDescription]), icon: "triangle-alert")
    }

    /// 录制中采集意外中止：保存已录到的部分。
    private func sourceFailed(_ error: Error) {
        if phase == .recording { stop() } else { fail(error) }
    }

    private func teardownUI() {
        tick?.invalidate()
        tick = nil
        countdown?.close()
        countdown = nil
        bar?.close()
        bar = nil
        border?.close()
        border = nil
        pen?.close()
        clicks?.close()
        clicks = nil
        keys?.close()
        keys = nil
        camera?.close()
        camera = nil
        if statusIconShown {
            RecordStatusIcon.shared.end()
            statusIconShown = false
        }
    }

    private func finalize() {
        guard let p = pipeline else { finishEnded(restoreApp: false); return }
        let isGIF = config.isGIF
        queue.async { [weak self] in
            p.finish { ok, duration in
                DispatchQueue.main.async { self?.written(ok: ok, duration: duration, isGIF: isGIF) }
            }
        }
    }

    private func written(ok: Bool, duration: Double, isGIF: Bool) {
        guard ok, let p = pipeline else {
            if let t = tempURL { try? FileManager.default.removeItem(at: t) }
            pipeline?.gif?.discard()
            Toast.show(L("record.failed", ["error": RecordError.noFrames.localizedDescription]), icon: "triangle-alert")
            finishEnded(restoreApp: false)
            return
        }
        let folder = config.folder
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        if isGIF, let g = p.gif {
            let progress = RecordProgressWindow(text: L("record.encodingGif"), screen: screen)
            let out = RecordConfig.outputURL(folder: folder, ext: "gif", date: startDate)
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                var last = 0.0
                let okGif = g.encode(to: out, end: duration) { v in
                    guard v - last >= 0.01 || v >= 1 else { return }
                    last = v
                    DispatchQueue.main.async { progress.progress = v }
                }
                g.discard()
                DispatchQueue.main.async {
                    progress.close()
                    self?.saved(okGif ? out : nil)
                }
            }
            return
        }
        guard let tmp = tempURL else { finishEnded(restoreApp: false); return }
        let out = RecordConfig.outputURL(folder: folder, ext: "mp4", date: startDate)
        do {
            try FileManager.default.moveItem(at: tmp, to: out)
            saved(out)
        } catch {
            Toast.show(L("notify.saveFailed", ["error": error.localizedDescription]), icon: "triangle-alert")
            finishEnded(restoreApp: false)
        }
    }

    /// 录完之后（none / play / reveal）+ toast record.saved（点击在文件夹中显示）。
    private func saved(_ url: URL?) {
        guard let url = url else {
            Toast.show(L("record.failed", ["error": "GIF"]), icon: "triangle-alert")
            finishEnded(restoreApp: false)
            return
        }
        switch config.afterAction {
        case "play": NSWorkspace.shared.open(url)
        case "reveal": NSWorkspace.shared.activateFileViewerSelecting([url])
        default: break
        }
        Toast.show(L("record.saved")) { NSWorkspace.shared.activateFileViewerSelecting([url]) }
        finishEnded(restoreApp: false)
    }

    private func finishEnded(restoreApp: Bool) {
        phase = .ended
        if restoreApp { restorePreviousApp() }
        let cb = onEnd
        onEnd = nil
        cb?()
    }
}

// MARK: - 菜单栏图标

/// 录制时菜单栏图标换成红点 + 计时（spec 8.2、12）：单击停止，右键（或 Control 单击）弹出菜单。结束后恢复原图标与菜单。
final class RecordStatusIcon: NSObject {
    static let shared = RecordStatusIcon()
    private weak var item: NSStatusItem?
    private weak var menu: NSMenu?
    private var saved: (image: NSImage?, length: CGFloat, visible: Bool, position: NSControl.ImagePosition)?

    /// AppDelegate 建好状态栏图标后调用。
    static func attach(_ item: NSStatusItem, menu: NSMenu) {
        shared.item = item
        shared.menu = menu
    }

    private static func dot(paused: Bool) -> NSImage {
        let img = NSImage(size: NSSize(width: 12, height: 12), flipped: true) { r in
            guard let ctx = NSGraphicsContext.current?.cgContext else { return false }
            let c = paused ? NSColor.secondaryLabelColor.cgColor : Theme.tokenColor("recordBar.dot")
            Draw.ellipse(ctx, r.insetBy(dx: 1, dy: 1), fill: c)
            return true
        }
        img.isTemplate = false
        return img
    }

    func begin() {
        guard saved == nil, let item = item, let b = item.button else { return }
        saved = (b.image, item.length, item.isVisible, b.imagePosition)
        item.menu = nil
        item.length = NSStatusItem.variableLength
        item.isVisible = true
        b.image = RecordStatusIcon.dot(paused: false)
        b.imagePosition = .imageLeading
        b.target = self
        b.action = #selector(clicked(_:))
        b.sendAction(on: [.leftMouseUp, .rightMouseUp])
        update(clock: RecordConfig.clock(0), paused: false)
    }

    func update(clock: String, paused: Bool) {
        guard saved != nil, let b = item?.button else { return }
        b.attributedTitle = NSAttributedString(string: " " + clock, attributes: [.font: NSFont.monospacedDigitSystemFont(ofSize: 12, weight: .medium)])
        b.image = RecordStatusIcon.dot(paused: paused)
    }

    func end() {
        guard let s = saved else { return }
        saved = nil
        guard let item = item, let b = item.button else { return }
        b.attributedTitle = NSAttributedString(string: "")
        b.title = ""
        b.image = s.image
        b.imagePosition = s.position
        b.target = nil
        b.action = nil
        item.length = s.length
        item.menu = menu
        item.isVisible = Settings.shared.bool("general.showStatusIcon")
    }

    @objc private func clicked(_ sender: Any?) {
        let e = NSApp.currentEvent
        let wantsMenu = e?.type == .rightMouseUp || e?.modifierFlags.contains(.control) == true
        if wantsMenu, let menu = menu, let b = item?.button {
            AppMenu.build(into: menu)
            menu.popUp(positioning: nil, at: NSPoint(x: 0, y: b.bounds.height + 5), in: b)
        } else {
            RecordFeature.stop()
        }
    }
}
