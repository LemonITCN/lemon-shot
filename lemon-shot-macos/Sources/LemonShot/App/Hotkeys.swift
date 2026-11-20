import AppKit
import Carbon.HIToolbox
import LemonShotCore

/// 物理键位表（美式键盘）：虚拟键码 ↔ shortcuts.json 记法里的键名。
enum KeyMap {
    static let table: [(UInt16, String)] = [
        (0, "A"), (11, "B"), (8, "C"), (2, "D"), (14, "E"), (3, "F"), (5, "G"), (4, "H"), (34, "I"), (38, "J"), (40, "K"), (37, "L"), (46, "M"),
        (45, "N"), (31, "O"), (35, "P"), (12, "Q"), (15, "R"), (1, "S"), (17, "T"), (32, "U"), (9, "V"), (13, "W"), (7, "X"), (16, "Y"), (6, "Z"),
        (29, "0"), (18, "1"), (19, "2"), (20, "3"), (21, "4"), (23, "5"), (22, "6"), (26, "7"), (28, "8"), (25, "9"),
        (122, "F1"), (120, "F2"), (99, "F3"), (118, "F4"), (96, "F5"), (97, "F6"), (98, "F7"), (100, "F8"), (101, "F9"), (109, "F10"),
        (103, "F11"), (111, "F12"), (105, "F13"), (107, "F14"), (113, "F15"), (106, "F16"), (64, "F17"), (79, "F18"), (80, "F19"), (90, "F20"),
        (49, "Space"), (36, "Enter"), (76, "Enter"), (53, "Esc"), (48, "Tab"), (126, "Up"), (125, "Down"), (123, "Left"), (124, "Right"),
        (51, "Delete"), (117, "ForwardDelete"),
        (27, "-"), (24, "="), (33, "["), (30, "]"), (41, ";"), (39, "'"), (43, ","), (47, "."), (44, "/"), (42, "\\"), (50, "`"),
        (82, "0"), (83, "1"), (84, "2"), (85, "3"), (86, "4"), (87, "5"), (88, "6"), (89, "7"), (91, "8"), (92, "9"),
    ]

    static func name(forKeyCode code: UInt16) -> String? { table.first { $0.0 == code }?.1 }
    static func keyCode(forName name: String) -> UInt16? {
        let n = name.count == 1 ? name.uppercased() : name
        return table.first { $0.1.caseInsensitiveCompare(n) == .orderedSame }?.0
    }

    /// 截图会话用的键值：字母大写 ASCII、数字、空格 32、特殊键 LS_KEY_*。
    static func sessionKey(forKeyCode code: UInt16) -> Int32? {
        guard let n = name(forKeyCode: code) else { return nil }
        return sessionKey(forName: n)
    }

    static func sessionKey(forName n: String) -> Int32? {
        switch n {
        case "Esc": return Int32(LS_KEY_ESCAPE)
        case "Enter": return Int32(LS_KEY_ENTER)
        case "Tab": return Int32(LS_KEY_TAB)
        case "Delete": return Int32(LS_KEY_BACKSPACE)
        case "ForwardDelete": return Int32(LS_KEY_DELETE)
        case "Left": return Int32(LS_KEY_LEFT)
        case "Right": return Int32(LS_KEY_RIGHT)
        case "Up": return Int32(LS_KEY_UP)
        case "Down": return Int32(LS_KEY_DOWN)
        case "Space": return 32
        default:
            guard n.count == 1, let a = n.uppercased().unicodeScalars.first?.value, a < 128 else { return nil }
            return Int32(a)
        }
    }
}

/// 快捷键字符串（cmd/ctrl/alt/shift + 键）的解析与显示。
enum Shortcut {
    struct Parsed { var mods: NSEvent.ModifierFlags; var key: String }

    static func parse(_ s: String) -> Parsed? {
        let parts = s.split(separator: "+").map { String($0).trimmingCharacters(in: .whitespaces) }
        guard let key = parts.last, !key.isEmpty else { return nil }
        var mods: NSEvent.ModifierFlags = []
        for p in parts.dropLast() {
            switch p.lowercased() {
            case "cmd", "command": mods.insert(.command)
            case "ctrl", "control": mods.insert(.control)
            case "alt", "option": mods.insert(.option)
            case "shift": mods.insert(.shift)
            default: return nil
            }
        }
        return Parsed(mods: mods, key: key)
    }

