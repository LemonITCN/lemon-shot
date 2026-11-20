# Lemon Shot 功能与界面契约

两端（`lemon-shot-macos` Swift / `lemon-shot-windows` C++）都按这份文档实现，界面、交互、默认值逐项一致。改功能先改这里，再改两端。

共用资源都在 `lemon-shot-core/resources/`：

| 文件 | 内容 |
|---|---|
| `settings.defaults.json` | 全部设置项与默认值。两端设置文件格式完全相同，可互相导入 |
| `shortcuts.json` | 全局快捷键（分平台默认值）、截图时单键 |
| `i18n/zh-Hans.json`、`i18n/en.json` | 全部界面文案。界面上不许出现写死的文字 |
| `tokens.json` | 颜色（浅色/深色/截图层）、尺寸、圆角、字体 |
| `icons.json` | 全部图标的 SVG 路径（24 网格、描边 2、圆头圆角），用 `ls_svg_path_parse` 解析后原生描边 |

公共逻辑在 `lemon-shot-core`（C++17，C 接口见 `include/lemonshot.h`）。截图选区和标注编辑的状态机、标注几何、像素处理、长截图拼接、GIF、颜色都由内核做，两端只负责：系统捕获、窗口与事件、按内核给的数据绘制、原生文字输入、编码与文件、系统集成。

---

## 1. 总体

- 名称：所有语言都显示 **Lemon Shot**。包名 `cn.lemonit.shot`。
- 常驻后台：Mac 是菜单栏应用（`LSUIElement`，无 Dock 图标）；Windows 是托盘应用，单实例（命名互斥量 `Local\cn.lemonit.shot`，再次启动时通知已运行的实例打开偏好设置）。
- 设置文件：Mac `~/Library/Application Support/Lemon Shot/settings.json`；Windows `%APPDATA%\Lemon Shot\settings.json`。exe 旁边有 `portable` 文件时改用 exe 旁的 `data\` 目录（Windows 绿色版）。
  - 读：先加载内置默认值，再用用户文件逐键覆盖（深合并），未知键原样保留。写：只写完整对象，原子替换（先写临时文件再改名）。
  - 路径占位符：`{pictures}` `{movies}` `{music}` 分别是系统图片、影片（Windows 为「视频」）、音乐目录。
- 自定义快捷键：只把改过的项写进设置 `shortcuts.mac.<id>`、`shortcuts.win.<id>`（全局，按平台）与 `shortcuts.capture.<id>`（截图时单键），值的记法同 `shortcuts.json`，空串表示清除；没写的用 `shortcuts.json` 的默认值。
- 标注颜色 `annotate.<tool>.color`：数字为调色板序号，字符串 `#RRGGBB` 为自定义颜色。
- 贴图库：设置目录下 `pins/`，每张 `<时间戳>.png` + `library.json`（顺序、来源）。上次区域等运行状态存 `state.json`。
- 语言：`general.language` 为 `system` 时按系统首选语言，中文（任何中文变体）用 `zh-Hans`，其他用 `en`。文案占位符 `{name}` 直接替换。
- 外观：`general.theme` 跟随系统或强制浅色/深色。截图层、标注颜色不随主题变。
- 字体：Mac 用系统字体（中文自动落到苹方）；Windows 依次尝试 Microsoft YaHei UI、Microsoft YaHei、Segoe UI。字号用 `tokens.font.size`，单位逻辑点。
- 图标：只用 `icons.json`。描边宽 = `toolbar.iconStroke`（按 24 网格缩放到实际尺寸后的逻辑宽度），颜色 `icon`，激活 `iconActive`。不用 emoji，不用系统图标。
- 界面原则：标题 + 控件为主，不堆说明文字；需要说明的放一个 `info` 图标，悬停或点击显示气泡。
- 所有自绘控件两端尺寸、颜色、圆角完全按 `tokens.json`。

### 1.1 自测截图（两端必须实现）

`LemonShot --selftest <输出目录> [--scale 2] [--theme light|dark] [--lang zh-Hans|en]`：用固定假数据离屏渲染下列界面状态为 PNG（逻辑尺寸 × scale），文件名就是状态名，然后退出，不弹任何窗口、不需要屏幕录制权限。

| 状态名 | 内容 |
|---|---|
| `capture-selecting` | 1280×800 假屏幕（灰白渐变 + 三个假窗口），光标在 (400,300)，悬停高亮第二个窗口，放大镜、左下角提示 |
| `capture-selected` | 选区 (200,150,640,400)，尺寸提示，工具栏，无工具 |
| `capture-annotated` | 同上 + 矩形、椭圆、直线、四种箭头、画笔、荧光笔、像素马赛克、文字三种样式、序号 1-3、局部高亮、放大镜各一个，矩形被选中显示控制点 |
| `popover-<工具>` | 每个工具的样式面板（10 个） |
| `toolbar-hover` | 工具栏某按钮悬停 + 提示气泡 |
| `pin` | 贴图窗（带按钮、圆角、阴影） |
| `pin-library` | 贴图库窗口，6 张假图 |
| `ocr` | 文字识别窗口，含假结果 |
| `translate` | 翻译窗口，含假原文与译文 |
| `longshot` | 长截图进行中（区域边框 + 右侧预览 + 控制按钮） |
| `shell` | 带壳截图预览窗口（自动匹配机型 + 假截图） |
| `record-setup` | 录屏前设置面板（整屏 1280×800 场景，选区 + 设置面板） |
| `record-bar` | 录制中控制条（计时 00:12，带一段区域边框示意） |
| `record-keys` | 按键浮层（⌘ ⇧ 4 / Ctrl Shift 4 各平台自己的符号，画布 520×200） |
| `audio` | 录音面板（录音中 00:07，有电平） |
| `toast` | 「已复制」提示 |
| `prefs-<页>` | 偏好设置每一页（12 页，Windows 的权限页显示「无需授权」） |
| `menu` | 菜单栏/托盘菜单（自绘渲染一份，便于对照） |

