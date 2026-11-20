#include "Record/Hooks.h"

#include "Record/RecordUtil.h"
#include "Util/Log.h"

namespace ls {
namespace rec {
namespace hooks {

namespace {

HWND g_target = nullptr;
bool g_keys = false, g_mouse = false;
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
HANDLE g_ready = nullptr;
bool g_down[256];  // 钩子线程内：各键当前是否按下（判断自动重复与修饰键）

int Mods() {
    int m = 0;
    if (g_down[VK_LCONTROL] || g_down[VK_RCONTROL] || g_down[VK_CONTROL]) m |= kModCtrl;
    if (g_down[VK_LMENU] || g_down[VK_RMENU] || g_down[VK_MENU]) m |= kModAlt;
    if (g_down[VK_LSHIFT] || g_down[VK_RSHIFT] || g_down[VK_SHIFT]) m |= kModShift;
    if (g_down[VK_LWIN] || g_down[VK_RWIN]) m |= kModWin;
    return m;
}

LRESULT CALLBACK KeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        DWORD vk = k->vkCode & 0xFF;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        bool up = wp == WM_KEYUP || wp == WM_SYSKEYUP;
        if (down) {
            bool repeat = g_down[vk];
            g_down[vk] = true;
            if (!repeat && g_target) {
                WPARAM w = (WPARAM)(vk | ((DWORD)Mods() << 16));
                LPARAM l = (LPARAM)((k->scanCode & 0xFFFF) | ((k->flags & LLKHF_EXTENDED) ? 0x10000 : 0));
                PostMessageW(g_target, WM_REC_KEY, w, l);
            }
        } else if (up) {
            g_down[vk] = false;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN) && g_target) {
        const MSLLHOOKSTRUCT *m = (const MSLLHOOKSTRUCT *)lp;
        PostMessageW(g_target, WM_REC_CLICK, wp == WM_RBUTTONDOWN ? 1 : 0, MAKELPARAM((short)m->pt.x, (short)m->pt.y));
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI Proc(void *) {
    for (auto &d : g_down) d = false;
    // 开始前已经按住的修饰键
    const int mods[] = {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN};
    for (int vk : mods) g_down[vk] = GetAsyncKeyState(vk) < 0;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HHOOK kh = g_keys ? SetWindowsHookExW(WH_KEYBOARD_LL, &KeyProc, inst, 0) : nullptr;
    HHOOK mh = g_mouse ? SetWindowsHookExW(WH_MOUSE_LL, &MouseProc, inst, 0) : nullptr;
    if ((g_keys && !kh) || (g_mouse && !mh)) LS_LOG("record: 安装低级钩子失败 %lu", GetLastError());
    // 先建好本线程的消息队列再通知启动方（之后 PostThreadMessage 才能送达）
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    SetEvent(g_ready);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (kh) UnhookWindowsHookEx(kh);
    if (mh) UnhookWindowsHookEx(mh);
    return 0;
}

}  // namespace

bool Start(HWND target, bool keys, bool mouse) {
    Stop();
    if (!keys && !mouse) return true;
    g_target = target;
    g_keys = keys;
    g_mouse = mouse;
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, &Proc, nullptr, 0, &g_threadId);
    if (!g_thread) {
        CloseHandle(g_ready);
        g_ready = nullptr;
        return false;
    }
    WaitForSingleObject(g_ready, 2000);
    return true;
}

void Stop() {
    if (g_thread) {
        g_target = nullptr;
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
        if (WaitForSingleObject(g_thread, 3000) == WAIT_TIMEOUT) LS_LOG("record: 钩子线程没有及时退出");
        CloseHandle(g_thread);
        g_thread = nullptr;
        g_threadId = 0;
    }
    if (g_ready) CloseHandle(g_ready);
    g_ready = nullptr;
}

}  // namespace hooks
}  // namespace rec
}  // namespace ls
