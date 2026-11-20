import AppKit
import CommonCrypto
import LemonShotCore

/// 贴图管理：新建、剪贴板贴图、隐藏/显示全部、关闭全部。
final class PinManager {
    static let shared = PinManager()
    private(set) var pins: [PinWindow] = []
    private(set) var hidden = false

    /// frame：图片在屏幕上的位置（Cocoa 坐标）；为空时放在光标处。
    @discardableResult
    func pin(image: CGImage, scale: CGFloat, frame: CGRect?, source: String, addToLibrary: Bool = true) -> PinWindow {
        let size = CGSize(width: CGFloat(image.width) / scale, height: CGFloat(image.height) / scale)
        var r: CGRect
        if let f = frame {
            r = CGRect(origin: f.origin, size: size)
            r.origin.y = f.maxY - size.height
        } else {
            let m = NSEvent.mouseLocation
            r = CGRect(x: m.x, y: m.y - size.height, width: size.width, height: size.height)
            if let vis = ScreenCapture.screen(at: m)?.visibleFrame {
                r.origin.x = max(vis.minX, min(r.minX, vis.maxX - size.width))
                r.origin.y = max(vis.minY, min(r.minY, vis.maxY - size.height))
            }
        }
        if hidden { setHidden(false) }
        let w = PinWindow(image: image, scale: scale, imageFrame: r, source: source)
        pins.append(w)
        w.orderFrontRegardless()
        w.makeKey()
        if addToLibrary { PinLibrary.shared.add(image: image, scale: scale, source: source) }
        return w
    }

    func close(_ w: PinWindow) {
        w.orderOut(nil)
        w.close()
        pins.removeAll { $0 === w }
    }

    func closeAll() {
        for w in pins { w.orderOut(nil); w.close() }
        pins.removeAll()
        hidden = false
    }

    func setHidden(_ h: Bool) {
        hidden = h
        for w in pins { if h { w.orderOut(nil) } else { w.orderFrontRegardless() } }
    }

    func toggleAll() { setHidden(!hidden) }

    // MARK: 剪贴板贴图

    func pinClipboard() {
        let pb = NSPasteboard.general
        if let img = Clipboard.image(pb) {
            pin(image: img.0, scale: img.1, frame: nil, source: "clipboard")
            return
        }
        if let rich = Clipboard.attributedText(pb), let img = Clipboard.render(rich) {
            pin(image: img.0, scale: img.1, frame: nil, source: "clipboard")
            return
        }
        Toast.show(L("pin.noClipboard"), icon: "clipboard")
    }
}

enum Clipboard {
    static func image(_ pb: NSPasteboard) -> (CGImage, CGFloat)? {
        guard let types = pb.types, types.contains(where: { [.png, .tiff, NSPasteboard.PasteboardType("public.jpeg"), .fileURL].contains($0) }) else { return nil }
        if types.contains(.fileURL), let url = (pb.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL])?.first,
           let src = CGImageSourceCreateWithURL(url as CFURL, nil), let img = CGImageSourceCreateImageAtIndex(src, 0, nil) {
            return (img, dpiScale(src))
        }
        for t in [NSPasteboard.PasteboardType.png, .tiff, NSPasteboard.PasteboardType("public.jpeg")] {
            if let d = pb.data(forType: t), let src = CGImageSourceCreateWithData(d as CFData, nil), let img = CGImageSourceCreateImageAtIndex(src, 0, nil) {
                return (img, dpiScale(src))
            }
        }
        return nil
    }

    private static func dpiScale(_ src: CGImageSource) -> CGFloat {
        guard let p = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any], let dpi = (p[kCGImagePropertyDPIWidth] as? NSNumber)?.doubleValue, dpi > 72 else { return 1 }
        return CGFloat(dpi / 72)
    }

    static func attributedText(_ pb: NSPasteboard) -> NSAttributedString? {
        if let rtf = pb.data(forType: .rtf), let a = NSAttributedString(rtf: rtf, documentAttributes: nil), a.length > 0 { return a }
        if let s = pb.string(forType: .string), !s.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            return NSAttributedString(string: s, attributes: [.font: Fonts.ui(13), .foregroundColor: NSColor(cgColor: Theme.color("text")) ?? .black])
        }
        return nil
    }

    /// 文字排版成图：最大宽 600pt，surface 底 + 12pt 内边距。
    static func render(_ text: NSAttributedString) -> (CGImage, CGFloat)? {
        let maxW: CGFloat = 600, pad: CGFloat = 12
        let bounds = text.boundingRect(with: CGSize(width: maxW - 2 * pad, height: 100_000), options: [.usesLineFragmentOrigin, .usesFontLeading])
        let size = CGSize(width: ceil(min(maxW, bounds.width + 2 * pad)), height: ceil(bounds.height + 2 * pad))
        let scale = NSScreen.main?.backingScaleFactor ?? 2
        let img = Offscreen.render(size: size, scale: scale) { ctx in
            Draw.fill(ctx, CGRect(origin: .zero, size: size), Theme.color("surface"))
            NSGraphicsContext.saveGraphicsState()
            NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: true)
            text.draw(with: CGRect(x: pad, y: pad, width: size.width - 2 * pad, height: size.height - 2 * pad), options: [.usesLineFragmentOrigin, .usesFontLeading])
            NSGraphicsContext.restoreGraphicsState()
        }
        return img.map { ($0, scale) }
    }
}

