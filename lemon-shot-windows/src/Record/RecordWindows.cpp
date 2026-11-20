#include "Record/RecordWindows.h"

#include <cmath>

#include "Settings/I18n.h"
#include "UI/Controls.h"
#include "Util/Api.h"
#include "Util/ComPtr.h"
#include "Util/Util.h"

namespace ls {
namespace rec {

namespace {

const DWORD kChromeEx = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
const float kBarMargin = 18;  // 控制条四周留给阴影的边距
enum { TIMER_TIP = 1, TIMER_FADE_WAIT = 2, TIMER_FADE = 3 };

}  // namespace

// ---------------------------------------------------------------- 提示气泡

void TipWindow::Show(const std::wstring &text, RECT anchor, bool below, UINT dpi) {
    if (text.empty()) return;
    if (!hwnd_) {
        if (!CreateWin(kChromeEx | WS_EX_TRANSPARENT, WS_POPUP, 0, 0, 1, 1, nullptr, L"LemonShot")) return;
        api::ExcludeFromCapture(hwnd_);
    }
    float s = dpi / 96.f;
    RectF sz = ui::MeasureBubble(text, 320);
    int w = (int)std::ceil(sz.w * s), h = (int)std::ceil(sz.h * s);
    int gap = (int)std::lround(6 * s);
    int x = (anchor.left + anchor.right) / 2 - w / 2;
    int y = below ? anchor.bottom + gap : anchor.top - gap - h;
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    if (below && y + h > mi.rcMonitor.bottom) y = anchor.top - gap - h;
    if (!below && y < mi.rcMonitor.top) y = anchor.bottom + gap;
    if (x + w > mi.rcMonitor.right - 4) x = mi.rcMonitor.right - 4 - w;
    if (x < mi.rcMonitor.left + 4) x = mi.rcMonitor.left + 4;
    ID2D1RenderTarget *rt = surface_.Begin(w, h, dpi);
    if (!rt) return;
    {
        Canvas c(rt);
        ui::DrawBubble(c, RectF(0, 0, sz.w, sz.h), text);
    }
    surface_.End(hwnd_, x, y, 255);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void TipWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

// ---------------------------------------------------------------- 控制条

bool BarWindow::Create(UINT dpi) {
    dpi_ = dpi;
    if (hwnd_) return true;
    if (!CreateWin(kChromeEx, WS_POPUP, 0, 0, 1, 1, nullptr, L"LemonShot")) return false;
    excluded_ = api::ExcludeFromCapture(hwnd_);
    return true;
}

void BarWindow::Place(int x, int y) {
    pos_ = POINT{x, y};
    if (visible_) Render();
}

void BarWindow::Render() {
    if (!hwnd_) return;
    float s = Scale();
    SizeF bs = view.Size();
    int w = (int)std::ceil((bs.w + 2 * kBarMargin) * s), h = (int)std::ceil((bs.h + 2 * kBarMargin) * s);
    ID2D1RenderTarget *rt = surface_.Begin(w, h, dpi_);
    if (!rt) return;
    {
        Canvas c(rt);
        view.Draw(c, RectF(kBarMargin, kBarMargin, bs.w, bs.h));
    }
    int m = (int)std::lround(kBarMargin * s);
    surface_.End(hwnd_, pos_.x - m, pos_.y - m, alpha_);
}

void BarWindow::Show() {
    if (!hwnd_) return;
    visible_ = true;
    alpha_ = 255;
    Render();
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ScheduleFade();
}

void BarWindow::Hide() {
    visible_ = false;
    tip_.Hide();
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_FADE_WAIT);
        KillTimer(hwnd_, TIMER_FADE);
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void BarWindow::Close() {
    tip_.DestroyWin();
    DestroyWin();
    visible_ = false;
}

void BarWindow::SetAutoFade(bool on) {
    autoFade_ = on;
    ScheduleFade();
}

void BarWindow::SetAlpha(BYTE a) {
    alpha_ = a;
    if (hwnd_ && visible_) LayeredSurface::SetAlpha(hwnd_, a);
}

void BarWindow::ScheduleFade() {
    if (!hwnd_) return;
    KillTimer(hwnd_, TIMER_FADE_WAIT);
    KillTimer(hwnd_, TIMER_FADE);
    if (!autoFade_ || hovering_ || !visible_) {
        if (alpha_ != 255) SetAlpha(255);
        return;
    }
    // 全屏录制：3 秒后在 animation.normalMs 内淡到 35%
    SetTimer(hwnd_, TIMER_FADE_WAIT, 3000, nullptr);
}

void BarWindow::SetHover(int id) {
    if (view.hover == id) return;
    view.hover = id;
    KillTimer(hwnd_, TIMER_TIP);
    tip_.Hide();
    tipFor_ = BAR_NONE;
    if (id) SetTimer(hwnd_, TIMER_TIP, 500, nullptr);
    Render();
}

LRESULT BarWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    float s = Scale();
    auto lx = [&](LPARAM l) { return MouseX(l) / s - kBarMargin; };
    auto ly = [&](LPARAM l) { return MouseY(l) / s - kBarMargin; };
    SizeF bs = view.Size();
    RectF bar(0, 0, bs.w, bs.h);
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
        return TRUE;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        if (!hovering_) {
            hovering_ = true;
            ScheduleFade();
        }
        int id = view.Hit(bar, lx(lp), ly(lp));
        if (view.pressed) id = id == view.pressed ? id : BAR_NONE;
        SetHover(id);
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        hovering_ = false;
        if (!view.pressed) SetHover(BAR_NONE);
        ScheduleFade();
        return 0;
    case WM_LBUTTONDOWN: {
        int id = view.Hit(bar, lx(lp), ly(lp));
        tip_.Hide();
        KillTimer(hwnd_, TIMER_TIP);
        if (id) {
            view.pressed = id;
            view.hover = id;
            SetCapture(hwnd_);
            Render();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int id = view.pressed;
        if (!id) return 0;
        view.pressed = BAR_NONE;
        if (GetCapture() == hwnd_) ReleaseCapture();
        bool inside = view.Hit(bar, lx(lp), ly(lp)) == id;
        view.hover = inside ? id : BAR_NONE;
        Render();
        if (inside && onButton) onButton(id);  // 可能关闭控制条
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd_ && view.pressed) {
            view.pressed = BAR_NONE;
            Render();
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TIP) {
            KillTimer(hwnd_, TIMER_TIP);
            int id = view.hover;
            if (id && !view.pressed && visible_) {
                RectF b = view.ButtonRect(bar, id);
                int m = (int)std::lround(kBarMargin * s);
                RECT a = {pos_.x - m + (LONG)std::floor((b.x + kBarMargin) * s), pos_.y - m + (LONG)std::floor((b.y + kBarMargin) * s),
                          pos_.x - m + (LONG)std::ceil((b.r() + kBarMargin) * s), pos_.y - m + (LONG)std::ceil((b.b() + kBarMargin) * s)};
                // 控制条在屏幕下半部时气泡放上方
                HMONITOR mon = MonitorFromPoint(pos_, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {};
                mi.cbSize = sizeof mi;
                GetMonitorInfoW(mon, &mi);
                bool below = tipSide ? tipSide > 0 : pos_.y < (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
                tip_.Show(BarView::Tooltip(id, view.paused), a, below, dpi_);
                tipFor_ = id;
            }
            return 0;
        }
        if (wp == TIMER_FADE_WAIT) {
            KillTimer(hwnd_, TIMER_FADE_WAIT);
            if (autoFade_ && !hovering_ && visible_) {
                fadeStart_ = NowSeconds();
                SetTimer(hwnd_, TIMER_FADE, 15, nullptr);
            }
            return 0;
        }
        if (wp == TIMER_FADE) {
            double ms = theme::Tok().animation.normalMs;
            double t = (NowSeconds() - fadeStart_) * 1000.0 / (ms > 0 ? ms : 200);
            if (t >= 1 || hovering_) {
                KillTimer(hwnd_, TIMER_FADE);
                t = hovering_ ? 0 : 1;
            }
            SetAlpha((BYTE)std::lround(255.0 * (1.0 - 0.65 * t)));
            return 0;
        }
        break;
    }
    return Window::Handle(msg, wp, lp);
}

// ---------------------------------------------------------------- 区域边框

void BorderWindows::Show(RECT r, UINT dpi) {
    Close();
    float s = dpi / 96.f;
    int pad = (int)std::ceil(4 * s);
    float bw = std::round(theme::Tok().recordBar.regionBorderWidth * s);
    if (bw < 1) bw = 1;
    // 四条：上、下、左、右（左右不含四角）
    RECT rc[4] = {{r.left - pad, r.top - pad, r.right + pad, r.top},
                  {r.left - pad, r.bottom, r.right + pad, r.bottom + pad},
                  {r.left - pad, r.top, r.left, r.bottom},
                  {r.right, r.top, r.right + pad, r.bottom}};
    // 虚线 6/4（以线宽为单位 3/2），平头，像素坐标绘制
    ComPtr<ID2D1StrokeStyle> dash;
    const float dashes[2] = {3.f, 2.f};
    D2D1_STROKE_STYLE_PROPERTIES p = {D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER,
                                      10.f, D2D1_DASH_STYLE_CUSTOM, 0.f};
    gfx::D2D()->CreateStrokeStyle(&p, dashes, 2, dash.Put());
    Color col = theme::Tok().recordBar.regionBorder;
    for (int i = 0; i < 4; ++i) {
        Strip &st = strips_[i];
        int w = rc[i].right - rc[i].left, h = rc[i].bottom - rc[i].top;
        if (w <= 0 || h <= 0) continue;
        if (!st.CreateWin(kChromeEx | WS_EX_TRANSPARENT, WS_POPUP, 0, 0, 1, 1, nullptr, L"LemonShot")) continue;
        api::ExcludeFromCapture(st.Hwnd());
        ID2D1RenderTarget *rt = st.surface.Begin(w, h, 96);
        if (!rt) continue;
        {
            Canvas c(rt);
            // 描边中线在区域外 bw/2 处：整条线都在区域外
            float ox = (float)(r.left - rc[i].left), oy = (float)(r.top - rc[i].top);
            RectF line(ox - bw / 2, oy - bw / 2, (float)(r.right - r.left) + bw, (float)(r.bottom - r.top) + bw);
            D2D1_RECT_F d = line.D2();
            rt->DrawRectangle(&d, c.Brush(col), bw, dash.Get());
        }
        st.surface.End(st.Hwnd(), rc[i].left, rc[i].top, 255);
        SetWindowPos(st.Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void BorderWindows::Close() {
    for (auto &s : strips_) s.DestroyWin();
}

// ---------------------------------------------------------------- 倒计时

bool CountdownWindow::Start(RECT region, UINT dpi, int seconds) {
    region_ = region;
    dpi_ = dpi;
    remaining_ = seconds;
    if (!hwnd_) {
        if (!CreateWin(kChromeEx | WS_EX_TRANSPARENT, WS_POPUP, 0, 0, 1, 1, nullptr, L"LemonShot")) return false;
        excluded_ = api::ExcludeFromCapture(hwnd_);
    }
    // Esc 取消：倒计时期间临时注册全局热键（不抢焦点）；被别的程序占用时改为轮询按键状态
    hotkey_ = RegisterHotKey(hwnd_, 1, 0, VK_ESCAPE) != 0;
    if (!hotkey_) SetTimer(hwnd_, 3, 30, nullptr);
    Render();
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(hwnd_, 1, 1000, nullptr);
    return true;
}

void CountdownWindow::Render() {
    float s = dpi_ / 96.f;
    float size = theme::Tok().recordBar.countdownSize;
    int px = (int)std::ceil(size * s);
    ID2D1RenderTarget *rt = surface_.Begin(px, px, dpi_);
    if (!rt) return;
    {
        Canvas c(rt);
        c.FillEllipse(size / 2, size / 2, size / 2, size / 2, theme::Cap().tipBg);
        TextStyle ts;
        ts.size = size * 0.45f;
        ts.weight = kSemibold;
        ts.color = Color{1, 1, 1, 1};
        ts.h = HAlign::Center;
        ts.v = VAlign::Center;
        ts.tabular = true;
        c.Text(FormatW(L"%d", remaining_), RectF(0, 0, size, size), ts);
    }
    surface_.End(hwnd_, (region_.left + region_.right) / 2 - px / 2, (region_.top + region_.bottom) / 2 - px / 2, 255);
}

void CountdownWindow::Close() {
    if (hwnd_) {
        KillTimer(hwnd_, 1);
        KillTimer(hwnd_, 2);
        KillTimer(hwnd_, 3);
        if (hotkey_) UnregisterHotKey(hwnd_, 1);
    }
    hotkey_ = false;
    DestroyWin();
}

LRESULT CountdownWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == 1) {
            std::function<void()> cb = onCancel;
            Close();
            if (cb) cb();
        }
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            --remaining_;
            if (remaining_ > 0) {
                Render();
                return 0;
            }
            KillTimer(hwnd_, 1);
            KillTimer(hwnd_, 3);
            if (hotkey_) UnregisterHotKey(hwnd_, 1);
            hotkey_ = false;
            ShowWindow(hwnd_, SW_HIDE);
            if (excluded_) {
                std::function<void()> cb = onDone;
                Close();
                if (cb) cb();
            } else {
                // 排除不了的系统：等倒计时窗口从屏幕上消失再开始，免得录进第一帧
                SetTimer(hwnd_, 2, 120, nullptr);
            }
            return 0;
        }
        if (wp == 2) {
            std::function<void()> cb = onDone;
            Close();
            if (cb) cb();
            return 0;
        }
        if (wp == 3) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                std::function<void()> cb = onCancel;
                Close();
                if (cb) cb();
            }
            return 0;
        }
        break;
    }
    return Window::Handle(msg, wp, lp);
}

// ---------------------------------------------------------------- GIF 进度

bool ProgressWindow::Show(HMONITOR mon, const std::wstring &text) {
    text_ = text;
    if (!hwnd_) {
        if (!CreateWin(kChromeEx | WS_EX_TRANSPARENT, WS_POPUP, 0, 0, 1, 1, nullptr, L"LemonShot")) return false;
        api::ExcludeFromCapture(hwnd_);
    }
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    dpi_ = api::MonitorDpi(mon);
    float s = dpi_ / 96.f;
    SizeF sz = ProgressSize();
    int w = (int)std::ceil(sz.w * s), h = (int)std::ceil(sz.h * s);
    pos_ = POINT{(mi.rcWork.left + mi.rcWork.right) / 2 - w / 2, mi.rcWork.bottom - (int)std::lround(80 * s) - h};
    Render();
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void ProgressWindow::SetProgress(double v) {
    progress_ = v;
    if (hwnd_) Render();
}

void ProgressWindow::Render() {
    float s = dpi_ / 96.f;
    SizeF sz = ProgressSize();
    ID2D1RenderTarget *rt = surface_.Begin((int)std::ceil(sz.w * s), (int)std::ceil(sz.h * s), dpi_);
    if (!rt) return;
    {
        Canvas c(rt);
        DrawProgress(c, RectF(0, 0, sz.w, sz.h), text_, progress_);
    }
    surface_.End(hwnd_, pos_.x, pos_.y, 255);
}

void ProgressWindow::Close() { DestroyWin(); }

}  // namespace rec
}  // namespace ls
