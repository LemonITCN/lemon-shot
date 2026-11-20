// 截图入口（spec 2.1、2.9）：截图层、截光标下窗口、全屏、上次区域、预设尺寸、延时、取色。
#pragma once

namespace ls {
namespace capture {

// Ocr / Translate / LongShot：全局快捷键进来的截图层，选完区域直接交给对应模块
// Record：录屏模式（spec 8.1），工具栏换成录屏设置面板
enum class Mode { Normal, LastArea, Preset, ColorPicker, Ocr, Translate, LongShot, Record };

void Start(Mode mode = Mode::Normal);
bool IsActive();
void Cancel();
void CaptureWindowUnderCursor();
void CaptureFullscreen();
void StartDelayed();
void OnSettingsChanged();

}  // namespace capture
}  // namespace ls
