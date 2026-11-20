import AppKit
import AVFoundation
import IOKit.hid

/// 权限（spec 14）：状态检测与「去授权」。
enum Permissions {
    enum Kind: String, CaseIterable {
        case screen, accessibility, input, microphone, camera, systemAudio

        var titleKey: String { "perm.\(rawValue)" }
        var descKey: String { "perm.\(rawValue).desc" }
        var icon: String {
            switch self {
            case .screen: return "monitor"
            case .accessibility: return "hand"
            case .input: return "keyboard"
            case .microphone: return "mic"
            case .camera: return "webcam"
            case .systemAudio: return "volume-2"
            }
        }
        var settingsAnchor: String {
            switch self {
            case .screen: return "Privacy_ScreenCapture"
            case .accessibility: return "Privacy_Accessibility"
            case .input: return "Privacy_ListenEvent"
            case .microphone: return "Privacy_Microphone"
            case .camera: return "Privacy_Camera"
            case .systemAudio: return "Privacy_AudioCapture"
            }
        }
    }

    /// 自测用固定状态。
    static var fake: [Kind: Bool]?

    static func granted(_ k: Kind) -> Bool {
        if let f = fake { return f[k] ?? false }
        switch k {
        case .screen: return ScreenCapture.hasPermission
        case .accessibility: return AXIsProcessTrusted()
        case .input:
            if #available(macOS 10.15, *) { return IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted }
            return true
        case .microphone: return AVCaptureDevice.authorizationStatus(for: .audio) == .authorized
        case .camera: return AVCaptureDevice.authorizationStatus(for: .video) == .authorized
        case .systemAudio:
            // 系统声音随屏幕录制权限一起授予（SCStream.capturesAudio）
            return ScreenCapture.hasPermission
        }
    }

    static func openSettings(_ k: Kind) {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(k.settingsAnchor)") {
            NSWorkspace.shared.open(url)
        }
    }

    /// 「去授权」：能弹系统请求的先请求一次，再打开系统设置对应页。
    static func request(_ k: Kind, completion: @escaping () -> Void) {
        switch k {
        case .screen, .systemAudio:
            if #available(macOS 10.15, *) { _ = CGRequestScreenCaptureAccess() }
            openSettings(k)
            completion()
        case .accessibility:
            let opts = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
            _ = AXIsProcessTrustedWithOptions(opts)
            completion()
        case .input:
            if #available(macOS 10.15, *) { _ = IOHIDRequestAccess(kIOHIDRequestTypeListenEvent) }
            openSettings(k)
            completion()
        case .microphone, .camera:
            let type: AVMediaType = k == .microphone ? .audio : .video
            if AVCaptureDevice.authorizationStatus(for: type) == .notDetermined {
                AVCaptureDevice.requestAccess(for: type) { _ in DispatchQueue.main.async { completion() } }
            } else {
                openSettings(k)
                completion()
            }
        }
    }
}

/// 录屏/录音设备列表（偏好设置下拉用）。
enum Devices {
    static var fake: [(String, String)]?

    static func list(audio: Bool) -> [(String, String)] {
        if let f = fake { return f }
        let type: AVMediaType = audio ? .audio : .video
        if #available(macOS 10.15, *) {
            var types: [AVCaptureDevice.DeviceType] = audio ? [.builtInMicrophone] : [.builtInWideAngleCamera]
            if #available(macOS 14.0, *) { types += audio ? [.microphone] : [.external] } else { types += [.externalUnknown] }
            let s = AVCaptureDevice.DiscoverySession(deviceTypes: types, mediaType: type, position: .unspecified)
            return s.devices.map { ($0.uniqueID, $0.localizedName) }
        }
        return []
    }
}
