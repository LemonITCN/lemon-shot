import AppKit
import CoreImage
import LemonShotCore

/// 第二期自测状态（spec 1.1）：ocr、translate、longshot、shell；外加离屏识别检查 ocr-check.txt。
/// 环境变量 LEMONSHOT_SELFTEST_EXTRA 非空时另出 x- 开头的核对图（带壳各机型、拖选区遮罩），不属于契约状态。
enum Phase2SelfTest {
    static func run(screen: FrozenImage, dir: URL, write: (String, CGImage?) -> Void) {
        let scale = SelfTest.scale
        let zh = I18n.lang == "zh-Hans"
        let thumbRect = CGRect(x: 240, y: 180, width: 600, height: 400)
        write("ocr", OCRWindowController.render(scale: scale, thumbnail: SelfTest.crop(of: screen, thumbRect), text: zh ? fakeOCRZh : fakeOCREn))
        write("translate", TranslateWindowController.render(scale: scale, source: zh ? fakeSourceEn : fakeSourceZh, result: zh ? fakeResultZh : fakeResultEn))
        write("longshot", longshot(screen))
        // 带壳预览：自动匹配（固定匹配到通用笔记本，与 Windows 自测同一场景）+ 透明背景 + 假屏幕截图
        write("shell", ShellWindowController.render(scale: scale, shot: screen.cgImage, model: "auto", background: "transparent", auto: .laptop))
        ocrCheck(dir)
        if let e = ProcessInfo.processInfo.environment["LEMONSHOT_SELFTEST_EXTRA"], !e.isEmpty {
            for m in ShellModel.allCases {
                write("x-shell-" + m.rawValue, ShellWindowController.render(scale: 1, shot: screen.cgImage, model: m.rawValue, background: m == .iphone ? "white" : "transparent"))
            }
            write("x-shell-auto-black", ShellWindowController.render(scale: 1, shot: screen.cgImage, model: "auto", background: "black"))
            write("x-capture-dragging", captureDragging(screen))
        }
    }

    // MARK: 假数据（测试内容，不是界面文案）

    static let fakeOCRZh = "LemonShot 截图工具\n支持文字识别、截图翻译、长截图和带壳截图。\n识别结果可以直接修改，复制后粘贴到任意位置。\n\nVersion 1.0.0 · 2026-11-20\nhttps://lemonit.cn"
    static let fakeOCREn = "LemonShot screenshot tool\nRecognize text, translate screenshots, capture scrolling pages and add device frames.\nEdit the result here, then paste it anywhere.\n\nVersion 1.0.0 · 2026-11-20\nhttps://lemonit.cn"
    static let fakeSourceEn = "LemonShot captures any part of your screen, recognizes the text in it and translates it into your language in one step."
    static let fakeResultZh = "LemonShot 可以截取屏幕的任意区域，识别其中的文字，并一步翻译成你的语言。"
    static let fakeSourceZh = "LemonShot 可以截取屏幕的任意区域，识别其中的文字，并一步翻译成你的语言。"
    static let fakeResultEn = "LemonShot captures any part of your screen, recognizes the text in it and translates it into your language in one step."

    /// 长截图进行中：假屏幕 + 区域边框 + 右侧面板（预览为区域内容竖向拼三份）。
    static func longshot(_ screen: FrozenImage) -> CGImage? {
        let region = SelfTest.sampleSelection
        guard let part = SelfTest.crop(of: screen, region) else { return nil }
        let w = part.width, h = part.height * 3
        guard let page = FrozenImage.make(width: w, height: h, scale: SelfTest.scale, space: Theme.sRGB, draw: { ctx in
            for i in 0..<3 { Draw.image(ctx, part, in: CGRect(x: 0, y: i * part.height, width: w, height: part.height)) }
        }) else { return nil }
        return LongShotSession.render(scale: SelfTest.scale, screen: screen, region: region, preview: page.cgImage, heightPixels: h)
    }