/// 贴图库：设置目录下 pins/，每张 <时间戳>.png + library.json（顺序、来源）。内容哈希去重，超过 libraryMax 删最旧。
final class PinLibrary {
    static let shared = PinLibrary()
    static let didChange = Notification.Name("cn.lemonit.shot.pinLibraryDidChange")

    struct Entry {
        var file: String
        var source: String
        var hash: String
        var scale: Double
        var created: Double
    }

    private(set) var entries: [Entry] = []
    private var thumbs: [String: CGImage] = [:]
    var persistent = true
    private var loaded = false

    var dir: URL { Settings.shared.directory.appendingPathComponent("pins", isDirectory: true) }
    private var indexURL: URL { dir.appendingPathComponent("library.json") }

    func load() {
        loaded = true
        guard persistent, let j = JSON.parse(file: indexURL), let items = j["items"]?.array else { return }
        entries = items.compactMap { it in
            guard let f = it["file"]?.string else { return nil }
            return Entry(file: f, source: it["source"]?.string ?? "", hash: it["hash"]?.string ?? "", scale: it["scale"]?.double ?? 1, created: it["created"]?.double ?? 0)
        }
    }

    /// 自测用：直接给条目和缩略图。
    func setFake(_ items: [(Entry, CGImage)]) {
        persistent = false
        loaded = true
        entries = items.map { $0.0 }
        for (e, img) in items { thumbs[e.file] = img }
    }

    private func save() {
        guard persistent else { return }
        var arr: [JSON] = []
        for e in entries {
            var o = JObject()
            o["file"] = .string(e.file)
            o["source"] = .string(e.source)
            o["hash"] = .string(e.hash)
            o["scale"] = .number(e.scale)
            o["created"] = .number(e.created)
            arr.append(.object(o))
        }
        var root = JObject()
        root["version"] = .number(1)
        root["items"] = .array(arr)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try? Data((JSON.object(root).serialized() + "\n").utf8).write(to: indexURL, options: .atomic)
        NotificationCenter.default.post(name: PinLibrary.didChange, object: nil)
    }

    static func sha256(_ d: Data) -> String {
        var out = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
        d.withUnsafeBytes { _ = CC_SHA256($0.baseAddress, CC_LONG(d.count), &out) }
        return out.map { String(format: "%02x", $0) }.joined()
    }

    func add(image: CGImage, scale: CGFloat, source: String) {
        if !loaded { load() }
        guard persistent, let png = Export.pngData(image, scale: scale) else { return }
        let h = PinLibrary.sha256(png)
        if let i = entries.firstIndex(where: { $0.hash == h }) {
            let e = entries.remove(at: i)
            entries.insert(e, at: 0)
            save()
            return
        }
        let now = Date().timeIntervalSince1970
        var name = "\(Int64(now * 1000)).png"
        while FileManager.default.fileExists(atPath: dir.appendingPathComponent(name).path) { name = "\(Int64(now * 1000) + Int64.random(in: 1...999)).png" }
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            try png.write(to: dir.appendingPathComponent(name), options: .atomic)
        } catch { return }
        entries.insert(Entry(file: name, source: source, hash: h, scale: Double(scale), created: now), at: 0)
        let maxN = max(10, Settings.shared.int("pin.libraryMax"))
        while entries.count > maxN {
            let old = entries.removeLast()
            try? FileManager.default.removeItem(at: dir.appendingPathComponent(old.file))
            thumbs[old.file] = nil
        }
        save()
    }

    func remove(_ file: String) {
        entries.removeAll { $0.file == file }
        thumbs[file] = nil
        if persistent { try? FileManager.default.removeItem(at: dir.appendingPathComponent(file)) }
        save()
    }

    func clear() {
        for e in entries where persistent { try? FileManager.default.removeItem(at: dir.appendingPathComponent(e.file)) }
        entries.removeAll()
        thumbs.removeAll()
        save()
    }

    func image(_ e: Entry) -> CGImage? {
        if !persistent { return thumbs[e.file] }
        guard let src = CGImageSourceCreateWithURL(dir.appendingPathComponent(e.file) as CFURL, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(src, 0, nil)
    }

    func thumbnail(_ e: Entry) -> CGImage? {
        if let t = thumbs[e.file] { return t }
        guard let src = CGImageSourceCreateWithURL(dir.appendingPathComponent(e.file) as CFURL, nil) else { return nil }
        let opts: [CFString: Any] = [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: 320, kCGImageSourceCreateThumbnailWithTransform: true]
        let t = CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary)
        thumbs[e.file] = t
        return t
    }

    func repin(_ e: Entry) {
        guard let img = image(e) else { return }
        PinManager.shared.pin(image: img, scale: CGFloat(e.scale), frame: nil, source: "library", addToLibrary: false)
    }
}

