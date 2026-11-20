#include "UI/Window.h"

#include "lemonshot.h"

namespace ls {

const wchar_t kWindowClass[] = L"LemonShot.Window";
static HINSTANCE g_inst = nullptr;

void RegisterWindowClass(HINSTANCE inst) {
    g_inst = inst;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &Window::Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
}

Window::~Window() {
    if (hwnd_) {
        // 对象先于窗口析构：断开绑定再销毁，避免回调到已析构的对象
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
}

bool Window::CreateWin(DWORD exStyle, DWORD style, int x, int y, int w, int h, HWND parent, const wchar_t *title) {
    HWND hwnd = CreateWindowExW(exStyle, kWindowClass, title, style, x, y, w, h, parent, nullptr, g_inst, this);
    return hwnd != nullptr;
}

void Window::DestroyWin() {
    if (hwnd_) DestroyWindow(hwnd_);
}

LRESULT Window::Handle(UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(hwnd_, msg, wp, lp); }

LRESULT CALLBACK Window::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
        Window *w = reinterpret_cast<Window *>(cs->lpCreateParams);
        if (w) {
            w->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
        }
    }
    Window *self = reinterpret_cast<Window *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) {
        LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->hwnd_ = nullptr;
        self->OnDestroyed();
        return r;
    }
    return self->Handle(msg, wp, lp);
}

int CurrentMods() {
    int m = 0;
    if (GetKeyState(VK_SHIFT) < 0) m |= LS_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) < 0) m |= LS_MOD_CTRL | LS_MOD_PRIMARY;
    if (GetKeyState(VK_MENU) < 0) m |= LS_MOD_ALT;
    return m;
}

}  // namespace ls