    /// 核对图：正在拖出新选区时只遮选区外（spec 2.2）。
    static func captureDragging(_ screen: FrozenImage) -> CGImage? {
        guard let s = SelfTest.makeSession(screen) else { return nil }
        let a = CGPoint(x: 300, y: 220), b = CGPoint(x: 760, y: 520)
        _ = s.mouseMove(a, mods: 0)
        _ = s.mouseDown(a, button: Int32(LS_BUTTON_LEFT), mods: 0, clicks: 1)
        for i in 1...6 {
            let t = CGFloat(i) / 6
            _ = s.mouseMove(CGPoint(x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t), mods: 0)
        }
        return SelfTest.scene(s, mouse: b).0
    }

    // MARK: 离屏识别检查

    /// 渲染一段中英文 + 二维码，跑同一套识别流程（Vision 离屏可用、不需要权限），结果写进 ocr-check.txt。
    static func ocrCheck(_ dir: URL) {
        let size = CGSize(width: 760, height: 300)
        let lines = ["LemonShot 文字识别自测", "这是第二行中文，用来检查阅读顺序。", "The quick brown fox jumps over the lazy dog.", "价格：128 元（含税）"]
        let qr = qrImage("https://lemonit.cn/lemon-shot")
        guard let img = Offscreen.render(size: size, scale: 2, { ctx in
            Draw.fill(ctx, CGRect(origin: .zero, size: size), CGColor(gray: 1, alpha: 1))
            var y: CGFloat = 30
            for l in lines {
                Draw.text(ctx, l, font: NSFont.systemFont(ofSize: 22), color: CGColor(gray: 0, alpha: 1), in: CGRect(x: 30, y: y, width: 520, height: 34), truncate: false)
                y += 50
            }
            if let q = qr {
                ctx.interpolationQuality = .none
                Draw.image(ctx, q, in: CGRect(x: 580, y: 60, width: 150, height: 150))
            }
        }) else { return }
        var report = ""
        let t0 = Date()
        do {
            let out = try OCREngine.run(img)
            let ms = Int(Date().timeIntervalSince(t0) * 1000)
            let b = OCRWindowController.Block(lines: out.lines, codes: out.codes)
            report += "languages: \(out.languages.joined(separator: ", "))\n"
            let t1 = Date()
            _ = try? OCREngine.run(img)
            let ms2 = Int(Date().timeIntervalSince(t1) * 1000)
            report += "lines: \(out.lines.count)  codes: \(out.codes.count)  time: \(ms) ms (first), \(ms2) ms (second)\n"
            report += "--- layout ---\n" + OCREngine.layout(out.lines, joinLines: false, asciiPunct: false) + "\n"
            report += "--- joinLines + asciiPunct ---\n" + OCREngine.layout(out.lines, joinLines: true, asciiPunct: true) + "\n"
            report += "--- codes ---\n" + out.codes.joined(separator: "\n") + "\n"
            report += "--- window text (settings) ---\n" + OCRWindowController.compose([b]) + "\n"
        } catch {
            report += "error: \(error.localizedDescription)\n"
        }
        try? Data(report.utf8).write(to: dir.appendingPathComponent("ocr-check.txt"))
        if let e = ProcessInfo.processInfo.environment["LEMONSHOT_SELFTEST_EXTRA"], !e.isEmpty {
            _ = Draw.writePNG(img, to: dir.appendingPathComponent("x-ocr-check-input.png"))
        }
    }

    private static func qrImage(_ s: String) -> CGImage? {
        guard let f = CIFilter(name: "CIQRCodeGenerator") else { return nil }
        f.setValue(Data(s.utf8), forKey: "inputMessage")
        f.setValue("M", forKey: "inputCorrectionLevel")
        guard let out = f.outputImage?.transformed(by: CGAffineTransform(scaleX: 8, y: 8)) else { return nil }
        return CIContext(options: nil).createCGImage(out, from: out.extent)
    }
}