对照脚本 `scripts/compare-selftest.sh` 把两端输出并排生成对照页。

### 1.2 两端已知的平台差异（不算不一致）

- 工具窗口（文字识别、翻译、贴图库、长截图结果、带壳预览、偏好设置）：Mac 用透明标题栏的全尺寸内容视图，Windows 用原生标题栏；契约里的窗口尺寸都按内容区算，所以 Windows 的自测图比 Mac 少 44pt 的自绘标题区。文字识别窗口的置顶图钉 Mac 在标题栏右侧，Windows 在工具行最右侧。
- 录屏：Windows 按固定帧率送帧（屏幕静止时重复帧对 H.264 几乎不占空间，GIF 会合并），Mac 静止时每秒补一帧；Windows 托盘图标录制中只显示红点、不显示计时；按住麦克风/摄像头开关 0.5 秒后，Windows 在松开时弹设备菜单。
- 权限：Windows 麦克风被隐私设置禁用时提示 `record.noMicrophone`，没有 Mac 的系统权限页跳转。
- 长截图期间 Windows 临时注册全局 Esc 用于取消（侧面板不抢焦点）。

---

## 2. 截图

### 2.1 进入

按全局快捷键 `capture`（或菜单）：

1. 立即抓取所有屏幕的完整画面（冻结），同时取窗口列表（只取在屏的普通窗口，排除自己的窗口，按 z 序从上到下）。整个过程目标 < 120 ms。
2. 每块屏一个全屏无边框置顶窗口（截图层），各自一个 `ls_session`，`base` 为该屏冻结画面。
3. 光标所在屏获得键盘焦点。只有一块屏可以有选区：某块屏开始拖出选区时，其他屏的会话重置为无悬停、整屏遮罩。
4. 选区只能在一块屏内（与 macOS 系统截图一致）。

捕获策略：
- Mac：macOS 14+ 用 `SCScreenshotManager` 按屏截图（`SCContentFilter(display:excludingWindows:[])`，`showsCursor` 按设置，分辨率 = 屏幕像素）；macOS 11–13 用 `CGWindowListCreateImage(CGRectNull…)`（SDK 已标为不可用，运行时用 `dlsym` 取）。窗口列表用 `CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID)`，取 layer 0 的普通窗口以及菜单栏、程序坞所在区域。
- Windows：GDI `BitBlt(SRCCOPY|CAPTUREBLT)` 按显示器截取（快，不需要授权，Win7 起都可用）。窗口列表用 `EnumWindows`，可见、非最小化、非 cloaked（`DWMWA_CLOAKED`），矩形取 `DWMWA_EXTENDED_FRAME_BOUNDS`（Win7 无 DWM 时退回 `GetWindowRect`），按 z 序。截入光标：`GetCursorInfo` + `DrawIconEx` 画到冻结图上。
- DPI：Windows 进程声明 Per-Monitor V2（Win10 1703+），Win8.1 退回 Per-Monitor，Win7 为系统 DPI。每块屏的 `scale` = 该屏 DPI / 96。Mac `scale` = `backingScaleFactor`。

### 2.2 截图层绘制（每块屏，从下到上）

1. 冻结画面（按逻辑尺寸铺满）。
2. 遮罩 `capture.mask`：没有选区时整屏遮罩，悬停区域挖空并画 `windowHover` 边框（`windowHoverBorderWidth`）与 `windowHoverFill`；只要 `state.has_selection`（包括正在拖出选区时）就只遮选区外、选区内露出原画面。
3. 标注（只在选区内，裁剪到选区）：按 `ls_session_shape_at` 顺序画，见 2.5。
4. 选区边框 `selectionBorder` 宽 `selectionBorderWidth`，8 个控制点（`handleSize`，白底 `handleFill` + `handleBorder` 1pt 边，正方形）；锁定选区或正在画标注时不画控制点。
5. 十字辅助线（设置开启时，SELECTING 阶段）：过光标的整屏横竖线 `crosshair` 1pt。
6. 尺寸提示：`ls_session_place_size_tip`，内容 `{w} × {h}`（像素）+ 锁定比例时 `比例 16:9`；圆角 `tipRadius`，底 `tipBg`，字 `tipText`，等宽数字。SELECTED 阶段单击尺寸提示变成两个输入框（宽、高），回车应用（`ls_session_set_selection`，保持左上角）。
7. 放大镜（`show_magnifier` 且设置开启）：`ls_session_place_magnifier(size, size + infoHeight, offset)`。上部 `size×size` 显示光标周围 `pixels×pixels` 像素放大，像素网格线 `magnifierGrid`，中心像素框 `magnifierCenter` 2pt，十字中线；下部信息区（`tipBg` 底）：第 1 行 `x, y`（像素坐标），第 2 行色块 + HEX，第 3 行颜色名称（`ls_color_name`）；显示提示时第 4 行 `capture.magnifier.copy`。外框白色 1pt，圆角 `magnifier.radius`。
8. 左下角提示（设置开启）：距屏幕左、下各 16pt 的胶囊，`tipBg` 底，文字颜色 `capture.tipsColor`。SELECTING 显示 `capture.tip.select` 和 `capture.tip.keys` 两行；SELECTED 显示 `capture.tip.adjust`。
9. 工具栏、样式面板、提示气泡（2.3、2.4）。

