import AppKit

/// 设置：内置默认值 + 用户文件深合并；写盘只写完整对象并原子替换；变更后发通知。
final class Settings {
    static let shared = Settings()
    static let didChange = Notification.Name("cn.lemonit.shot.settingsDidChange")

    /// 自测或只读模式下不读写用户文件。
    var persistent = true

    private(set) var defaults: JSON
    private(set) var root: JSON

    let directory: URL = {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        return base.appendingPathComponent("LemonShot", isDirectory: true)
    }()

    var fileURL: URL { directory.appendingPathComponent("settings.json") }

    private init() {
        defaults = Resources.json("settings.defaults.json")
        root = defaults
    }

    func load() {
        defaults = Resources.json("settings.defaults.json")
        root = defaults
        guard persistent else { return }
        if let user = JSON.parse(file: fileURL) {
            root = JSON.deepMerge(defaults, user)
        }
    }

    // MARK: 读

    func value(_ key: String) -> JSON? { root.at(key) ?? defaults.at(key) }
    func bool(_ key: String) -> Bool { value(key)?.bool ?? false }
    func double(_ key: String) -> Double { value(key)?.double ?? 0 }
    func int(_ key: String) -> Int { value(key)?.int ?? 0 }
    func string(_ key: String) -> String { value(key)?.string ?? "" }
    func cgFloat(_ key: String) -> CGFloat { CGFloat(double(key)) }

    // MARK: 写

    func set(_ key: String, _ v: JSON) {
        if root.at(key) == v { return }
        root.set(key, v)
        save()
        NotificationCenter.default.post(name: Settings.didChange, object: self, userInfo: ["key": key])
    }

    func set(_ key: String, _ b: Bool) { set(key, .bool(b)) }
    func set(_ key: String, _ d: Double) { set(key, .number(d)) }
    func set(_ key: String, _ s: String) { set(key, .string(s)) }

    func resetAll() {
        root = defaults
        save()
        NotificationCenter.default.post(name: Settings.didChange, object: self, userInfo: ["key": ""])
    }

    /// 导入：在默认值上深合并导入文件。
    func importFile(_ url: URL) -> Bool {
        guard let v = JSON.parse(file: url), v.object != nil else { return false }
        root = JSON.deepMerge(defaults, v)
        save()
        NotificationCenter.default.post(name: Settings.didChange, object: self, userInfo: ["key": ""])
        return true
    }

    func exportFile(_ url: URL) -> Bool {
        let data = Data((root.serialized() + "\n").utf8)
        return (try? data.write(to: url, options: .atomic)) != nil
    }

    private func save() {
        guard persistent else { return }
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let data = Data((root.serialized() + "\n").utf8)
            // .atomic：先写临时文件再改名替换
            try data.write(to: fileURL, options: .atomic)
        } catch {
            NSLog("LemonShot: 写设置失败 %@", String(describing: error))
        }
    }

    // MARK: 路径占位符

    static func resolvePath(_ raw: String) -> URL {
        var s = raw
        let fm = FileManager.default
        func dir(_ d: FileManager.SearchPathDirectory, _ fallback: String) -> String {
            fm.urls(for: d, in: .userDomainMask).first?.path ?? (NSHomeDirectory() + "/" + fallback)
        }
        s = s.replacingOccurrences(of: "{pictures}", with: dir(.picturesDirectory, "Pictures"))
        s = s.replacingOccurrences(of: "{movies}", with: dir(.moviesDirectory, "Movies"))
        s = s.replacingOccurrences(of: "{music}", with: dir(.musicDirectory, "Music"))
        s = (s as NSString).expandingTildeInPath
        return URL(fileURLWithPath: s, isDirectory: true)
    }

    /// 把真实路径还原成占位符形式（用户在偏好设置里选了系统目录下的文件夹时保持可移植）。
    static func abbreviatePath(_ path: String) -> String {
        let fm = FileManager.default
        let pairs: [(FileManager.SearchPathDirectory, String)] = [(.picturesDirectory, "{pictures}"), (.moviesDirectory, "{movies}"), (.musicDirectory, "{music}")]
        for (d, token) in pairs {
            if let base = fm.urls(for: d, in: .userDomainMask).first?.path, path == base || path.hasPrefix(base + "/") {
                return token + path.dropFirst(base.count)
            }
        }
        return path
    }
}

/// 运行状态（上次区域等），存 state.json。
final class AppState {
    static let shared = AppState()
    private var root: JSON = .object(JObject())
    var persistent = true

    private var fileURL: URL { Settings.shared.directory.appendingPathComponent("state.json") }

    func load() {
        guard persistent, let v = JSON.parse(file: fileURL) else { return }
        root = v
    }

    func value(_ key: String) -> JSON? { root.at(key) }

    func set(_ key: String, _ v: JSON) {
        root.set(key, v)
        guard persistent else { return }
        try? FileManager.default.createDirectory(at: Settings.shared.directory, withIntermediateDirectories: true)
        try? Data((root.serialized() + "\n").utf8).write(to: fileURL, options: .atomic)
    }

    /// 上次截图区域（本屏逻辑坐标），按显示器保存。
    func lastArea(display key: String) -> CGRect? {
        guard let a = root.at("lastArea")?[key]?.array, a.count == 4 else { return nil }
        let v = a.compactMap { $0.double }
        guard v.count == 4, v[2] > 0, v[3] > 0 else { return nil }
        return CGRect(x: v[0], y: v[1], width: v[2], height: v[3])
    }

    func setLastArea(_ r: CGRect, display key: String) {
        set("lastArea.\(key)", .array([.number(Double(r.minX)), .number(Double(r.minY)), .number(Double(r.width)), .number(Double(r.height))]))
        set("lastAreaDisplay", .string(key))
    }
}
