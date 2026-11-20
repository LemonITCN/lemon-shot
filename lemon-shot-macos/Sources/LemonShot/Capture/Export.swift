import AppKit
import LemonShotCore

/// 导出（spec 2.8）：按像素画「冻结画面裁剪 + 标注」，编码、复制、保存、音效。
enum Export {
    /// 结果图与其每逻辑点像素数。
    struct Result {
        var image: CGImage
        var scale: CGFloat
    }

    static func bitmapContext(width: Int, height: Int, space: CGColorSpace) -> CGContext? {
        CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
    }

    /// 选区导出。与屏幕上是同一套标注绘制代码，坐标换算到像素。
    static func render(session: Session, applyResolution: Bool = true) -> Result? {
        let px = session.selectionPixels
        let w = Int(px.width), h = Int(px.height)
        guard w > 0, h > 0 else { return nil }
        let fz = session.frozen
        guard let ctx = bitmapContext(width: w, height: h, space: fz.colorSpace) else { return nil }
        ctx.translateBy(x: 0, y: CGFloat(h))
        ctx.scaleBy(x: 1, y: -1)
        ctx.interpolationQuality = .none
        if let crop = fz.cgImage.cropping(to: px) {
            Draw.image(ctx, crop, in: CGRect(x: 0, y: 0, width: crop.width, height: crop.height))
        }
        let s = session.scale
        ctx.interpolationQuality = .high
        ctx.scaleBy(x: s, y: s)
        ctx.translateBy(x: -px.minX / s, y: -px.minY / s)
        ctx.clip(to: session.state.selection.cg)
        AnnotRender.drawAll(ctx, session: session)
        guard let img = ctx.makeImage() else { return nil }
        var result = Result(image: img, scale: s)
        if applyResolution { result = standardize(result) }
        return result
    }

    /// save.resolution 为 standard 且 scale > 1 时缩到 1 倍（ls_image_resize）。
    static func standardize(_ r: Result) -> Result {
        guard Settings.shared.string("save.resolution") == "standard", r.scale > 1.01 else { return r }
        let nw = max(1, Int((CGFloat(r.image.width) / r.scale).rounded())), nh = max(1, Int((CGFloat(r.image.height) / r.scale).rounded()))
        let space = FrozenImage.rgbSpace(r.image.colorSpace)
        if let src = FrozenImage.from(r.image, scale: r.scale) {
            var out = ls_image()
            var input = src.image
            if ls_image_resize(&input, Int32(nw), Int32(nh), &out) == 0, let fz = FrozenImage.adopt(out, scale: 1, space: space) {
                return Result(image: fz.cgImage, scale: 1)
            }
        }
        guard let ctx = bitmapContext(width: nw, height: nh, space: space) else { return r }
        ctx.interpolationQuality = .high
        ctx.draw(r.image, in: CGRect(x: 0, y: 0, width: nw, height: nh))
        return ctx.makeImage().map { Result(image: $0, scale: 1) } ?? r
    }

    /// 颜色描述文件：display 保留屏幕描述文件；srgb 转成 sRGB。
    static func applyProfile(_ img: CGImage) -> CGImage {
        guard Settings.shared.string("save.colorProfile") == "srgb", img.colorSpace != Theme.sRGB,
              let ctx = bitmapContext(width: img.width, height: img.height, space: Theme.sRGB) else { return img }
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
        return ctx.makeImage() ?? img
    }

    static func flattened(_ img: CGImage) -> CGImage {
        let space = FrozenImage.rgbSpace(img.colorSpace)
        guard let ctx = bitmapContext(width: img.width, height: img.height, space: space) else { return img }
        ctx.setFillColor(CGColor(gray: 1, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: img.width, height: img.height))
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
        return ctx.makeImage() ?? img
    }

    // MARK: 编码

    static func fileExtension(_ format: String) -> String {
        switch format {
        case "jpg": return "jpg"
        case "tiff": return "tiff"
        case "bmp": return "bmp"
        case "pdf": return "pdf"
        case "heic": return "heic"
        case "webp": return "webp"
        default: return "png"
        }
    }

