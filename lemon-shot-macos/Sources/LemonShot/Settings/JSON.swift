import Foundation

/// 保序 JSON 值。设置文件、文案、令牌、图标都用它读；写设置时保持默认值里的键顺序，未知键追加在后面。
enum JSON: Equatable {
    case null
    case bool(Bool)
    case number(Double)
    case string(String)
    case array([JSON])
    case object(JObject)

    // MARK: 读取

    var bool: Bool? { if case .bool(let b) = self { return b }; if case .number(let n) = self { return n != 0 }; return nil }
    var double: Double? { if case .number(let n) = self { return n }; return nil }
    var int: Int? { if case .number(let n) = self { return Int(n.rounded()) }; return nil }
    var string: String? { if case .string(let s) = self { return s }; return nil }
    var array: [JSON]? { if case .array(let a) = self { return a }; return nil }
    var object: JObject? { if case .object(let o) = self { return o }; return nil }
    var isNull: Bool { if case .null = self { return true }; return false }

    subscript(key: String) -> JSON? {
        if case .object(let o) = self { return o[key] }
        return nil
    }

    subscript(index: Int) -> JSON? {
        if case .array(let a) = self, index >= 0, index < a.count { return a[index] }
        return nil
    }

    /// 按「a.b.c」路径取值。
    func at(_ path: String) -> JSON? {
        var cur: JSON? = self
        for part in path.split(separator: ".") {
            guard let c = cur else { return nil }
            if let idx = Int(part), case .array = c { cur = c[idx] } else { cur = c[String(part)] }
        }
        return cur
    }

    /// 按路径写值，中间缺的对象自动补上。
    mutating func set(_ path: String, _ value: JSON) {
        let parts = path.split(separator: ".").map(String.init)
        guard !parts.isEmpty else { self = value; return }
        JSON.setParts(&self, parts[...], value)
    }

    private static func setParts(_ node: inout JSON, _ parts: ArraySlice<String>, _ value: JSON) {
        guard let first = parts.first else { node = value; return }
        if case .array(var arr) = node, let idx = Int(first), idx >= 0 {
            while arr.count <= idx { arr.append(.null) }
            var child = arr[idx]
            setParts(&child, parts.dropFirst(), value)
            arr[idx] = child
            node = .array(arr)
            return
        }
        var obj = node.object ?? JObject()
        var child = obj[first] ?? .null
        setParts(&child, parts.dropFirst(), value)
        obj[first] = child
        node = .object(obj)
    }

    // MARK: 解析

    static func parse(_ data: Data) -> JSON? {
        var p = Parser(bytes: [UInt8](data))
        p.skipWS()
        guard let v = p.value() else { return nil }
        p.skipWS()
        return p.i == p.bytes.count ? v : nil
    }

    static func parse(file url: URL) -> JSON? {
        guard let d = try? Data(contentsOf: url) else { return nil }
        return parse(d)
    }

    private struct Parser {
        let bytes: [UInt8]
        var i = 0

        mutating func skipWS() {
            while i < bytes.count, bytes[i] == 0x20 || bytes[i] == 0x0A || bytes[i] == 0x0D || bytes[i] == 0x09 { i += 1 }
        }

        mutating func value() -> JSON? {
            skipWS()
            guard i < bytes.count else { return nil }
            switch bytes[i] {
            case UInt8(ascii: "{"): return object()
            case UInt8(ascii: "["): return array()
            case UInt8(ascii: "\""): return string().map { .string($0) }
            case UInt8(ascii: "t"): return literal("true", .bool(true))
            case UInt8(ascii: "f"): return literal("false", .bool(false))
            case UInt8(ascii: "n"): return literal("null", .null)
            default: return number()
            }
        }

        mutating func literal(_ word: String, _ v: JSON) -> JSON? {
            let w = Array(word.utf8)
            guard i + w.count <= bytes.count, Array(bytes[i..<i + w.count]) == w else { return nil }
            i += w.count
            return v
        }

        mutating func number() -> JSON? {
            let start = i
            while i < bytes.count {
                let c = bytes[i]
                if (c >= 0x30 && c <= 0x39) || c == UInt8(ascii: "-") || c == UInt8(ascii: "+") || c == UInt8(ascii: ".") || c == UInt8(ascii: "e") || c == UInt8(ascii: "E") { i += 1 } else { break }
            }
            guard i > start, let s = String(bytes: bytes[start..<i], encoding: .ascii), let d = Double(s) else { return nil }
            return .number(d)
        }

        mutating func string() -> String? {
            guard bytes[i] == UInt8(ascii: "\"") else { return nil }
            i += 1
            var out = [UInt8]()
            while i < bytes.count {
                let c = bytes[i]
                if c == UInt8(ascii: "\"") { i += 1; return String(decoding: out, as: UTF8.self) }
                if c == UInt8(ascii: "\\") {
                    i += 1
                    guard i < bytes.count else { return nil }
                    let e = bytes[i]
                    i += 1
                    switch e {
                    case UInt8(ascii: "n"): out.append(0x0A)
                    case UInt8(ascii: "t"): out.append(0x09)
                    case UInt8(ascii: "r"): out.append(0x0D)
                    case UInt8(ascii: "b"): out.append(0x08)
                    case UInt8(ascii: "f"): out.append(0x0C)
                    case UInt8(ascii: "u"):
                        guard var cp = hex4() else { return nil }
                        if cp >= 0xD800 && cp < 0xDC00, i + 1 < bytes.count, bytes[i] == UInt8(ascii: "\\"), bytes[i + 1] == UInt8(ascii: "u") {
                            i += 2
                            guard let lo = hex4() else { return nil }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                        }
                        let scalar = Unicode.Scalar(cp) ?? "?"
                        out.append(contentsOf: Array(String(Character(scalar)).utf8))
                    default: out.append(e)
                    }
                    continue
                }
                out.append(c)
                i += 1
            }
            return nil
        }

