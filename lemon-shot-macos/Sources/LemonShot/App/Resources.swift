import Foundation

/// 共用资源定位：正式包里在 Contents/Resources/shared；开发运行（swift run、Xcode）时回溯到源码树的 lemon-shot-core/resources。
enum Resources {
    static let root: URL = {
        if let env = ProcessInfo.processInfo.environment["LEMONSHOT_RESOURCES"], !env.isEmpty {
            return URL(fileURLWithPath: env, isDirectory: true)
        }
        if let res = Bundle.main.resourceURL {
            let shared = res.appendingPathComponent("shared", isDirectory: true)
            if FileManager.default.fileExists(atPath: shared.appendingPathComponent("tokens.json").path) { return shared }
        }
        // 本文件位于 lemon-shot-macos/Sources/LemonShot/App/，往上四级是仓库根目录
        var url = URL(fileURLWithPath: #filePath)
        for _ in 0..<4 { url.deleteLastPathComponent() }
        return url.deletingLastPathComponent().appendingPathComponent("lemon-shot-core/resources", isDirectory: true)
    }()

    static func url(_ relative: String) -> URL { root.appendingPathComponent(relative) }

    static func json(_ relative: String) -> JSON {
        guard let v = JSON.parse(file: url(relative)) else {
            NSLog("Lemon Shot: 读取资源失败 %@", url(relative).path)
            return .object(JObject())
        }
        return v
    }
}