    static func string(mods: NSEvent.ModifierFlags, key: String) -> String {
        var parts: [String] = []
        if mods.contains(.control) { parts.append("ctrl") }
        if mods.contains(.option) { parts.append("alt") }
        if mods.contains(.shift) { parts.append("shift") }
        if mods.contains(.command) { parts.append("cmd") }
        parts.append(key)
        return parts.joined(separator: "+")
    }

    static func keySymbol(_ key: String) -> String {
        switch key {
        case "Enter": return "↩"
        case "Esc": return "⎋"
        case "Tab": return "⇥"
        case "Space": return "Space"
        case "Delete": return "⌫"
        case "ForwardDelete": return "⌦"
        case "Up": return "↑"
        case "Down": return "↓"
        case "Left": return "←"
        case "Right": return "→"
        default: return key.count == 1 ? key.uppercased() : key
        }
    }

    static func display(mods: NSEvent.ModifierFlags, key: String?) -> String {
        var s = ""
        if mods.contains(.control) { s += "⌃" }
        if mods.contains(.option) { s += "⌥" }
        if mods.contains(.shift) { s += "⇧" }
        if mods.contains(.command) { s += "⌘" }
        if let k = key { s += keySymbol(k) }
        return s
    }

    static func display(_ s: String) -> String {
        guard !s.isEmpty, let p = parse(s) else { return "" }
        return display(mods: p.mods, key: p.key)
    }

    /// NSMenuItem 的快捷键显示。
    static func menuEquivalent(_ s: String) -> (String, NSEvent.ModifierFlags)? {
        guard let p = parse(s) else { return nil }
        let k: String
        switch p.key {
        case "Enter": k = "\r"
        case "Esc": k = "\u{1b}"
        case "Tab": k = "\t"
        case "Space": k = " "
        case "Delete": k = "\u{8}"
        case "Up": k = String(Character(UnicodeScalar(NSUpArrowFunctionKey)!))
        case "Down": k = String(Character(UnicodeScalar(NSDownArrowFunctionKey)!))
        case "Left": k = String(Character(UnicodeScalar(NSLeftArrowFunctionKey)!))
        case "Right": k = String(Character(UnicodeScalar(NSRightArrowFunctionKey)!))
        default:
            if p.key.hasPrefix("F"), let n = Int(p.key.dropFirst()), n >= 1, n <= 20 {
                k = String(Character(UnicodeScalar(NSF1FunctionKey + n - 1)!))
            } else {
                k = p.key.lowercased()
            }
        }
        return (k, p.mods)
    }
}

/// 快捷键表：shortcuts.json 的默认值 + 用户设置（shortcuts.mac.<id>、shortcuts.capture.<id>）。
enum ShortcutTable {
    static let resource = Resources.json("shortcuts.json")

    static var globalIDs: [String] { resource["global"]?.array?.compactMap { $0["id"]?.string } ?? [] }
    static var captureIDs: [String] { resource["capture"]?.array?.compactMap { $0["id"]?.string } ?? [] }

    static func defaultGlobal(_ id: String) -> String {
        resource["global"]?.array?.first { $0["id"]?.string == id }?["mac"]?.string ?? ""
    }

    static func defaultCapture(_ id: String) -> String {
        resource["capture"]?.array?.first { $0["id"]?.string == id }?["key"]?.string ?? ""
    }

    static func global(_ id: String) -> String {
        Settings.shared.value("shortcuts.mac.\(id)")?.string ?? defaultGlobal(id)
    }

    static func capture(_ id: String) -> String {
        Settings.shared.value("shortcuts.capture.\(id)")?.string ?? defaultCapture(id)
    }

    static func setGlobal(_ id: String, _ v: String) { Settings.shared.set("shortcuts.mac.\(id)", .string(v)) }
    static func setCapture(_ id: String, _ v: String) { Settings.shared.set("shortcuts.capture.\(id)", .string(v)) }

    static func resetAll() {
        var root = Settings.shared.value("shortcuts")?.object ?? JObject()
        root["mac"] = nil
        root["capture"] = nil
        Settings.shared.set("shortcuts", .object(root))
    }

