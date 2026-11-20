// 低级键盘/鼠标钩子（spec 8.2 显示按键、显示点击）：单独一个线程装 WH_KEYBOARD_LL / WH_MOUSE_LL 并跑消息循环，
// 钩子回调里只做最少的事（记录修饰键状态、PostMessage 给界面线程），不拦截任何输入。
#pragma once

#include <windows.h>

namespace ls {
namespace rec {

enum : UINT {
    WM_REC_KEY = WM_APP + 0x310,  // wParam：低 16 位 vk，高 16 位修饰键（kModCtrl…）；lParam：扫描码 | 扩展键标志 << 16
    WM_REC_CLICK,                 // wParam：0 左键，1 右键；lParam：屏幕像素坐标 MAKELPARAM(x, y)
};

enum { kModCtrl = 1, kModAlt = 2, kModShift = 4, kModWin = 8 };

namespace hooks {

bool Start(HWND target, bool keys, bool mouse);
void Stop();

}  // namespace hooks
}  // namespace rec
}  // namespace ls