光标形状按 `state.cursor`。

### 2.3 工具栏

位置 `ls_session_place_toolbar(w, h, toolbarGap, toolbarScreenMargin)`：默认在选区下方右对齐，放不下改上方，再放不下放选区内右下角。`Tab` 隐藏/显示。

尺寸：高 `toolbar.height`，按钮 `button` 见方，图标 `iconSize`，按钮间距 `spacing`，左右内边距 `paddingX`，组之间分隔线（宽 `separatorWidth` 高 `separatorHeight`，左右各 `separatorMargin`）。底 `toolbarBg`，边 `toolbarBorder` 1pt，圆角 `radius`，阴影 `shadowBlur`/`shadowOffsetY`/`shadow`。按钮悬停 `surfaceHover` 底，按下 `surfacePressed`，当前工具 `surfaceSelected` 底 + `iconActive` 图标。

按钮顺序（图标名 → 动作）：

| 组 | 按钮 |
|---|---|
| 标注 | `square` 矩形、`circle` 椭圆、`slash` 直线、`move-up-right` 箭头、`pencil` 画笔、`grid-3x3` 马赛克、`type` 文字、`list-ordered` 序号、`focus` 局部高亮、`zoom-in` 放大镜 |
| 编辑 | `undo-2` 撤销、`redo-2` 重做（不可用时图标用 `textTertiary`） |
| 功能 | `scroll-text` 长截图、`video` 录屏、`scan-text` 文字识别、`languages` 翻译、`pin` 贴图 |
| 输出 | `download` 保存、`x` 取消、`check` 完成（`check` 用 `accent` 色图标） |

- 点工具按钮：`ls_session_set_tool`，同时在按钮下方（工具栏在上方时为上方）弹出该工具的样式面板；再点同一工具取消工具并收起面板。
- 悬停：任意按钮悬停 `capture.toolbarPopupDelayMs` 后显示提示气泡「名称　快捷键」，气泡在按钮正下方居中（工具栏在选区上方时在正上方），间距 6pt，夹在屏幕内。工具按钮悬停同样延时后改为弹出该工具的样式面板（不显示气泡）；在面板里改样式只改该工具的默认样式（以及选中的同类标注），不切换当前工具。
- 多选窗口模式（`window_count > 0`）下标注组、长截图、录屏按钮禁用。

### 2.4 样式面板

`ls_session_place_popover`。圆角 `popover.radius`，内边距 `padding`，行高 `rowHeight`，行间距 `gap`，底 `popoverBg`。两行：

- 第 1 行（工具相关，从左到右）：
  - 粗细三档统一为线宽 2/3/6（圆点大小示意），当前值不在三档上时高亮最接近的一档
  - 矩形：粗细三档、实心开关（`square` 实心图示）、圆角开关（开 = 半径 6）
  - 椭圆：粗细三档、实心开关
  - 直线：粗细三档、虚线开关
  - 箭头：四种样式（用内核几何画的小预览）、粗细三档
  - 画笔：画笔/荧光笔、粗细三档
  - 马赛克：四种样式（像素块、像素画笔、模糊、模糊画笔），块大小滑块（4–40），画笔样式时多一个笔刷大小滑块（8–80）
  - 文字：三种样式（普通、描边、底色），字号滑块（10–96），粗体开关
  - 序号：三种样式，大小滑块（16–64），「下一个」数字步进
  - 局部高亮：遮罩深浅滑块（0.2–0.9），圆角开关
  - 放大镜：倍数四档（1.5、2、3、4），边框粗细三档（写 `annotate.magnify.lineWidth`）
- 马赛克没有颜色，第 2 行放块大小/笔刷大小滑块；序号的「下一个」只在本次截图内有效，不写回设置。
- 第 2 行（局部高亮、马赛克除外）：`annotate.palette` 10 个色块（直径 `swatch`，间距 `swatchGap`，选中加 2pt `accent` 外环）+ 自定义颜色（打开系统取色面板）。
- 右上角 `info` 图标，悬停显示 `style.hint`。
- 改动立即 `ls_session_set_style`，并写回设置 `annotate.<tool>`（下次截图沿用）。

### 2.5 标注绘制（两端严格一致）

对每个 `ls_shape_view`：
- `LS_SHAPE_MASK`：用黑色、不透明度 `style.opacity` 按奇偶规则填充 `fill`。
- 先 `fill`（`style.color`，乘 `style.opacity`），再 `stroke`（`style.color`，宽 `style.line_width`，圆头圆角，`dashed` 时虚线段 3×宽、间隔 2×宽），整体不透明度 `style.opacity`（用透明层一次合成，重叠部分不叠加变深）。
- 文字：`editing` 时不画文字。底色样式先填 `text_background` 圆角矩形（`text_rect` 外扩 4pt，圆角 4pt）；描边样式先画 `text_outline` 描边文字（宽 `text_size/8`）再画填充文字；字体同 1 节，字号 `text_size`，`style.bold` 时加粗；多行按 `\n`，行高由平台字体决定（测量回调必须用同一字体）。
- 序号：`fill`/`stroke` 已是圆或方块；数字在 `text_rect` 内水平垂直居中，字号 `text_size`，粗体，颜色 `text_color`。
- 马赛克/模糊：把 `patch` 画到 `patch_rect`（像素图已带透明度）。
- 放大镜：圆内裁剪画冻结画面的 `magnify_src` 放大到圆的外接正方形，再描边 `stroke`。
- 选中标注：控制点用 `ls_session_shape_handles`，白底 `shapeHandleFill` + `shapeHandleBorder`，圆形直径 `handleSize`。没有控制点的标注（画笔、马赛克画笔、文字、序号）选中时在 `bounds` 外画 1pt 虚线框（`shapeSelectionDash`，线段 4、间隔 3）。
- 有 `fill` 路径时一律用 `fill_color` 填充；描边线宽和整体不透明度直接用 `view.style`（内核已换算好，荧光笔、空心序号不要再乘）。马赛克 patch 不再乘不透明度。

