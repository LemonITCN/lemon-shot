// 「在所有桌面显示」：Windows 没有公开接口，用外壳的 IVirtualDesktopPinnedApps（Win10+）。取不到时返回 false，菜单项禁用。
#pragma once

#include <windows.h>

namespace ls {
namespace vdesk {

bool Available(HWND hwnd);
bool Pin(HWND hwnd, bool pin);

}  // namespace vdesk
}  // namespace ls