    /// 截图时单键 id → 内核绑定。
    static func binding(_ id: String) -> ls_binding? {
        let map: [String: Int32] = [
            "toolRect": 1, "toolEllipse": 2, "toolLine": 3, "toolArrow": 4, "toolPen": 5, "toolMosaic": 6, "toolText": 7,
            "toolStep": 8, "toolHighlight": 9, "toolMagnify": 10,
            "longshot": Int32(LS_BIND_LONGSHOT.rawValue), "ocr": Int32(LS_BIND_OCR.rawValue), "translate": Int32(LS_BIND_TRANSLATE.rawValue),
            "pin": Int32(LS_BIND_PIN.rawValue), "record": Int32(LS_BIND_RECORD.rawValue), "lastArea": Int32(LS_BIND_LAST_AREA.rawValue),
            "fullscreen": Int32(LS_BIND_FULLSCREEN.rawValue), "copyRGB": Int32(LS_BIND_COPY_RGB.rawValue), "copyHEX": Int32(LS_BIND_COPY_HEX.rawValue),
            "copyCustom": Int32(LS_BIND_COPY_CUSTOM.rawValue), "toggleToolbar": Int32(LS_BIND_TOGGLE_TOOLBAR.rawValue),
        ]
        guard let v = map[id] else { return nil }
        return ls_binding(rawValue: UInt32(v))
    }
}

/// 全局快捷键：Carbon RegisterEventHotKey。注册失败的 id 记在 failed 里供偏好设置标红。
final class Hotkeys {
    static let shared = Hotkeys()
    static let didChange = Notification.Name("cn.lemonit.shot.hotkeysDidChange")

    var handler: ((String) -> Void)?
    private(set) var failed: Set<String> = []
    private var refs: [EventHotKeyRef] = []
    private var ids: [String] = []
    private var installed = false
    private var suspendCount = 0

    private func install() {
        guard !installed else { return }
        installed = true
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, event, _ -> OSStatus in
            var hk = EventHotKeyID()
            let st = GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID), nil, MemoryLayout<EventHotKeyID>.size, nil, &hk)
            if st == noErr {
                let idx = Int(hk.id)
                DispatchQueue.main.async { Hotkeys.shared.fire(idx) }
            }
            return noErr
        }, 1, &spec, nil, nil)
    }

    private func fire(_ idx: Int) {
        guard idx >= 0, idx < ids.count else { return }
        handler?(ids[idx])
    }

    private func unregisterAll() {
        for r in refs { UnregisterEventHotKey(r) }
        refs.removeAll()
        ids.removeAll()
    }

    /// 按当前设置重新注册全部全局快捷键。返回失败的 id。
    @discardableResult
    func registerAll() -> Set<String> {
        install()
        unregisterAll()
        guard suspendCount == 0 else { return failed }
        var fails: Set<String> = []
        for id in ShortcutTable.globalIDs {
            let s = ShortcutTable.global(id)
            guard !s.isEmpty, let p = Shortcut.parse(s), let code = KeyMap.keyCode(forName: p.key) else { continue }
            var mods: UInt32 = 0
            if p.mods.contains(.command) { mods |= UInt32(cmdKey) }
            if p.mods.contains(.option) { mods |= UInt32(optionKey) }
            if p.mods.contains(.control) { mods |= UInt32(controlKey) }
            if p.mods.contains(.shift) { mods |= UInt32(shiftKey) }
            var ref: EventHotKeyRef?
            let hid = EventHotKeyID(signature: OSType(0x4C53_484B), id: UInt32(ids.count)) // 'LSHK'
            let st = RegisterEventHotKey(UInt32(code), mods, hid, GetApplicationEventTarget(), 0, &ref)
            if st == noErr, let r = ref {
                refs.append(r)
                ids.append(id)
            } else {
                fails.insert(id)
            }
        }
        failed = fails
        NotificationCenter.default.post(name: Hotkeys.didChange, object: nil)
        return fails
    }

    /// 录入新快捷键时暂停，避免按下的组合直接触发功能。
    func suspend() {
        suspendCount += 1
        if suspendCount == 1 { unregisterAll() }
    }

    func resume() {
        suspendCount = max(0, suspendCount - 1)
        if suspendCount == 0 { registerAll() }
    }
}