### 2.6 文字输入

收到 `LS_EV_TEXT_BEGIN` 后在 `ls_session_text_edit().rect` 放一个原生多行输入框（透明背景、无边框，字体字号颜色与标注一致，支持输入法），每次内容变化调用 `ls_session_set_text`，失焦或 Esc 调用 `ls_session_end_text`。回车换行；主修饰键+回车结束编辑。

### 2.7 动作

`LS_EV_ACTION` 后循环 `ls_session_take_action`：

| 动作 | 行为 |
|---|---|
| `CANCEL` | 关闭全部截图层 |
| `DONE`（回车 / ✓ 按钮） | `save.enterAction`：`copy` 复制、`save` 保存、`both` 两者 |
| `DOUBLE_CLICK` | `capture.doubleClickAction`：`copy` / `save` / `pin` / `none` |
| `COPY` / `SAVE` | 复制 / 保存 |
| `PIN` `OCR` `TRANSLATE` `LONGSHOT` `RECORD` | 见对应章节 |
| `COPY_RGB` `COPY_HEX` `COPY_CUSTOM` | 复制光标下颜色（换算到 `color.space`，格式见 9 节），显示 toast `capture.copiedColor`，不退出 |
| `LAST_AREA` | 没有上次区域，什么也不做 |

连按两下修饰键（Mac Option、Windows Alt，间隔 < 350ms，中间无其他按键）：执行 `capture.doubleModifierAction`（默认 `saveAndOpen`：保存后用 `capture.openWithApp` 打开，空 = 系统默认，`folder` = 在文件夹中显示）。

结束后：记录本次选区为「上次区域」（按屏，`state.json`），若 `capture.returnToPreviousApp` 则重新激活截图前的前台应用。

### 2.8 导出

- 像素矩形 `ls_session_selection_pixels`。离屏按像素尺寸画「冻结画面裁剪 + 标注」（与屏幕上同一套绘制代码，坐标换算到像素）。
- `save.resolution` 为 `standard` 且 scale > 1 时缩到 1 倍（`ls_image_resize`）。
- 复制：剪贴板放 PNG（Mac `NSPasteboard` 的 png + tiff；Windows `CF_DIBV5` + 注册格式 `PNG`）。
- 保存：`save.folder`（不存在则创建；创建失败 toast `notify.folderMissing`）+ 文件名模板（`{yyyy}` `{MM}` `{dd}` `{HH}` `{mm}` `{ss}` 及 `{yyyy-MM-dd HH.mm.ss}` 这种组合，重名加 ` (2)`）+ 扩展名。`askLocation` 时弹系统保存框。
- 格式：`png` `jpg`（品质 `jpegQuality`）`webp`（无损，先转 sRGB；任一边超过 16383 像素时退回 PNG）`tiff` `bmp` `pdf`；Mac 另有 `heic`；Windows 选 `heic` 时退回 `png`。webp 两端都用内核附带的 libwebp（第二期接入，之前退回 png）。
- 颜色描述文件：`display` 嵌入屏幕描述文件（Mac）；`srgb` 转成 sRGB 并嵌入 sRGB。Windows 一律按 sRGB 处理。
- 音效（`general.playSound`）：Mac 系统截图声（`/System/Library/Components/CoreAudio.component/.../Grab.aif` 不存在时用 `NSSound(named:"Pop")`）；Windows 没有系统截图声，播放内存里合成的短快门声（失败时 `SystemAsterisk`）。
- 完成后 toast：复制 `notify.copied`、保存 `notify.saved`（点 toast 打开文件夹）。

### 2.9 其他截图方式

| 功能 | 行为 |
|---|---|
| 窗口阴影参数 | `ls_image_frame(radius = windowCornerRadius·scale, shadow_radius = windowShadowRadius·scale, shadow_offset_y = round(windowShadowRadius/3)·scale, shadow = windowShadowColor)`；输出四周各留 ceil(shadow_radius + |offset_y|) 像素透明边，原图位于 (pad, pad) |
| 截取光标下窗口 | 不进截图层，直接截光标下最上层窗口（Mac `SCScreenshotManager` 用 `desktopIndependentWindow`；macOS 11–13 `CGWindowListCreateImage(.optionIncludingWindow)`；Windows `PrintWindow(PW_RENDERFULLCONTENT)`，失败退回屏幕裁剪），按 `capture.windowCorners/Shadow` 用 `ls_image_frame` 加圆角阴影，然后按 `enterAction` 输出 |
| 截取全屏 | 光标所在屏整屏，按 `enterAction` 输出 |
| 截取上次区域 | 进截图层，直接以上次区域为选区（`has_initial_selection`） |
| 预设尺寸 | 进截图层，选区为 `presetWidth×presetHeight` 像素、中心在光标处并夹在屏内 |
| 延时截图 | 屏幕中央显示倒计时（大号数字，`countdownSize` 圆形 `tipBg` 底），每秒 `delaySound` 嘀一声，到时冻结画面并进截图层 |
| 多窗口 | 截图层里按住 Shift 点窗口，可多选；输出 = 选中窗口各自截图（同单窗口方式）按原位置合成在所有窗口外接矩形内，透明背景；`multiWindowCorners/Shadow` 控制各窗口圆角阴影 |
| 带壳截图 | 见 7 节 |

