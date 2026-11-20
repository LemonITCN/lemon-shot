// 应用：启动、单实例、托盘、全局快捷键、各功能入口。
#pragma once

#include <windows.h>

#include <string>

namespace ls {
namespace app {

enum Command {
    CMD_NONE = 0,
    CMD_CAPTURE = 100,
    CMD_CAPTURE_WINDOW,
    CMD_CAPTURE_FULLSCREEN,
    CMD_CAPTURE_LAST_AREA,
    CMD_CAPTURE_PRESET,
    CMD_CAPTURE_DELAYED,
    CMD_CAPTURE_SHELL,
    CMD_LONGSHOT,
    CMD_RECORD,
    CMD_RECORD_AUDIO,
    CMD_RECORD_PAUSE,
    CMD_RECORD_PEN,
    CMD_RECORD_STOP,
    CMD_OCR,
    CMD_TRANSLATE,
    CMD_COLOR_PICKER,
    CMD_PIN_CLIPBOARD,
    CMD_PIN_LIBRARY,
    CMD_TOGGLE_PINS,
    CMD_CLOSE_PINS,
    CMD_OPEN_FOLDER,
    CMD_PREFS,
    CMD_ABOUT,
    CMD_QUIT,
};

int Run(HINSTANCE inst);
HINSTANCE Instance();
HWND MainHwnd();  // 隐藏主窗口：托盘、快捷键、剪贴板所有者
void Execute(int cmd);
int CommandForShortcut(const std::string &id);
void Quit();

// 在当前消息处理完之后执行
void Defer(void (*fn)(void *), void *ctx);
// 生成应用图标（运行时用 icons.json 画；size 为像素）
HICON AppIcon(int size);
void PlayShutter();
void OnLanguageChanged();

}  // namespace app
}  // namespace ls
