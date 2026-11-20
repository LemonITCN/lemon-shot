import AppKit
import LemonShotCore
import Vision

/// 文字识别（spec 4）的入口。
enum OCRFeature {
    /// 全局快捷键 / 菜单「文字识别」：开了「固定区域」且记得上次区域时直接识别该区域，否则进截图层（选完区域自动识别）。
    static func start() {
        if Settings.shared.bool("ocr.fixedRegion"), let r = savedRegion, let screen = NSScreen.screens.first(where: { $0.frame.contains(r.center) }) {
            guard CaptureController.ensurePermission() else { return }
            RegionGrabber.grabOnce(region: r, screen: screen) { img in
                guard let img = img else {
                    CaptureController.shared.start(.ocr)
                    return
                }
                OCRWindowController.recognize(image: img, scale: screen.backingScaleFactor)
            }
            return
        }
        CaptureController.shared.start(.ocr)
    }

    /// 截图层工具栏「文字识别」：选区图像（像素）与它在屏幕上的位置（Cocoa 全局坐标）。
    static func recognize(image: CGImage, scale: CGFloat, region: CGRect) {
        savedRegion = region
        // 截图层收尾时会切回原来的应用（capture.returnToPreviousApp）：等它切完再把结果窗口放到前面
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            OCRWindowController.recognize(image: image, scale: scale)
        }
    }

    /// 贴图右键菜单「文字识别」。
    static func recognize(pinImage: CGImage, scale: CGFloat) {
        OCRWindowController.recognize(image: pinImage, scale: scale)
    }

    /// 固定区域：上一次截图层识别的区域（Cocoa 全局坐标），存 state.json。
    static var savedRegion: CGRect? {
        get {
            guard let a = AppState.shared.value("ocrRegion")?.array, a.count == 4 else { return nil }
            let v = a.compactMap { $0.double }
            guard v.count == 4, v[2] > 0, v[3] > 0 else { return nil }
            return CGRect(x: v[0], y: v[1], width: v[2], height: v[3])
        }
        set {
            guard let r = newValue else { return }
            AppState.shared.set("ocrRegion", .array([.number(Double(r.minX)), .number(Double(r.minY)), .number(Double(r.width)), .number(Double(r.height))]))
        }
    }
}

/// 识别引擎：Vision 文字识别 + 条码识别；结果行交给内核 ls_text_layout 整理（与 Windows 同一套规则）。
enum OCREngine {
    struct Line {
        var text: String
        /// 图像像素坐标（左上原点）。
        var box: CGRect
        var confidence: Float
    }

    struct Output {
        var lines: [Line] = []
        var codes: [String] = []
        /// 实际使用的识别语言（自测记录用）。
        var languages: [String] = []
    }

    static let queue = DispatchQueue(label: "cn.lemonit.shot.ocr", qos: .userInitiated)

    /// 后台识别，回调在主线程。
    static func recognize(_ image: CGImage, completion: @escaping (Output?, String?) -> Void) {
        queue.async {
            do {
                let out = try run(image)
                DispatchQueue.main.async { completion(out, nil) }
            } catch {
                let msg = error.localizedDescription
                DispatchQueue.main.async { completion(nil, msg) }
            }
        }
    }

    /// 期望的语言：zh-Hans、zh-Hant、en、ja、ko，与系统支持的取交集（macOS 11/12 不支持日韩，自动跳过）。
    static func languages(for req: VNRecognizeTextRequest) -> [String] {
        var supported: [String] = []
        if #available(macOS 12.0, *) {
            supported = (try? req.supportedRecognitionLanguages()) ?? []
        } else {
            supported = (try? VNRecognizeTextRequest.supportedRecognitionLanguages(for: .accurate, revision: req.revision)) ?? []
        }
        var out: [String] = []
        for w in ["zh-Hans", "zh-Hant", "en", "ja", "ko"] {
            if let m = supported.first(where: { $0 == w || $0.hasPrefix(w + "-") }), !out.contains(m) { out.append(m) }
        }
        return out.isEmpty ? ["en-US"] : out
    }

    private static func textRequest() -> VNRecognizeTextRequest {
        let r = VNRecognizeTextRequest()
        r.recognitionLevel = .accurate
        r.usesLanguageCorrection = false
        return r
    }

    /// 同步识别（在后台线程或自测里调用）。
    static func run(_ image: CGImage) throws -> Output {
        var out = Output()
        let w = CGFloat(image.width), h = CGFloat(image.height)
        // 语言组合不被接受时依次退到「中英」「英」
        let full = languages(for: textRequest())
        var attempts: [[String]] = [full]
        let zhEn = full.filter { $0.hasPrefix("zh") || $0.hasPrefix("en") }
        if zhEn != full && !zhEn.isEmpty { attempts.append(zhEn) }
        if !attempts.contains(["en-US"]) { attempts.append(["en-US"]) }
        var lastError: Error?
        var done = false
        for langs in attempts {
            let req = textRequest()
            req.recognitionLanguages = langs
            do {
                try VNImageRequestHandler(cgImage: image, options: [:]).perform([req])
                for o in req.results ?? [] {
                    guard let cand = o.topCandidates(1).first else { continue }
                    let bb = o.boundingBox
                    let r = CGRect(x: bb.minX * w, y: (1 - bb.maxY) * h, width: bb.width * w, height: bb.height * h)
                    out.lines.append(Line(text: cand.string, box: r, confidence: cand.confidence))
                }
                out.languages = langs
                done = true
                break
            } catch {
                lastError = error
            }
        }
        if !done, let e = lastError { throw e }

        let bar = VNDetectBarcodesRequest()
        if (try? VNImageRequestHandler(cgImage: image, options: [:]).perform([bar])) != nil {
            var found: [(CGRect, String)] = []
            for o in bar.results ?? [] {
                guard let s = o.payloadStringValue, !s.isEmpty, !found.contains(where: { $0.1 == s }) else { continue }
                found.append((o.boundingBox, s))
            }
            // 从上到下（Vision 坐标原点在左下）
            found.sort { a, b in abs(a.0.maxY - b.0.maxY) > 0.02 ? a.0.maxY > b.0.maxY : a.0.minX < b.0.minX }
            out.codes = found.map { $0.1 }
        }
        return out
    }

    /// 内核整理：阅读顺序、合并换行、英文标点。
    static func layout(_ lines: [Line], joinLines: Bool, asciiPunct: Bool) -> String {
        guard !lines.isEmpty else { return "" }
        var flags: Int32 = 0
        if joinLines { flags |= Int32(LS_TEXT_JOIN_LINES) }
        if asciiPunct { flags |= Int32(LS_TEXT_ASCII_PUNCT) }
        let cstrs = lines.map { strdup($0.text) }
        defer { for p in cstrs { free(p) } }
        var arr: [ls_text_line] = lines.enumerated().map { i, l in
            ls_text_line(text: UnsafePointer(cstrs[i]), box: l.box.ls, confidence: l.confidence)
        }
        guard let p = ls_text_layout(&arr, Int32(arr.count), flags) else { return "" }
        defer { ls_free(p) }
        return String(cString: p)
    }

    /// 文本是不是一个可以打开的链接。
    static func link(_ s: String) -> URL? {
        let t = s.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.contains("\n"), let u = URL(string: t), let scheme = u.scheme?.lowercased(), ["http", "https"].contains(scheme), u.host != nil else { return nil }
        return u
    }
}