### 2.10 Windows 特有

- 截图层窗口：`WS_POPUP`、`WS_EX_TOPMOST | WS_EX_TOOLWINDOW`，每屏一个，D2D 渲染到 HWND。
- 键盘：截图层拿前台焦点（`SetForegroundWindow` 前用 `AllowSetForegroundWindow`/`AttachThreadInput` 技巧）。
- 用 Direct2D 1.0 + DirectWrite（Win7 SP1 起都有），不用 1.1 专属接口；没有 D2D 的极端环境退回 GDI+ 不在本期范围。

---

## 3. 贴图

- 来源：截图工具栏「贴图」；全局快捷键 `pinClipboard`（剪贴板图片直接贴；文字先排版渲染成图，最大宽 600pt，`surface` 底 + 12pt 内边距，字 13pt。Mac 保留富文本格式，Windows 按纯文字排版）；贴图库。剪贴板没有可贴内容时 toast `pin.noClipboard`。
- 窗口：无边框置顶，初始位置 = 截图原位置（剪贴板贴图在光标处）。圆角 `pin.cornerRadius`（`pin.corners`），阴影（`pin.shadow`），1pt 边框（活动 `activeBorder`，非活动 `inactiveBorder`）。
- 操作：拖动移动；滚轮缩放（以光标为中心，步长 `zoomStep`，范围 `zoomMin–zoomMax`），缩放时中间显示百分比 1 秒；主修饰键+滚轮调透明度（步长 `opacityStep`，最低 0.1）；方向键移动 1 像素（Shift 10 像素）；双击关闭（`doubleClickCloses`）；Esc 关闭；主修饰键+C 复制，主修饰键+S 保存。
- 悬停按钮（`showButtons`）：右上角浮出 `copy` `download` `pencil`（标注）`x` 四个小按钮（24pt）。
- 右键菜单：复制、保存、标注、文字识别、不透明度（子菜单 100/80/60/40/20%）、缩放（原始大小/50%/200%）、向左旋转、向右旋转、水平翻转、垂直翻转、圆角、阴影、在所有桌面显示、关闭、关闭全部贴图、隐藏全部贴图。
- 标注：在贴图窗内开一个 `lock_selection` 会话，工具栏贴在贴图下方，完成后替换贴图内容。
- 全局 `togglePins`：隐藏/显示全部。
- 「在所有桌面显示」：Mac `collectionBehavior = .canJoinAllSpaces`；Windows 没有公开接口，用 `IVirtualDesktopPinnedApps`（按系统版本选接口 GUID，失败则菜单项禁用）。
- 贴图库：每次贴图把图存进库（内容哈希去重），超过 `libraryMax` 删最旧。库窗口 720×480：网格缩略图（160×120，`surface` 卡片，圆角 8），单击重新贴图，悬停右上角 `trash-2` 删除；标题栏右侧「清空」（确认框）。

## 4. 文字识别

- 截图工具栏「文字识别」、全局 `ocr`（进截图层，选完区域自动识别）。
- 引擎：Mac `VNRecognizeTextRequest`（`.accurate`，语言 zh-Hans、zh-Hant、en、ja、ko，自动纠正关）+ `VNDetectBarcodesRequest`；Windows 内核 OCR（PaddleOCR PP-OCR 中英模型 + ncnn，随安装包，第二期接入）+ zxing-cpp 识别二维码。
- `ocr.preferQRCode`：识别到二维码时只显示二维码内容（链接可点「打开链接」）。
- 结果窗口 520×420，可调大小：顶部工具行（开关胶囊：合并换行、英文标点、连续识别、固定区域），标题栏右侧图钉按钮切换置顶（`ocr.keepOnTop` 默认开），中间可编辑文本区，底部按钮：复制、复制并关闭、翻译；识别到链接二维码时多一个「打开链接」。左侧 120pt 缩略图（截图）可折叠。开关改动写回 `ocr.*`。
- `ocr.preferQRCode` 关闭时只显示文字，没有文字才显示二维码内容。
- 合并换行：同一段落内的行用空格（中文无空格）连接，空行保留。英文标点：，。；：？！（）「」“”‘’ 转半角。
- 连续识别：窗口保持打开，再按 `ocr` 快捷键新结果追加到末尾（空行分隔）。固定区域：记住选区，再按快捷键直接识别同一区域。

## 5. 截图翻译

- 截图工具栏「翻译」、全局 `translate`、OCR 窗口「翻译」按钮。
- 窗口 560×420：上「原文」（可编辑），下「译文」，顶部：源语言（自动）→ 目标语言下拉、翻译服务下拉；底部：复制译文。
- 服务：`microsoft`（免密钥：先 GET `https://edge.microsoft.com/translate/auth` 拿令牌，缓存 8 分钟，再 POST `https://api-edge.cognitivetranslator.com/translate?api-version=3.0&to=xx`，失败退回 `api.cognitive.microsofttranslator.com`）、`baidu`、`tencent`（TC3-HMAC-SHA256 签名）、`youdao`（v3 签名）、`deepl`（free key 用 api-free 域名）、`openai`（`/chat/completions`，系统提示「把用户文本翻译成{目标语言}，只输出译文」）。
- 截图翻译整理原文时固定用「合并换行」；窗口里改目标语言和服务会写回设置。
- 目标语言 `auto`：原文以中文为主（CJK 字符过半）译成英语，否则译成简体中文。
- 网络请求：Mac `URLSession`；Windows `WinHTTP`（Win7 默认不开 TLS 1.2，用 `WINHTTP_OPTION_SECURE_PROTOCOLS` 显式开启）。超时 15 秒。

