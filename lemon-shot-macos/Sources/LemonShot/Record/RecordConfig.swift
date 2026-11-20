import AppKit
import CoreMedia

/// 录屏设置快照（spec 8）：开始录制时读一次 record.*，录制过程中不再变。
struct RecordConfig {
    var format = "mp4"
    var quality = "high"
    var fps = 30
    var gifFps = 12
    var gifScale = 1.0
    var microphone = true
    var microphoneDevice = ""
    var systemAudio = true
    var showCursor = true
    var showClicks = true
    var showKeys = false
    var camera = false
    var cameraDevice = ""
    var cameraShape = "circle"
    var countdown = 3
    var maxDurationSec = 0
    var afterAction = "reveal"
    var folder = URL(fileURLWithPath: NSHomeDirectory())

    var isGIF: Bool { format == "gif" }
    /// GIF 不带声音。
    var wantsMicrophone: Bool { !isGIF && microphone }
    var wantsSystemAudio: Bool { !isGIF && systemAudio && RecordConfig.systemAudioSupported }
    var hasAudio: Bool { wantsMicrophone || wantsSystemAudio }

    static func load() -> RecordConfig {
        let s = Settings.shared
        var c = RecordConfig()
        c.format = s.string("record.format") == "gif" ? "gif" : "mp4"
        c.quality = s.string("record.quality")
        c.fps = clampFps(s.int("record.fps"))
        c.gifFps = max(1, min(50, s.int("record.gifFps")))
        c.gifScale = max(0.1, min(1, s.double("record.gifScale")))
        c.microphone = s.bool("record.microphone")
        c.microphoneDevice = s.string("record.microphoneDevice")
        c.systemAudio = s.bool("record.systemAudio")
        c.showCursor = s.bool("record.showCursor")
        c.showClicks = s.bool("record.showClicks")
        c.showKeys = s.bool("record.showKeys")
        c.camera = s.bool("record.camera")
        c.cameraDevice = s.string("record.cameraDevice")
        c.cameraShape = s.string("record.cameraShape")
        c.countdown = max(0, min(10, s.int("record.countdown")))
        c.maxDurationSec = max(0, s.int("record.maxDurationSec"))
        c.afterAction = s.string("record.afterAction")
        c.folder = Settings.resolvePath(s.string("record.folder"))
        return c
    }

    static func clampFps(_ v: Int) -> Int {
        [15, 24, 30, 60].min { abs($0 - v) < abs($1 - v) } ?? 30
    }

    /// 系统声音：macOS 13+ 用 SCStream.capturesAudio，更早的系统不支持。
    static var systemAudioSupported: Bool {
        if #available(macOS 13.0, *) { return true }
        return false
    }

    /// 能否把自己的控制条、边框排除在画面外（macOS 12.3+ ScreenCaptureKit）。
    static var canExcludeWindows: Bool {
        if #available(macOS 12.3, *) { return true }
        return false
    }

    // MARK: 尺寸与码率（spec 8.3）

    /// H.264 的宽高必须是偶数。
    static func even(_ v: Double) -> Int { max(2, Int(v.rounded(.down)) & ~1) }

    /// 视频输出像素尺寸：超清 = 原像素，高清 = 长边 ≤ 1920，标清 = 长边 ≤ 1280。
    /// H.264 硬件编码单边上限 4096，原像素超过时等比缩到 4096。
    static func videoSize(points: CGSize, scale: CGFloat, quality: String) -> (Int, Int) {
        let pw = Double(points.width * scale), ph = Double(points.height * scale)
        let long = max(pw, ph)
        var limit: Double
        switch quality {
        case "ultra": limit = 4096
        case "standard": limit = 1280
        default: limit = 1920
        }
        limit = min(limit, 4096)
        let f = long > limit ? limit / long : 1
        return (even(pw * f), even(ph * f))
    }

    /// GIF 尺寸：区域逻辑尺寸（1 倍）× gifScale。
    static func gifSize(points: CGSize, gifScale: Double) -> (Int, Int) {
        let w = max(2, Int((Double(points.width) * gifScale).rounded()))
        let h = max(2, Int((Double(points.height) * gifScale).rounded()))
        return (w, h)
    }

    /// 码率 ≈ 宽 × 高 × 帧率 × 0.1（bit/s）。
    static func bitrate(width: Int, height: Int, fps: Int) -> Int {
        max(500_000, Int(Double(width) * Double(height) * Double(fps) * 0.1))
    }

    /// 文件名 `LemonShot {yyyy-MM-dd HH.mm.ss}.<ext>`，重名加 ` (2)`。
    static func outputURL(folder: URL, ext: String, date: Date) -> URL {
        Export.uniqueURL(folder: folder, base: Export.fileName(template: "LemonShot {yyyy-MM-dd HH.mm.ss}", date: date), ext: ext)
    }

    /// 计时显示：mm:ss，满一小时 h:mm:ss。
    static func clock(_ seconds: Double) -> String {
        let t = max(0, Int(seconds.rounded(.down)))
        let h = t / 3600, m = (t / 60) % 60, s = t % 60
        return h > 0 ? String(format: "%d:%02d:%02d", h, m, s) : String(format: "%02d:%02d", m, s)
    }
}

/// 主机时钟（与 SCStream、CGDisplayStream 的时间戳同一时基），单位秒。
enum HostClock {
    static func now() -> Double { CMClockGetTime(CMClockGetHostTimeClock()).seconds }
}
