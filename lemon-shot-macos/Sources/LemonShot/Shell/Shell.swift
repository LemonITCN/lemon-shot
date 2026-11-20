import AppKit

/// 带壳截图（spec 7）的入口：延时后截光标所在屏的全屏，打开带壳预览窗口。
enum ShellFeature {
    /// 全局 captureShell / 菜单「带壳截图」。
    static func start() {
        guard !CaptureController.shared.isActive, CaptureController.ensurePermission() else { return }
        // 预览窗口开着时先收起，免得截进去
        ShellWindowController.current?.window.orderOut(nil)
        let delay = max(0, Settings.shared.double("shell.delay"))
        DispatchQueue.main.asyncAfter(deadline: .now() + delay + 0.05) {
            guard let screen = ScreenCapture.screen(at: NSEvent.mouseLocation) else { return }
            ScreenCapture.captureScreens([screen], cursor: Settings.shared.bool("capture.captureCursor")) { images in
                guard let img = images[ScreenCapture.displayID(screen)] else {
                    ShellWindowController.current?.window.orderFront(nil)
                    return
                }
                let scale = CGFloat(img.width) / screen.frame.width
                Export.playSound()
                ShellWindowController.show(image: img, scale: scale, auto: autoModel(for: screen))
            }
        }
    }

    // MARK: 自动匹配

    static var hardwareModel: String {
        var size = 0
        guard sysctlbyname("hw.model", nil, &size, nil, 0) == 0, size > 0 else { return "" }
        var buf = [CChar](repeating: 0, count: size)
        guard sysctlbyname("hw.model", &buf, &size, nil, 0) == 0 else { return "" }
        return String(cString: buf)
    }

    /// hw.model → 机型。只收录带内建屏幕的机器；查不到的按前缀归类。
    static let modelTable: [String: ShellModel] = [
        // MacBook Air（Apple 芯片）
        "MacBookAir10,1": .macbookAir, "Mac14,2": .macbookAir, "Mac14,15": .macbookAir, "Mac15,12": .macbookAir, "Mac15,13": .macbookAir,
        "Mac16,12": .macbookAir, "Mac16,13": .macbookAir,
        // MacBook Pro 14 英寸（13 英寸 Apple 芯片机型也归到这里）
        "MacBookPro17,1": .macbookPro14, "MacBookPro18,3": .macbookPro14, "MacBookPro18,4": .macbookPro14, "Mac14,7": .macbookPro14,
        "Mac14,5": .macbookPro14, "Mac14,9": .macbookPro14, "Mac15,3": .macbookPro14, "Mac15,6": .macbookPro14, "Mac15,8": .macbookPro14,
        "Mac15,10": .macbookPro14, "Mac16,1": .macbookPro14, "Mac16,6": .macbookPro14, "Mac16,8": .macbookPro14,
        // MacBook Pro 16 英寸
        "MacBookPro18,1": .macbookPro16, "MacBookPro18,2": .macbookPro16, "Mac14,6": .macbookPro16, "Mac14,10": .macbookPro16,
        "Mac15,7": .macbookPro16, "Mac15,9": .macbookPro16, "Mac15,11": .macbookPro16, "Mac16,5": .macbookPro16, "Mac16,7": .macbookPro16,
        // iMac 24 英寸
        "iMac21,1": .imac24, "iMac21,2": .imac24, "Mac15,4": .imac24, "Mac15,5": .imac24, "Mac16,2": .imac24, "Mac16,3": .imac24,
    ]

    static func modelFromHardware(_ hw: String) -> ShellModel? {
        if let m = modelTable[hw] { return m }
        if hw.hasPrefix("MacBookAir") { return .macbookAir }
        if hw.hasPrefix("MacBookPro") || hw.hasPrefix("MacBook") { return .laptop }
        if hw.hasPrefix("iMac") { return .imac24 }
        return nil
    }

    /// 被截的屏是内建屏：按 hw.model；外接屏：Studio Display 按名字认，其余为通用显示器。
    static func autoModel(for screen: NSScreen) -> ShellModel {
        let id = ScreenCapture.displayID(screen)
        if CGDisplayIsBuiltin(id) != 0 {
            return modelFromHardware(hardwareModel) ?? .laptop
        }
        if #available(macOS 10.15, *) {
            let name = screen.localizedName.lowercased()
            if name.contains("studio display") { return .studioDisplay }
        }
        // 台式机（mac mini、Mac Studio 等）接的也是外接屏
        if modelFromHardware(hardwareModel) == .imac24 { return .imac24 }
        return .monitor
    }
}
