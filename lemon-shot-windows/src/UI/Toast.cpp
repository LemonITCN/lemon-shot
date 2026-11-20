#include "UI/Toast.h"

#include <cmath>

#include "Capture/Screen.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/Util.h"

namespace ls {
namespace toast {

namespace {

const float kHeight = 36, kPad = 16, kIcon = 16, kGap = 8, kBottom = 80, kTextSize = 13;
const double kHoldMs = 1500;

class ToastWindow : public Window {
public:
    std::wstring text;
    std::string icon;
    Action action;
    double start = 0;
    bool visible = false;
    LayeredSurface surface;
    int x = 0, y = 0;
    UINT dpi = 96;

    void ShowNew() {
        if (!hwnd_) {
            if (!CreateWin(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP, 0, 0, 1, 1,
                           nullptr, L"Lemon Shot"))
                return;
            api::ExcludeFromCapture(hwnd_);
        }
        LONG ex = GetWindowLongW(hwnd_, GWL_EXSTYLE);
        if (action.kind == kNone)
            ex |= WS_EX_TRANSPARENT;
        else
            ex &= ~WS_EX_TRANSPARENT;
        SetWindowLongW(hwnd_, GWL_EXSTYLE, ex);
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(mon, &mi);
        dpi = api::MonitorDpi(mon);
        float s = dpi / 96.f;
        RectF sz = Measure(text);
        int pw = (int)std::ceil(sz.w * s), ph = (int)std::ceil(sz.h * s);
        x = (mi.rcWork.left + mi.rcWork.right) / 2 - pw / 2;
        y = mi.rcWork.bottom - (int)std::lround(kBottom * s) - ph;
        start = NowSeconds();
        visible = true;
        Render(0);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(hwnd_, 1, 15, nullptr);
    }

    void Render(BYTE alpha) {
        float s = dpi / 96.f;
        RectF sz = Measure(text);
        ID2D1RenderTarget *rt = surface.Begin((int)std::ceil(sz.w * s), (int)std::ceil(sz.h * s), dpi);
        if (!rt) return;
        {
            Canvas c(rt);
            Draw(c, RectF(0, 0, sz.w, sz.h), text, icon.c_str());
        }
        surface.End(hwnd_, x, y, alpha);
    }

    void Hide() {
        visible = false;
        if (hwnd_) {
            KillTimer(hwnd_, 1);
            ShowWindow(hwnd_, SW_HIDE);
        }
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        switch (msg) {
        case WM_TIMER: {
            double ms = (NowSeconds() - start) * 1000.0;
            double in = 120, out = 200;
            double a;
            if (ms < in)
                a = ms / in;
            else if (ms < in + kHoldMs)
                a = 1;
            else if (ms < in + kHoldMs + out)
                a = 1 - (ms - in - kHoldMs) / out;
            else {
                Hide();
                return 0;
            }
            LayeredSurface::SetAlpha(hwnd_, (BYTE)(a * 255.0 + 0.5));
            return 0;
        }
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR:
            if (action.kind != kNone) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649) /* IDC_HAND */));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP:
            if (action.kind == kReveal) RevealInFolder(action.path);
            else if (action.kind == kOpen) ShellOpen(action.path);
            Hide();
            return 0;
        }
        return Window::Handle(msg, wp, lp);
    }
};

ToastWindow *g_toast = nullptr;

}  // namespace

float Margin() { return 0; }

RectF Measure(const std::wstring &text) {
    float tw = gfx::Measure(text, kTextSize, kRegular).w;
    return RectF(0, 0, std::ceil(kPad + kIcon + kGap + tw + kPad), kHeight);
}

void Draw(Canvas &c, RectF r, const std::wstring &text, const char *icon) {
    const auto &cap = theme::Cap();
    c.FillRound(r, r.h / 2, cap.tipBg);
    if (icon && icon[0]) c.Icon(icon, r.x + kPad + kIcon / 2, r.cy(), kIcon, Color{1, 1, 1, 1}, 1.75f);
    TextStyle ts;
    ts.size = kTextSize;
    ts.color = Color{1, 1, 1, 1};
    ts.v = VAlign::Center;
    float tx = r.x + kPad + kIcon + kGap;
    c.Text(text, RectF(tx, r.y, r.r() - tx, r.h), ts);
}

void Show(const std::wstring &text, const char *icon, const Action &action) {
    if (!g_toast) g_toast = new ToastWindow();
    g_toast->text = text;
    g_toast->icon = icon ? icon : "";
    g_toast->action = action;
    g_toast->ShowNew();
}

void HideNow() {
    if (g_toast && g_toast->visible) g_toast->Hide();
}

}  // namespace toast
}  // namespace ls