/// 贴图上标注：在贴图位置开一个 lock_selection 会话，工具栏贴在贴图下方，完成后替换贴图内容。
final class PinAnnotator: CaptureViewDelegate {
    weak var pin: PinWindow?
    var onFinish: ((Export.Result?) -> Void)?
    private var panel: CapturePanel?
    private var view: CaptureView?

    init(pin: PinWindow) { self.pin = pin }

    func begin() {
        guard let pin = pin else { return }
        let r = pin.imageFrame
        let tb = CaptureToolbar.size(mode: .pin)
        let extra: CGFloat = 170
        var area = r.union(CGRect(x: r.maxX - tb.width - 12, y: r.minY - extra, width: tb.width + 12, height: extra))
        if let vis = ScreenCapture.screen(at: CGPoint(x: r.midX, y: r.midY))?.frame { area = area.intersection(vis) }
        area = area.integral
        let scale = CGFloat(pin.image.width) / r.width
        let sel = CGRect(x: r.minX - area.minX, y: area.maxY - r.maxY, width: r.width, height: r.height)
        guard let fz = FrozenImage.make(width: Int((area.width * scale).rounded()), height: Int((area.height * scale).rounded()), scale: scale, space: pin.image.colorSpace, draw: { ctx in
            ctx.interpolationQuality = .high
            Draw.image(ctx, pin.image, in: CGRect(x: sel.minX * scale, y: sel.minY * scale, width: sel.width * scale, height: sel.height * scale))
        }) else { onFinish?(nil); return }
        var opts = Session.Options()
        opts.lockSelection = true
        opts.initialSelection = sel
        opts.detectWindows = false
        guard let s = Session(frozen: fz, size: area.size, options: opts) else { onFinish?(nil); return }
        let p = CapturePanel(frame: area, opaque: false)
        p.level = NSWindow.Level(rawValue: NSWindow.Level.floating.rawValue + 1)
        let v = CaptureView(frame: CGRect(origin: .zero, size: area.size), session: s, mode: .pinAnnotate, scale: scale)
        v.delegate = self
        p.contentView = v
        p.setFrame(area, display: false)
        panel = p
        view = v
        pin.setButtonsVisible(false)
        p.orderFrontRegardless()
        p.makeKey()
        p.makeFirstResponder(v)
    }

    private func end(_ result: Export.Result?) {
        view?.endTextEditing()
        view?.hidePopover()
        panel?.orderOut(nil)
        let p = panel, v = view
        panel = nil
        view = nil
        // 等当前事件处理完再释放截图层视图与会话
        DispatchQueue.main.async {
            _ = v
            p?.contentView = nil
            p?.close()
        }
        onFinish?(result)
    }

    func captureView(_ v: CaptureView, perform action: ls_action) {
        switch action {
        case LS_ACTION_CANCEL: end(nil)
        case LS_ACTION_DONE, LS_ACTION_DOUBLE_CLICK, LS_ACTION_PIN:
            end(Export.render(session: v.session, applyResolution: false))
        case LS_ACTION_COPY:
            let r = Export.render(session: v.session, applyResolution: false)
            if let r = r { Export.copy(r); Toast.show(L("notify.copied")) }
            end(r)
        case LS_ACTION_SAVE:
            let r = Export.render(session: v.session, applyResolution: false)
            end(r)
            if let r = r, let url = Export.save(r) { Toast.show(L("notify.saved")) { NSWorkspace.shared.activateFileViewerSelecting([url]) } }
        default: break
        }
    }

    func captureViewDidBeginSelection(_ v: CaptureView) {}
    func captureViewMouseEntered(_ v: CaptureView) {}
    func captureViewDoubleModifier(_ v: CaptureView) {}
}
