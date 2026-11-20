import AppKit
import ServiceManagement

/// 各功能入口（全局快捷键与菜单共用）。
enum Features {
    static func run(_ id: String) {
        switch id {
        case "capture": CaptureController.shared.start()
        case "captureWindow": CaptureModes.captureWindowUnderCursor()
        case "captureFullscreen": CaptureModes.captureFullscreen()
        case "captureLastArea": CaptureController.shared.start(initial: .lastArea)
        case "capturePreset": CaptureController.shared.start(initial: .preset)
        case "captureDelayed": CaptureModes.captureDelayed()
        case "captureShell": ShellFeature.start()
        case "longshot": LongShotFeature.start()
        case "record": RecordFeature.start()
        case "recordAudio": AudioFeature.start()
        case "recordPause": RecordFeature.togglePause()
        case "recordPen": RecordFeature.togglePen()
        case "ocr": OCRFeature.start()
        case "translate": TranslateFeature.start()
        case "colorPicker": CaptureController.shared.start(.colorPicker)
        case "pinClipboard": PinManager.shared.pinClipboard()
        case "pinLibrary": PinLibraryWindowController.show()
        case "togglePins": PinManager.shared.toggleAll()
        default: break
        }
    }

    static func openScreenshotsFolder() {
        let url = Settings.resolvePath(Settings.shared.string("save.folder"))
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        NSWorkspace.shared.open(url)
    }
}

/// 开机启动：macOS 13+ 用 SMAppService；更早用用户 LaunchAgent。
enum LoginItem {
    private static var agentURL: URL {
        URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/LaunchAgents/cn.lemonit.shot.plist")
    }

    static func set(_ on: Bool) {
        if #available(macOS 13.0, *) {
            do {
                if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
            } catch {
                NSLog("Lemon Shot: 开机启动设置失败 %@", String(describing: error))
            }
            return
        }
        if on {
            guard let exe = Bundle.main.executablePath else { return }
            let plist: [String: Any] = ["Label": "cn.lemonit.shot", "ProgramArguments": [exe], "RunAtLoad": true, "ProcessType": "Interactive"]
            try? FileManager.default.createDirectory(at: agentURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            (plist as NSDictionary).write(to: agentURL, atomically: true)
        } else {
            try? FileManager.default.removeItem(at: agentURL)
        }
    }
}
