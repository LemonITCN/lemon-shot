// 窗口基类：所有窗口共用一个窗口类，消息转发到虚函数 Handle。
#pragma once

#include <windows.h>

namespace ls {

// 自定义消息
enum : UINT {
    WM_LS_TRAY = WM_APP + 1,       // 托盘图标回调
    WM_LS_SHOW_PREFS = WM_APP + 2, // 第二个实例请求打开偏好设置
    WM_LS_END_TEXT = WM_APP + 3,   // 文字输入框失焦，延迟结束编辑
    WM_LS_CLOSE_POPUP = WM_APP + 4,
    WM_LS_DEFERRED = WM_APP + 5,   // 通用延迟执行（wParam = 编号）
};

void RegisterWindowClass(HINSTANCE inst);
extern const wchar_t kWindowClass[];

class Window {
public:
    Window() = default;
    virtual ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    HWND Hwnd() const { return hwnd_; }
    bool CreateWin(DWORD exStyle, DWORD style, int x, int y, int w, int h, HWND parent, const wchar_t *title = L"");
    void DestroyWin();

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

protected:
    virtual LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    // 窗口销毁之后（WM_NCDESTROY），hwnd_ 已清空。可以在这里 delete this。
    virtual void OnDestroyed() {}

    HWND hwnd_ = nullptr;
};

// 鼠标坐标（客户区像素，含负值）
inline int MouseX(LPARAM lp) { return (int)(short)LOWORD(lp); }
inline int MouseY(LPARAM lp) { return (int)(short)HIWORD(lp); }

// 当前修饰键 → LS_MOD_*（Ctrl 同时置 LS_MOD_PRIMARY）
int CurrentMods();

}  // namespace ls