        mutating func hex4() -> UInt32? {
            guard i + 4 <= bytes.count, let s = String(bytes: bytes[i..<i + 4], encoding: .ascii), let v = UInt32(s, radix: 16) else { return nil }
            i += 4
            return v
        }

        mutating func array() -> JSON? {
            i += 1
            var arr = [JSON]()
            skipWS()
            if i < bytes.count, bytes[i] == UInt8(ascii: "]") { i += 1; return .array(arr) }
            while true {
                guard let v = value() else { return nil }
                arr.append(v)
                skipWS()
                guard i < bytes.count else { return nil }
                if bytes[i] == UInt8(ascii: ",") { i += 1; continue }
                if bytes[i] == UInt8(ascii: "]") { i += 1; return .array(arr) }
                return nil
            }
        }

        mutating func object() -> JSON? {
            i += 1
            var obj = JObject()
            skipWS()
            if i < bytes.count, bytes[i] == UInt8(ascii: "}") { i += 1; return .object(obj) }
            while true {
                skipWS()
                guard i < bytes.count, let k = string() else { return nil }
                skipWS()
                guard i < bytes.count, bytes[i] == UInt8(ascii: ":") else { return nil }
                i += 1
                guard let v = value() else { return nil }
                obj[k] = v
                skipWS()
                guard i < bytes.count else { return nil }
                if bytes[i] == UInt8(ascii: ",") { i += 1; continue }
                if bytes[i] == UInt8(ascii: "}") { i += 1; return .object(obj) }
                return nil
            }
        }
    }

    // MARK: 输出

    func serialized(indent: Int = 0) -> String {
        var out = ""
        write(&out, indent)
        return out
    }

    private func write(_ out: inout String, _ level: Int) {
        let pad = String(repeating: "  ", count: level + 1)
        let padEnd = String(repeating: "  ", count: level)
        switch self {
        case .null: out += "null"
        case .bool(let b): out += b ? "true" : "false"
        case .number(let n):
            if n.isFinite, n == n.rounded(), abs(n) < 1e15 { out += String(Int64(n)) } else if n.isFinite { out += String(n) } else { out += "0" }
        case .string(let s): JSON.writeString(&out, s)
        case .array(let a):
            if a.isEmpty { out += "[]"; return }
            let simple = a.allSatisfy { if case .object = $0 { return false }; if case .array = $0 { return false }; return true }
            if simple {
                out += "["
                for (i, v) in a.enumerated() { if i > 0 { out += ", " }; v.write(&out, level + 1) }
                out += "]"
            } else {
                out += "[\n"
                for (i, v) in a.enumerated() {
                    out += pad
                    v.write(&out, level + 1)
                    out += i < a.count - 1 ? ",\n" : "\n"
                }
                out += padEnd + "]"
            }
        case .object(let o):
            if o.keys.isEmpty { out += "{}"; return }
            out += "{\n"
            for (i, k) in o.keys.enumerated() {
                out += pad
                JSON.writeString(&out, k)
                out += ": "
                o.dict[k]!.write(&out, level + 1)
                out += i < o.keys.count - 1 ? ",\n" : "\n"
            }
            out += padEnd + "}"
        }
    }

    private static func writeString(_ out: inout String, _ s: String) {
        out += "\""
        for u in s.unicodeScalars {
            switch u {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            case "\n": out += "\\n"
            case "\r": out += "\\r"
            case "\t": out += "\\t"
            default:
                if u.value < 0x20 { out += String(format: "\\u%04x", u.value) } else { out.unicodeScalars.append(u) }
            }
        }
        out += "\""
    }

    /// 深合并：base 为默认值，over 逐键覆盖；两边都是对象时递归，未知键追加。
    static func deepMerge(_ base: JSON, _ over: JSON) -> JSON {
        guard case .object(let b) = base, case .object(let o) = over else { return over }
        var r = b
        for k in o.keys {
            let ov = o.dict[k]!
            if let bv = b[k] { r[k] = deepMerge(bv, ov) } else { r[k] = ov }
        }
        return .object(r)
    }
}

/// 保持插入顺序的对象。
struct JObject: Equatable {
    private(set) var keys: [String] = []
    private(set) var dict: [String: JSON] = [:]

    subscript(key: String) -> JSON? {
        get { dict[key] }
        set {
            if let v = newValue {
                if dict[key] == nil { keys.append(key) }
                dict[key] = v
            } else if dict.removeValue(forKey: key) != nil {
                keys.removeAll { $0 == key }
            }
        }
    }
}
