// swift-tools-version:5.9
// LemonShot macOS 版。用 scripts/build-macos.sh 打包成 .app；开发时也可以直接用 Xcode 打开本文件。
import PackageDescription

let package = Package(
    name: "LemonShot",
    platforms: [.macOS(.v11)],
    products: [
        .executable(name: "LemonShot", targets: ["LemonShot"]),
    ],
    targets: [
        // 公共内核：Core 是指向 ../lemon-shot-core 的软链，直接编译其 C++ 源码
        .target(
            name: "LemonShotCore",
            path: "Core",
            exclude: ["resources", "tests", "tools", "CMakeLists.txt", "third_party", "extras", "cmake"],
            sources: ["src"],
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("src"),
                .define("LEMONSHOT_WITH_WEBP"),
                .unsafeFlags(["-fno-exceptions", "-fno-rtti"]),
            ]
        ),
        // WebP 编码：libwebp 编码部分 + extras/webp.cpp（与 scripts/build-macos.sh 一致）
        .target(
            name: "LemonShotWebP",
            path: "Core",
            sources: [
                "extras/webp.cpp",
                "third_party/libwebp/src/enc",
                "third_party/libwebp/src/dsp",
                "third_party/libwebp/src/utils",
                "third_party/libwebp/sharpyuv",
            ],
            publicHeadersPath: "extras",
            cSettings: [
                .headerSearchPath("third_party/libwebp"),
                .headerSearchPath("third_party/libwebp/src"),
                .unsafeFlags(["-w"]),
            ],
            cxxSettings: [
                .headerSearchPath("include"),
                .headerSearchPath("third_party/libwebp"),
                .headerSearchPath("third_party/libwebp/src"),
                .unsafeFlags(["-fno-exceptions", "-fno-rtti"]),
            ]
        ),
        .executableTarget(
            name: "LemonShot",
            dependencies: ["LemonShotCore", "LemonShotWebP"],
            path: "Sources/LemonShot",
            swiftSettings: [
                .unsafeFlags(["-Osize"], .when(configuration: .release)),
            ],
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("Carbon"),
                // ScreenCaptureKit 只在 macOS 12.3+ 存在，弱链接，老系统运行时判断后不用
                .unsafeFlags(["-Xlinker", "-weak_framework", "-Xlinker", "ScreenCaptureKit"]),
            ]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