    static func encode(_ src: CGImage, format: String, scale: CGFloat) -> Data? {
        if format == "webp" { return webpData(src) ?? encode(src, format: "png", scale: scale) }
        let img = applyProfile(src)
        if format == "pdf" {
            let data = NSMutableData()
            var box = CGRect(x: 0, y: 0, width: CGFloat(img.width) / scale, height: CGFloat(img.height) / scale)
            guard let consumer = CGDataConsumer(data: data as CFMutableData), let ctx = CGContext(consumer: consumer, mediaBox: &box, nil) else { return nil }
            ctx.beginPDFPage(nil)
            ctx.draw(img, in: box)
            ctx.endPDFPage()
            ctx.closePDF()
            return data as Data
        }
        let type: String
        var props: [CFString: Any] = [:]
        var image = img
        switch format {
        case "jpg":
            type = "public.jpeg"
            props[kCGImageDestinationLossyCompressionQuality] = Settings.shared.double("save.jpegQuality") / 100
            image = flattened(img)
        case "tiff": type = "public.tiff"
        case "bmp":
            type = "com.microsoft.bmp"
            image = flattened(img)
        case "heic":
            type = "public.heic"
            props[kCGImageDestinationLossyCompressionQuality] = Settings.shared.double("save.jpegQuality") / 100
        default: type = "public.png"
        }
        if scale > 1.01 {
            props[kCGImagePropertyDPIWidth] = 72 * scale
            props[kCGImagePropertyDPIHeight] = 72 * scale
        }
        let data = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(data as CFMutableData, type as CFString, 1, nil) else {
            return format == "png" ? nil : encode(src, format: "png", scale: scale)
        }
        CGImageDestinationAddImage(dest, image, props as CFDictionary)
        guard CGImageDestinationFinalize(dest) else { return format == "png" ? nil : encode(src, format: "png", scale: scale) }
        return data as Data
    }

    /// WebP 无损（内核附带的 libwebp）。WebP 不带描述文件，一律先转成 sRGB。超过 16383 像素边长时返回空。
    static let webpMaxDimension = 16383

    static func webpData(_ src: CGImage) -> Data? {
        guard src.width <= webpMaxDimension, src.height <= webpMaxDimension,
              let fz = FrozenImage.make(width: src.width, height: src.height, scale: 1, space: Theme.sRGB, draw: { ctx in
                  ctx.interpolationQuality = .none
                  Draw.image(ctx, src, in: CGRect(x: 0, y: 0, width: src.width, height: src.height))
              }) else { return nil }
        var input = fz.image
        var out: UnsafeMutablePointer<UInt8>?
        var size = 0
        guard ls_webp_encode(&input, 1, 75, &out, &size) == 0, let p = out, size > 0 else { return nil }
        defer { ls_free(p) }
        return Data(bytes: p, count: size)
    }

    static func pngData(_ img: CGImage, scale: CGFloat = 1) -> Data? {
        let data = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(data as CFMutableData, "public.png" as CFString, 1, nil) else { return nil }
        var props: [CFString: Any] = [:]
        if scale > 1.01 { props[kCGImagePropertyDPIWidth] = 72 * scale; props[kCGImagePropertyDPIHeight] = 72 * scale }
        CGImageDestinationAddImage(dest, img, props as CFDictionary)
        return CGImageDestinationFinalize(dest) ? data as Data : nil
    }

    static func tiffData(_ img: CGImage) -> Data? {
        let data = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(data as CFMutableData, "public.tiff" as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dest, img, nil)
        return CGImageDestinationFinalize(dest) ? data as Data : nil
    }

    // MARK: 输出

    /// 复制：剪贴板放 PNG + TIFF。
    static func copy(_ r: Result) {
        let img = applyProfile(r.image)
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.declareTypes([.png, .tiff], owner: nil)
        if let png = pngData(img, scale: r.scale) { pb.setData(png, forType: .png) }
        if let tiff = tiffData(img) { pb.setData(tiff, forType: .tiff) }
    }

    static func fileName(template: String, date: Date = Date()) -> String {
        var out = ""
        var i = template.startIndex
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        while i < template.endIndex {
            if template[i] == "{", let close = template[i...].firstIndex(of: "}") {
                let pattern = String(template[template.index(after: i)..<close])
                f.dateFormat = pattern
                out += f.string(from: date)
                i = template.index(after: close)
            } else {
                out.append(template[i])
                i = template.index(after: i)
            }
        }
        let bad = CharacterSet(charactersIn: "/:")
        return out.components(separatedBy: bad).joined(separator: "-").trimmingCharacters(in: .whitespaces)
    }

