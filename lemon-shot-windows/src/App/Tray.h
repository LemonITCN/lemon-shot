// 托盘图标与菜单（spec 12）：左键截图，右键系统原生菜单，菜单项右侧显示快捷键。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "UI/Gfx.h"

namespace ls {
namespace tray {

struct MenuItem {
    int cmd;              // 0 = 分隔线
    const char *key;      // i18n 键
    const char *shortcut; // 全局快捷键 id，可空
};

std::vector<MenuItem> Items();  // 当前菜单结构（录制中顶部多两项）
void Create(HWND owner);
void Remove();
void Refresh();  // 图标、提示文字、显示/隐藏（general.showStatusIcon）
void OnTaskbarCreated();
// 返回 true 表示消息已处理
bool HandleCallback(HWND owner, WPARAM wp, LPARAM lp);
void ShowMenu(HWND owner);
void Balloon(const std::wstring &title, const std::wstring &text);

// 自测：把菜单画成一张图（与系统菜单同样的结构）
RectF MenuPreviewSize();
void DrawMenuPreview(Canvas &c, RectF r);

}  // namespace tray
}  // namespace ls
