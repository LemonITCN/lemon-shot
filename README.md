# LemonShot

小而精的截图录屏工具，Windows 与 macOS 体验一致，永久免费。

截图、标注、贴图、长截图、带壳截图、取色、文字识别、截图翻译、录屏（同时录麦克风和系统声音）、录音。

## 支持的系统

| 平台 | 版本 | 架构 |
|---|---|---|
| macOS | 11 Big Sur 及以上 | Apple 芯片、Intel（通用包） |
| Windows | 7 SP1、8、8.1、10、11，Server 2008 R2 及以上 | x86、x64；ARM64（Windows 10/11） |

## 目录

| 目录 | 内容 |
|---|---|
| `lemon-shot-macos/` | macOS 版，Swift + AppKit |
| `lemon-shot-windows/` | Windows 版，C++ + Win32 + Direct2D，单个 exe |
| `lemon-shot-core/` | 两端共用的 C++ 内核（截图选区与标注编辑、长截图拼接、图像处理、GIF 编码、颜色）和共用资源（设置默认值、快捷键、中英文文案、界面令牌、图标） |
| `docs/` | 功能与界面契约：两端按同一份契约实现，改功能先改契约 |
| `scripts/` | 构建脚本 |

## 构建

两端都在 Mac 上构建。

```bash
scripts/build-macos.sh
```

产物 `dist/macos/LemonShot.app`。设置 `LEMONSHOT_SIGN_IDENTITY` 用开发者证书签名，否则临时签名。开发时也可以用 Xcode 打开 `lemon-shot-macos/Package.swift`。

```bash
scripts/build-windows.sh
```

产物 `dist/windows/LemonShot-x86.exe`、`LemonShot-x64.exe`、`LemonShot-arm64.exe`。首次运行会自动下载 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 交叉编译工具链（约 210 MB，缓存在 `~/Library/Caches/LemonShot/build-tools`）。x86/x64 版链接系统自带的 msvcrt，Windows 7 不需要额外安装运行库。

```bash
scripts/build-core-tests.sh
```

编译并运行内核单元测试。

## 自动构建与发布

每次推送到 `main` 或提交 PR，[GitHub Actions](.github/workflows/build.yml) 会：

- 构建 macOS 通用包（DMG、zip）和 Windows x86 / x64 / ARM64 三个 exe，并检查 exe 不调用 Windows 7 没有的系统函数；
- 在 macOS 15、macOS 26（Apple 芯片与 Intel）、Windows Server 2022 / 2025（x64、x86）、Windows 11 ARM 上运行内核单元测试和自测截图，在 Ubuntu（x64、ARM64）上运行内核单元测试；
- 把两端自测截图并排生成对照页（构建产物 `selftest-compare`）。

推送 `v` 开头的标签（如 `v0.1.0`）时，自动创建 Release 并上传全部安装包。

在仓库 Secrets 里配置 `MACOS_CERTIFICATE_P12`、`MACOS_CERTIFICATE_PASSWORD`、`MACOS_SIGN_IDENTITY` 后用开发者证书签名，再配置 `APPLE_NOTARY_KEY_P8`、`APPLE_NOTARY_KEY_ID`、`APPLE_NOTARY_ISSUER_ID` 后自动公证；不配置时为临时签名，从网上下载后需要在「隐私与安全性」里允许打开。

## 自测截图

两端都支持离屏渲染全部界面状态，用来对照两端界面是否一致：

```bash
"dist/macos/LemonShot.app/Contents/MacOS/LemonShot" --selftest out/mac
```

```bash
LemonShot-x64.exe --selftest out\win
```

## 许可

MIT。第三方组件见 `lemon-shot-core/resources/ICONS-LICENSE` 与 `lemon-shot-core/third_party/`。