    static func uniqueURL(folder: URL, base: String, ext: String) -> URL {
        var url = folder.appendingPathComponent(base).appendingPathExtension(ext)
        var n = 2
        while FileManager.default.fileExists(atPath: url.path) {
            url = folder.appendingPathComponent("\(base) (\(n))").appendingPathExtension(ext)
            n += 1
        }
        return url
    }

    /// 保存到 save.folder（或每次询问）。返回保存的路径。
    static func save(_ r: Result) -> URL? {
        let s = Settings.shared
        var format = s.string("save.format")
        // WebP 边长上限 16383 像素（长截图可能超过）：退回 PNG
        if format == "webp" && max(r.image.width, r.image.height) > webpMaxDimension { format = "png" }
        let ext = fileExtension(format)
        let base = fileName(template: s.string("save.fileName").isEmpty ? "Lemon Shot {yyyy-MM-dd HH.mm.ss}" : s.string("save.fileName"))
        let folder = Settings.resolvePath(s.string("save.folder"))
        var url: URL
        if s.bool("save.askLocation") {
            let panel = NSSavePanel()
            panel.directoryURL = folder
            panel.nameFieldStringValue = base + "." + ext
            panel.canCreateDirectories = true
            panel.level = NSWindow.Level(rawValue: NSWindow.Level.screenSaver.rawValue + 1)
            NSApp.activate(ignoringOtherApps: true)
            guard panel.runModal() == .OK, let u = panel.url else { return nil }
            url = u
        } else {
            do {
                try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            } catch {
                Toast.show(L("notify.folderMissing"), icon: "triangle-alert")
                return nil
            }
            url = uniqueURL(folder: folder, base: base, ext: ext)
        }
        guard let data = encode(r.image, format: format, scale: r.scale) else {
            Toast.show(L("notify.saveFailed", ["error": ext]), icon: "triangle-alert")
            return nil
        }
        do {
            try data.write(to: url, options: .atomic)
        } catch {
            Toast.show(L("notify.saveFailed", ["error": error.localizedDescription]), icon: "triangle-alert")
            return nil
        }
        return url
    }

    static func playSound() {
        guard Settings.shared.bool("general.playSound") else { return }
        let grab = "/System/Library/Components/CoreAudio.component/Contents/SharedSupport/SystemSounds/system/Grab.aif"
        if FileManager.default.fileExists(atPath: grab), let snd = NSSound(contentsOfFile: grab, byReference: true) {
            snd.play()
        } else {
            NSSound(named: "Pop")?.play()
        }
    }

    enum Output { case copy, save, both, pin, saveAndOpen }

    /// 按动作输出。pinFrame：贴图初始位置（屏幕坐标，Cocoa）。
    static func output(_ r: Result, _ kind: Output, pinFrame: CGRect?) {
        switch kind {
        case .copy:
            copy(r)
            playSound()
            Toast.show(L("notify.copied"))
        case .save, .both, .saveAndOpen:
            let alsoCopy = kind == .both || Settings.shared.bool("save.alsoCopyWhenSaving")
            if alsoCopy { copy(r) }
            guard let url = save(r) else { return }
            playSound()
            if kind == .saveAndOpen {
                open(url)
            } else {
                Toast.show(L("notify.saved")) { NSWorkspace.shared.activateFileViewerSelecting([url]) }
            }
        case .pin:
            PinManager.shared.pin(image: r.image, scale: r.scale, frame: pinFrame, source: "capture")
        }
    }

    /// 保存后按 capture.openWithApp 打开：空 = 系统默认，folder = 在文件夹中显示，否则为应用路径。
    static func open(_ url: URL) {
        let app = Settings.shared.string("capture.openWithApp")
        if app.isEmpty {
            NSWorkspace.shared.open(url)
        } else if app == "folder" {
            NSWorkspace.shared.activateFileViewerSelecting([url])
        } else {
            let appURL = URL(fileURLWithPath: app)
            if #available(macOS 10.15, *) {
                NSWorkspace.shared.open([url], withApplicationAt: appURL, configuration: NSWorkspace.OpenConfiguration(), completionHandler: nil)
            } else {
                NSWorkspace.shared.openFile(url.path, withApplication: app)
            }
        }
    }

    static func output(kindFor setting: String) -> Output? {
        switch setting {
        case "copy": return .copy
        case "save": return .save
        case "both": return .both
        case "pin": return .pin
        case "saveAndOpen": return .saveAndOpen
        default: return nil
        }
    }
}
