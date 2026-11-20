import Foundation

/// 界面文案。general.language 为 system 时按系统首选语言：任何中文变体用 zh-Hans，其他用 en。
enum I18n {
    static let didChange = Notification.Name("cn.lemonit.shot.languageDidChange")
    private(set) static var lang = "en"
    private static var table: [String: String] = [:]
    private static var fallback: [String: String] = [:]
    /// 自测时强制语言。
    static var forced: String?

    static func resolve(_ setting: String) -> String {
        if let f = forced { return f }
        if setting == "zh-Hans" || setting == "en" { return setting }
        let first = Locale.preferredLanguages.first?.lowercased() ?? "en"
        return first.hasPrefix("zh") ? "zh-Hans" : "en"
    }

    static func load() {
        let want = resolve(Settings.shared.string("general.language"))
        lang = want
        table = strings(want)
        fallback = want == "en" ? [:] : strings("en")
    }

    static func reloadIfNeeded() {
        let want = resolve(Settings.shared.string("general.language"))
        if want != lang {
            load()
            NotificationCenter.default.post(name: didChange, object: nil)
        }
    }

    private static func strings(_ code: String) -> [String: String] {
        var out: [String: String] = [:]
        if let o = Resources.json("i18n/\(code).json").object {
            for k in o.keys { if let s = o[k]?.string { out[k] = s } }
        }
        return out
    }

    static func has(_ key: String) -> Bool { table[key] != nil || fallback[key] != nil }

    static func t(_ key: String, _ args: [String: String] = [:]) -> String {
        var s = table[key] ?? fallback[key] ?? key
        for (k, v) in args { s = s.replacingOccurrences(of: "{\(k)}", with: v) }
        return s
    }
}

func L(_ key: String, _ args: [String: String] = [:]) -> String { I18n.t(key, args) }