## 6. 长截图

1. 进截图层选好区域（窗口模式不行，提示 `longshot.windowMode`），按 S 或工具栏「长截图」。
2. 截图层收起，只留：区域红色细边框（不进画面）+ 区域右侧（放不下则左侧）宽 160pt 的面板：缩略预览（实时增长，自动滚到底）、高度 `longshot.height`、按钮「自动滚动/停止滚动」「完成」「取消」。首次使用先显示注意事项（三条 `longshot.note*`，「我知道了」后记 `longshot.tutorialSeen`）。
3. 抓帧：每 100ms 截一次该区域（排除自己的窗口），喂给 `ls_stitch_push`。`NO_MATCH` 时面板提示 `longshot.noMatch`；`MAX` 停止并提示。
4. 自动滚动：在区域中心向下发滚轮事件（Mac `CGEventCreateScrollWheelEvent` 像素单位，每次 `scrollSpeed×40` 像素；Windows `SendInput` `MOUSEEVENTF_WHEEL` 每次 -120×scrollSpeed/3），每次滚动后等一帧接上再滚；连续 5 帧 `UNCHANGED` 视为到底，自动完成。Mac 需要辅助功能权限，没有时按钮提示去授权。
5. `longshot.autoScroll` 开启时（默认），有自动滚动条件（Mac 需辅助功能权限）就在开始后自动滚动，否则等用户手动滚。
6. 完成：`ls_stitch_render` 得到结果，打开结果窗口（宽 = min(图宽, 屏宽 60%)，可滚动查看）：复制、保存、贴图、关闭。贴图时缩到屏幕高度 80% 以内。

## 7. 带壳截图

- `shell.model` 取值：`auto` `macbookAir` `macbookPro14` `macbookPro16` `imac24` `studioDisplay` `laptop` `monitor` `iphone` `ipad`（iPhone、iPad 外壳横放）。
- 全局 `captureShell`：延时（`shell.delay` 默认 0）后截全屏，套上设备外壳，打开带壳预览窗口：左侧预览，右侧机型列表（自动匹配 / MacBook Air / MacBook Pro 14 / MacBook Pro 16 / iMac 24 / Studio Display / 通用笔记本 / 通用显示器 / iPhone / iPad）+ 背景（透明/白/黑）+ 复制、保存。
- 外壳为矢量自绘（圆角机身、屏幕黑边、刘海/摄像头、底座），不用任何厂商图片。Mac 自动匹配按 `sysctl hw.model` 查表；Windows 自动匹配为「通用笔记本」（有内置屏）或「通用显示器」。
- 截图按外壳屏幕区域等比缩放、居中裁切。

## 8. 录屏

### 8.1 选择区域与设置

全局 `record` 或截图工具栏「录屏」：进入截图层（录屏模式：工具栏换成录屏设置面板，不显示标注工具），拖动选区域 / 单击窗口 / F 全屏。选区下方显示设置面板（高 `toolbar.height`）：

`mic` 麦克风开关 · `volume-2` 系统声音开关 · `webcam` 摄像头开关 · `mouse-pointer-2` 光标 · `mouse-pointer-click` 点击 · `keyboard` 按键 | 格式（MP4/GIF）| 清晰度（超清/高清/标清）| 帧率（15/24/30/60）| 「开始录制」主按钮（`accent` 底）

各开关的默认值来自 `record.*`，改动写回设置。按住开关 0.5 秒弹出设备选择（麦克风、摄像头）。

录屏模式的截图层：不显示放大镜；单键只保留 F（全屏）、D（上次区域）、V（开始录制）、Tab；选好区域后左下角提示用 `record.tip.adjust`。回车、V、双击选区或「开始录制」按钮开始，Esc 取消。设置面板控件形式：开关打开时 `surfaceSelected` 底 + `iconActive` 图标，关闭时麦克风用 `mic-off`、系统声音用 `volume-x`；格式、清晰度、帧率用「当前值 + chevron-down」的紧凑下拉，宽度按最长选项固定。选 GIF 时两个声音开关禁用。

### 8.2 录制

