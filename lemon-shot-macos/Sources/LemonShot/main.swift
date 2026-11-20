import AppKit

// 入口：--selftest 离屏渲染界面状态后退出；否则作为菜单栏应用运行。
let args = CommandLine.arguments
if let i = args.firstIndex(of: "--selftest") {
    let dir = i + 1 < args.count ? args[i + 1] : "selftest"
    exit(SelfTest.run(outputDir: dir, args: args))
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
