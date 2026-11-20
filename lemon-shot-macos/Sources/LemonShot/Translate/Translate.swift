import AppKit
import LemonShotCore

/// 截图翻译（spec 5）的入口。
enum TranslateFeature {
    /// 全局快捷键 / 菜单「截图翻译」：进截图层，选完区域自动识别并翻译。
    static func start() {
        CaptureController.shared.start(.translate)
    }

    /// 截图层工具栏「翻译」：先识别选区文字，再翻译。
    static func translate(image: CGImage, scale: CGFloat, region: CGRect) {
        // 截图层收尾时会切回原来的应用：等它切完再把窗口放到前面
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            TranslateWindowController.translate(image: image)
        }
    }

    /// 文字识别窗口「翻译」。
    static func translate(text: String) {
        TranslateWindowController.translate(text: text)
    }

    /// 偏好设置里「测试」按钮：用所选服务翻译一个词，结果用 toast 提示。
    static func testProvider(_ provider: String) {
        TranslateService.translate(text: "Hello", provider: provider, target: "zh-Hans") { outcome in
            switch outcome {
            case .success: Toast.show(L("translate.testOk"))
            case .needKey: Toast.show(TranslateService.needKeyMessage(provider), icon: "triangle-alert")
            case .failure(let e): Toast.show(L("translate.failed", ["error": e]), icon: "triangle-alert")
            }
        }
    }
}

/// 翻译请求：内核拼装请求与签名、解析响应，这里只负责用 URLSession 发 HTTP（超时 15 秒）。
enum TranslateService {
    enum Outcome {
        case success(String)
        case needKey
        case failure(String)
    }

    private static let session: URLSession = {
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = 15
        cfg.timeoutIntervalForResource = 30
        cfg.requestCachePolicy = .reloadIgnoringLocalCacheData
        return URLSession(configuration: cfg)
    }()

    static func needKeyMessage(_ provider: String) -> String {
        L("translate.needKey", ["provider": L("translate.provider.\(provider)")])
    }

    /// from 固定为自动识别；target 为 i18n 语言代码或 auto。回调在主线程。
    static func translate(text: String, provider: String, target: String, completion: @escaping (Outcome) -> Void) {
        let pid = ls_translate_provider_from_name(provider)
        guard pid >= 0 else { completion(.failure(provider)); return }
        let config = (Settings.shared.value("translate") ?? .object(JObject())).serialized()
        guard let tr = ls_translate_begin(pid, config, text, "auto", target, Int64(Date().timeIntervalSince1970)) else {
            completion(.failure("translator"))
            return
        }
        step(tr, lastNetworkError: nil, completion: completion)
    }

    /// 内核状态机走一步：要发请求就发，完成或出错就收尾。只在主线程调用（内核对象不跨线程）。
    private static func step(_ tr: OpaquePointer, lastNetworkError: String?, completion: @escaping (Outcome) -> Void) {
        var req = ls_http_request()
        let st = ls_translate_request(tr, &req)
        if st == Int32(LS_TR_SEND) {
            guard let urlStr = req.url.map({ String(cString: $0) }), let url = URL(string: urlStr) else {
                ls_translate_end(tr)
                completion(.failure("URL"))
                return
            }
            var r = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 15)
            r.httpMethod = req.method.map { String(cString: $0) } ?? "GET"
            if let h = req.headers {
                for line in String(cString: h).split(separator: "\n") {
                    guard let colon = line.firstIndex(of: ":") else { continue }
                    let name = line[..<colon].trimmingCharacters(in: .whitespaces)
                    let value = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
                    if !name.isEmpty { r.setValue(value, forHTTPHeaderField: name) }
                }
            }
            if let b = req.body, req.body_size > 0 {
                r.httpBody = Data(bytes: b, count: Int(req.body_size))
            }
            session.dataTask(with: r) { data, resp, err in
                let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
                let body = data ?? Data()
                let netErr = err?.localizedDescription
                DispatchQueue.main.async {
                    _ = body.withUnsafeBytes { raw in
                        ls_translate_response(tr, Int32(status), raw.bindMemory(to: CChar.self).baseAddress, Int32(body.count))
                    }
                    step(tr, lastNetworkError: netErr ?? lastNetworkError, completion: completion)
                }
            }.resume()
            return
        }
        let result = String(cString: ls_translate_result(tr))
        ls_translate_end(tr)
        if st == Int32(LS_TR_DONE) {
            completion(.success(result))
        } else if result == "NEED_KEY" {
            completion(.needKey)
        } else if result == "HTTP 0", let e = lastNetworkError {
            // 没有拿到任何响应（断网、超时）：显示系统给的原因
            completion(.failure(e))
        } else {
            completion(.failure(result.isEmpty ? L("common.error") : result))
        }
    }
}