- 倒计时 `record.countdown` 秒（区域中央大号数字），可在倒计时中按 Esc 取消。
- 区域边框：`recordBar.regionBorder` 2pt 虚线，画在区域外侧，不进画面。
- 控制条：`recordBar.height` 胶囊，放在区域下方居中（放不下放上方，全屏录制时放屏幕底部居中并在 3 秒后自动半透明）：红点 + 计时 `00:12` · 暂停/继续 · 画笔 · 停止 · 取消。控制条、边框、设置面板都不进画面（Mac `SCContentFilter` 排除；Windows 10 2004+ `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`，更早的系统把控制条放在区域外，全屏录制时控制条只在暂停时显示）。
- Mac 菜单栏图标在录制时变成红点 + 计时，点它停止。
- 画笔：全局 `recordPen` 或控制条按钮，区域上叠一层透明画板（进画面），自由画线（颜色取标注调色板第 1 色，线宽 4），主修饰键+Z 撤销一笔，Esc 或再按一次退出画笔并清除笔迹。
- 显示点击：每次按下在光标处画一个扩散圆（`clickOverlay`，左键蓝、右键橙，`durationMs`），进画面。
- 显示按键：底部（`record.keys.position`）浮层显示最近按键组合（`keysOverlay` 样式，`maxKeys` 个，`fadeMs` 后淡出；`onlyShortcuts` 只显示带修饰键的组合；`hideRepeats` 连续相同键只显示一次）；Mac 用 ⌘⌥⌃⇧ 符号，Windows 用 Ctrl Alt Shift Win 文字。Mac 需要「输入监控」权限。
- 摄像头：区域右下角画中画（圆形或圆角矩形，直径为区域短边 22%），可拖动，进画面。
- 定时：`maxDurationSec` > 0 时到时自动停止。
- 录制中再按全局 `record` = 停止（倒计时中 = 取消）。菜单栏/托盘图标录制中左键单击停止，右键（Mac 另加 Control 单击）弹出原菜单。
- 系统无法把控制条排除出画面时（macOS 11–12.2、Windows 10 2004 以前）：控制条放在区域外；区域外放不下（含全屏录制）时只在暂停时显示。
- 视觉细节：按键浮层修饰键顺序 Mac 为 ⌃ ⌥ ⇧ ⌘、Windows 为 Ctrl Alt Shift Win，键之间空格分隔，胶囊左右内边距 = padding × 1.4，带阴影；控制条用 `toolbarBg` 底与 `toolbarBorder` 边，停止为红色实心方块，暂停时红点变 `textTertiary`；摄像头圆角矩形为正方形、圆角 18%，外圈 2pt 白边。

### 8.3 编码

| | Mac | Windows |
|---|---|---|
| 画面 | macOS 12.3+ `SCStream`（区域 = `sourceRect`，`showsCursor`）；11–12.2 `CGDisplayStream`（`dlsym`） | Win8+ DXGI Desktop Duplication（按显示器，自己画光标）；Win7 GDI `BitBlt` 抓区域 |
| 视频 | `AVAssetWriter` H.264（超清：原像素但长边≤4096（H.264 硬件编码上限），高清：长边≤1920，标清：长边≤1280；码率 = 宽×高×帧率×0.1）；屏幕静止时每秒补一帧 | Media Foundation `IMFSinkWriter` H.264 + AAC 写 MP4（硬件编码优先，`MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`）；系统没有 MF（N 版、服务器未装功能）提示 `record.encoderMissing` |
| 麦克风 | `AVCaptureSession` 音频 | WASAPI 采集 |
| 系统声音 | macOS 13+ `SCStream.capturesAudio`；更早提示 `record.systemAudioUnsupported` | WASAPI loopback（Vista 起都有） |
| 混音 | 两路重采样到 48k 立体声相加写一条 AAC 音轨 | 同左 |
| GIF | 尺寸 = 区域逻辑尺寸（1 倍）× `gifScale`，不受清晰度影响；按 `gifFps` 抓帧存临时原始帧，停止后用内核 `ls_gif_*` 编码（后台线程，显示 `record.encodingGif` 进度） | 同左 |

文件名 `Lemon Shot {yyyy-MM-dd HH.mm.ss}.mp4`，存 `record.folder`。停止后 `afterAction`：`none` / `play` 系统默认播放器打开 / `reveal` 在文件夹中显示；toast `record.saved`。

## 9. 取色

- 截图层放大镜里随时按 R/H/C 复制（见 2.7）。全局 `colorPicker`：进入只有放大镜的截图层（不能框选），单击复制 HEX 并退出，Esc 退出。
- 颜色值：冻结画面像素是屏幕原始值。`color.space`：`native` 原样；`srgb`/`p3`/`adobe`/`generic` 从屏幕色彩空间换算（Mac 用 ColorSync 从屏幕描述文件换到 sRGB，再用 `ls_color_convert`；Windows 视屏幕为 sRGB 直接 `ls_color_convert`）。
- 格式：RGB 为 `R, G, B`；HEX 用 `ls_color_hex`（`hexPrefix`、`hexUppercase`）；自定义用 `ls_color_format(customFormat)`。

## 10. 录音

全局 `recordAudio`：屏幕右上角浮出面板（300×120，圆角 12，`popoverBg`）：来源开关（麦克风/系统声音）、电平条、计时、开始/停止按钮。格式 `m4a`（AAC）/ `wav`；`mp3`：Windows 8+ 用 MF MP3 编码器，Mac 与 Win7 不支持时选项禁用。存 `audio.folder`，结束后 toast `audio.saved`（点击在文件夹中显示）。

## 11. 偏好设置

固定 760×520 窗口（Mac 用全尺寸内容视图、标题栏透明，侧边栏顶部为红绿灯留出 48pt；Windows 用原生标题栏，侧边栏顶部只留 12pt），左侧边栏 `sidebarWidth`（图标 + 文字，项高 `sidebarItemHeight`，选中 `surfaceSelected` 圆角底），右侧内容区（`contentPadding`）：页标题（`header` 字号）+ 分组（组标题 `caption` 字号 `textSecondary`），每行左标签（宽 `labelWidth`）右控件，行高 `rowHeight`；标签放不下时折成两行（行高随之增加），不缩小字号。内容超出窗口高度时内容区可滚动。控件全部自绘：开关（`switchWidth×switchHeight`）、分段选择、下拉（弹出自绘菜单）、滑块（右侧显示数值）、数字步进、颜色块（点开调色板气泡 + 「自定义…」系统取色）、路径（显示路径末两级 + 「选择…」）、快捷键录入框、文本框、密码框、按钮。

| 页（图标） | 内容 |
|---|---|
| 通用 `settings` | 语言、开机启动、显示菜单栏/托盘图标、截图音效、界面外观；底部按钮：导出设置、导入设置、恢复默认 |
| 截图 `crop` | 自动识别窗口、截入光标、十字辅助线、放大镜、操作提示 + 提示颜色、右键直接退出、切回原应用、工具设置弹出延迟（滑块 0–1500ms）、双击选区、连按两下修饰键、打开方式、预设尺寸（宽×高）、延时时长；分组「窗口截图」：圆角+半径、阴影+半径+颜色；分组「多窗口截图」：圆角、阴影 |
| 标注 `pencil` | 调色板 10 色可逐个改；各工具默认粗细/样式（与样式面板一致） |
| 保存 `download` | 回车键、保存时同时复制、保存位置、每次询问、格式、品质（jpg 时）、分辨率、文件名模板（下面实时显示示例）、颜色描述文件 |
| 贴图 `pin` | 圆角+半径、阴影、默认不透明度、双击关闭、所有桌面、显示按钮、贴图库最多保存（10–500） |
| 取色 `pipette` | 色彩空间、HEX 带 #、HEX 大写、自定义格式（`info` 说明占位符）+ 实时示例 |
| 识别与翻译 `scan-text` | OCR 各开关；翻译服务下拉、默认译为；所选服务的密钥字段 + 「测试」按钮 |
| 录屏 `video` | 保存位置、格式、清晰度、帧率、GIF 帧率与尺寸、麦克风设备、系统声音、摄像头设备与形状、光标、点击、按键分组（外观、大小、消失延迟、最多显示、只显示快捷键、不显示重复、位置）、倒计时、录制时长、录完之后 |
| 录音 `mic` | 保存位置、格式、麦克风设备、系统声音 |
| 快捷键 `keyboard` | 全局快捷键表（18 项，录入框 + 清除，冲突标红 `pref.shortcuts.conflict`）、截图时按键表、恢复默认 |
| 权限 `shield-check` | Mac：屏幕录制、辅助功能、输入监控、麦克风、摄像头、系统声音，每行状态 + 「去授权」（打开系统设置对应页）；Windows：同样的行，状态一律「无需授权」，麦克风/摄像头在 Win10+ 读隐私开关 |
| 关于 `info` | 图标、名称、版本、`about.tagline`、官网、源代码、开源许可、第三方组件（弹窗列出 Lucide ISC、libwebp BSD、zxing-cpp Apache-2.0、ncnn BSD、PaddleOCR 模型 Apache-2.0） |

设置变更立即生效、立即写盘。

## 12. 菜单栏 / 托盘菜单

单击图标弹出菜单（Windows 托盘：左键单击 = 截图，右键 = 菜单；Mac：单击 = 菜单）：

截图（快捷键）/ 截取光标下的窗口 / 截取全屏 / 截取上次区域 / 延时截图 / 带壳截图 / 长截图 — 分隔 — 录屏 / 录音 — 分隔 — 文字识别 / 截图翻译 / 取色 — 分隔 — 剪贴板贴图 / 贴图库 / 隐藏/显示全部贴图 / 关闭全部贴图 — 分隔 — 打开截图文件夹 / 偏好设置… / 关于 Lemon Shot — 分隔 — 退出 Lemon Shot

菜单用系统原生菜单（Mac `NSMenu`、Windows `TrackPopupMenu`），右侧显示快捷键。菜单栏/托盘图标两端同一设计：单色取景框（四角折线）加中心圆点，Mac 用模板图随系统深浅色，Windows 按任务栏深浅色各一套；录制中换成红点。录制中顶部多「停止录制」「暂停/继续录制」。

## 13. 提示（toast）

屏幕底部居中上方 80pt，胶囊（高 36，左右 16，`tipBg` 底、白字 13pt，左侧 16pt 图标），淡入 120ms，停留 1.5 秒，淡出 200ms；可点击时光标为手形。不抢焦点，不进截图/录屏画面。

## 14. 权限（Mac）

首次启动若没有屏幕录制权限，打开偏好设置的权限页。截图时没有权限：toast `notify.needScreenPermission` 并打开权限页。检测：屏幕录制 `CGPreflightScreenCaptureAccess`，辅助功能 `AXIsProcessTrusted`，输入监控 `IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)`，麦克风/摄像头 `AVCaptureDevice.authorizationStatus`。

## 15. 两端代码结构对照

模块名两端一致，便于逐项对照：

| 模块 | 内容 |
|---|---|
| `App` | 启动、单实例、菜单栏/托盘、快捷键注册、各功能入口 |
| `Settings` `I18n` `Theme` `Icons` | 设置读写、文案、令牌、图标 |
| `UI` | 自绘控件：按钮、图标按钮、开关、分段、下拉、滑块、步进、颜色块、快捷键框、文本框、气泡、toast |
| `Capture` | 冻结画面、窗口列表、截图层窗口、会话桥接、放大镜、工具栏、样式面板、标注绘制、导出 |
| `Pin` | 贴图窗、贴图库 |
| `OCR` `Translate` | 识别、翻译、结果窗口 |
| `LongShot` `Shell` | 长截图、带壳截图 |
| `Record` | 区域选择、设置面板、控制条、画面/声音采集、编码、按键/点击浮层、摄像头、画笔 |
| `Audio` | 录音 |
| `Prefs` | 偏好设置窗口与各页 |
| `SelfTest` | 自测截图 |
