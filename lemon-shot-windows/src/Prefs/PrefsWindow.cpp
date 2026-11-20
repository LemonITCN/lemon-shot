// 偏好设置窗口（spec 11）：固定 760×520 客户区，左侧边栏，右侧可滚动内容；控件全部自绘，文本输入时临时叠一个原生 EDIT。
#include <windows.h>
#include <commctrl.h>

#include <cmath>
#include <memory>

#include "App/App.h"
#include "Prefs/Prefs.h"
#include "Prefs/Widgets.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/Util.h"

namespace ls {
namespace prefs {

namespace {

const float kWinW = 760, kWinH = 520;
enum { ID_EDIT = 200, END_EDIT = 1 };

float SidebarW() { return theme::Tok().window.sidebarWidth; }

RectF SideItem(int i) {
    const auto &w = theme::Tok().window;
    float top = w.titleBarHeight + 4;
    return RectF(10, top + i * (w.sidebarItemHeight + 2), SidebarW() - 20, w.sidebarItemHeight);
}

void DrawSidebar(Canvas &c, float h, const std::string &selected, int hover) {
    const auto &u = theme::Ui();
    const auto &w = theme::Tok().window;
    c.FillRect(RectF(0, 0, SidebarW(), h), u.windowBg);
    c.FillRect(RectF(SidebarW() - 1, 0, 1, h), u.border);
    float iconS = w.sidebarIcon;
    const auto &pages = Pages();
    for (int i = 0; i < (int)pages.size(); ++i) {
        RectF r = SideItem(i);
        bool sel = selected == pages[(size_t)i].id;
        if (sel)
            c.FillRound(r, w.sidebarItemRadius, u.surfaceSelected);
        else if (i == hover)
            c.FillRound(r, w.sidebarItemRadius, u.surfaceHover);
        c.Icon(pages[(size_t)i].icon, r.x + 10 + iconS / 2, r.cy(), iconS, sel ? u.iconActive : u.icon, 1.6f);
        TextStyle ts;
        ts.size = theme::Tok().fontSize.body;
        ts.weight = sel ? kMedium : kRegular;
        ts.color = u.text;
        ts.ellipsis = true;
        c.Text(T(pages[(size_t)i].titleKey), RectF(r.x + 10 + iconS + 10, r.y, r.w - iconS - 30, r.h), ts);
    }
}

void DestroyLater(void *p);

class PrefsWindow : public Window, public Host {
public:
    std::string page = "general";
    std::unique_ptr<FormPage> form;
    float contentH = 0, scrollY = 0;
    ComPtr<ID2D1HwndRenderTarget> rt;
    UINT dpi = 96;
    Widget *hover = nullptr, *pressed = nullptr, *focus = nullptr;
    int sideHover = -1;
    bool tracking = false;
    bool rebuildPending = false;
    HWND edit = nullptr;
    HFONT editFont = nullptr;
    HBRUSH editBrush = nullptr;
    std::function<void(const std::wstring &, bool)> editCb;

    ~PrefsWindow() override {
        if (editFont) DeleteObject(editFont);
        if (editBrush) DeleteObject(editBrush);
    }

    float S() const { return dpi / 96.f; }

    void Open(const char *p) {
        if (p) {
            for (auto &pg : Pages())
                if (std::string(p) == pg.id) page = p;
        }
        if (!hwnd_) {
            POINT pt;
            GetCursorPos(&pt);
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            dpi = api::MonitorDpi(mon);
            MONITORINFO mi = {};
            mi.cbSize = sizeof mi;
            GetMonitorInfoW(mon, &mi);
            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
            RECT rc = {0, 0, (LONG)std::lround(kWinW * S()), (LONG)std::lround(kWinH * S())};
            api::AdjustRectForDpi(&rc, style, 0, dpi);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
            if (!CreateWin(0, style, x, y, w, h, nullptr, T("pref.title").c_str())) return;
            UINT actual = api::WindowDpi(hwnd_);
            if (actual != dpi) {
                dpi = actual;
                RECT r2 = {0, 0, (LONG)std::lround(kWinW * S()), (LONG)std::lround(kWinH * S())};
                api::AdjustRectForDpi(&r2, style, 0, dpi);
                SetWindowPos(hwnd_, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top, SWP_NOMOVE | SWP_NOZORDER);
            }
            api::SetDarkTitleBar(hwnd_, theme::IsDark());
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)app::AppIcon(32));
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)app::AppIcon(16));
            Rebuild(true);
        } else if (p) {
            Rebuild(true);
        }
        ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd_);
        Invalidate();
    }

    void Rebuild(bool resetScroll) {
        rebuildPending = false;
        CloseAllPopups();
        EndEdit();
        SetFocusWidget(nullptr);
        hover = pressed = nullptr;
        form.reset(BuildPage(page));
        form->frame = RectF(SidebarW(), 0, kWinW - SidebarW(), 0);
        contentH = form->LayoutPage(kWinW - SidebarW());
        form->frame.h = std::fmax(kWinH, contentH);
        form->SetHost(this);
        if (resetScroll) scrollY = 0;
        ClampScroll();
        if (hwnd_) SetWindowTextW(hwnd_, T("pref.title").c_str());
        Invalidate();
    }

    void ClampScroll() {
        float maxS = contentH - ViewH();
        if (maxS < 0) maxS = 0;
        scrollY = Clamp(scrollY, 0.f, maxS);
    }

    float ViewH() {
        if (!hwnd_) return kWinH;
        RECT rc;
        GetClientRect(hwnd_, &rc);
        return rc.bottom / S();
    }

    void Paint() {
        if (!rt) {
            rt = gfx::CreateHwndTarget(hwnd_, dpi);
            if (!rt) return;
        }
        rt->BeginDraw();
        {
            Canvas c(rt.Get());
            const auto &u = theme::Ui();
            c.SetTransform(MatIdentity());
            c.Clear(u.surface);
            c.SetTransform(MatTranslate(0, -scrollY));
            if (form) form->Draw(c);
            c.SetTransform(MatIdentity());
            float vh = ViewH();
            DrawSidebar(c, vh, page, sideHover);
            // 内容可滚动时右侧细滚动条
            if (contentH > vh + 1) {
                float track = vh - 8;
                float th = std::fmax(30.f, track * vh / contentH);
                float ty = 4 + (track - th) * (scrollY / (contentH - vh));
                c.FillRound(RectF(kWinW - 7, ty, 4, th), 2, WithAlpha(u.textTertiary, 0.6f));
            }
        }
        if (rt->EndDraw(nullptr, nullptr) == (HRESULT)D2DERR_RECREATE_TARGET) {
            rt.Reset();
            Invalidate();
        }
    }

    // ---------------------------------------------------------------- Host
    void Invalidate() override {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    HWND Hwnd() override { return hwnd_; }
    UINT Dpi() override { return dpi; }
    RECT ScreenRect(RectF r) override {
        float s = S();
        POINT a = {(LONG)std::floor(r.x * s), (LONG)std::floor((r.y - scrollY) * s)};
        POINT b = {(LONG)std::ceil(r.r() * s), (LONG)std::ceil((r.b() - scrollY) * s)};
        ClientToScreen(hwnd_, &a);
        ClientToScreen(hwnd_, &b);
        return RECT{a.x, a.y, b.x, b.y};
    }
    void BeginEdit(RectF r, const std::wstring &text, bool secure, bool center,
                   std::function<void(const std::wstring &, bool)> cb) override {
        EndEdit();
        float s = S();
        float fs = theme::Tok().fontSize.body;
        if (!editFont)
            editFont = CreateFontW(-(int)std::lround(fs * s), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, gfx::FontFamily().c_str());
        int h = (int)std::ceil((fs + 6) * s);
        int x = (int)std::floor(r.x * s), y = (int)std::lround((r.cy() - scrollY) * s) - h / 2;
        DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (secure ? ES_PASSWORD : 0) | (center ? ES_CENTER : 0);
        edit = CreateWindowExW(0, L"EDIT", text.c_str(), style, x, y, (int)std::ceil(r.w * s), h, hwnd_,
                               (HMENU)(INT_PTR)ID_EDIT, app::Instance(), nullptr);
        if (!edit) return;
        editCb = cb;
        SetWindowSubclass(edit, &PrefsWindow::EditProc, ID_EDIT, 0);
        SendMessageW(edit, WM_SETFONT, (WPARAM)editFont, FALSE);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
    }
    void EndEdit() override {
        if (!edit) return;
        HWND e = edit;
        edit = nullptr;
        int len = GetWindowTextLengthW(e);
        std::wstring t((size_t)len + 1, L'\0');
        GetWindowTextW(e, &t[0], len + 1);
        t.resize((size_t)len);
        auto cb = editCb;
        editCb = nullptr;
        DestroyWindow(e);
        if (cb) cb(t, true);
        Invalidate();
    }
    void SetFocusWidget(Widget *w) override {
        if (focus == w) return;
        Widget *old = focus;
        focus = w;
        if (old) old->OnFocus(false);
        if (w) w->OnFocus(true);
    }
    Widget *FocusWidget() override { return focus; }
    void RequestRebuild() override {
        if (rebuildPending) return;
        rebuildPending = true;
        app::Defer(
            [](void *p) {
                PrefsWindow *w = (PrefsWindow *)p;
                if (w->rebuildPending && w->Hwnd()) w->Rebuild(false);
            },
            this);
    }

    static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
        switch (m) {
        case WM_KEYDOWN:
            if (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB) {
                PostMessageW(GetParent(h), WM_LS_END_TEXT, END_EDIT, (LPARAM)h);
                return 0;
            }
            break;
        case WM_CHAR:
            if (w == 13 || w == 27 || w == 9) return 0;
            break;
        case WM_KILLFOCUS: PostMessageW(GetParent(h), WM_LS_END_TEXT, END_EDIT, (LPARAM)h); break;
        case WM_NCDESTROY: RemoveWindowSubclass(h, &PrefsWindow::EditProc, id); break;
        }
        return DefSubclassProc(h, m, w, l);
    }

    // ---------------------------------------------------------------- 输入
    void Content(LPARAM lp, float &x, float &y) {
        x = MouseX(lp) / S();
        y = MouseY(lp) / S() + scrollY;
    }

    int SideHit(LPARAM lp) {
        float x = MouseX(lp) / S(), y = MouseY(lp) / S();
        if (x >= SidebarW()) return -1;
        for (int i = 0; i < (int)Pages().size(); ++i)
            if (SideItem(i).Contains(x, y)) return i;
        return -1;
    }

    void SetHover(Widget *w) {
        if (w == hover) return;
        if (hover) {
            hover->hovered = false;
            hover->OnMouseLeave();
        }
        hover = w;
        if (hover) hover->hovered = true;
        Invalidate();
    }

    void OnDestroyed() override;

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        switch (msg) {
        case WM_NCCREATE: api::EnableNonClientScaling(hwnd_); break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (rt) {
                D2D1_SIZE_U sz = {LOWORD(lp), HIWORD(lp)};
                rt->Resize(&sz);
            }
            ClampScroll();
            return 0;
        case WM_DPICHANGED: {
            dpi = HIWORD(wp);
            RECT *r = (RECT *)lp;
            EndEdit();
            if (editFont) {
                DeleteObject(editFont);
                editFont = nullptr;
            }
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (rt) rt->SetDpi((float)dpi, (float)dpi);
            Invalidate();
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, hover ? hover->Cursor() : MAKEINTRESOURCEW(32512)));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            if (!tracking) {
                TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking = true;
            }
            float x, y;
            Content(lp, x, y);
            if (pressed) {
                pressed->OnMouseDrag(x, y);
                return 0;
            }
            int sh = SideHit(lp);
            if (sh != sideHover) {
                sideHover = sh;
                Invalidate();
            }
            Widget *w = (MouseX(lp) / S() >= SidebarW() && form) ? form->HitTest(x, y) : nullptr;
            SetHover(w);
            if (w) w->OnMouseMove(x, y);
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            sideHover = -1;
            if (!pressed) SetHover(nullptr);
            Invalidate();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            if (edit) EndEdit();
            int sh = SideHit(lp);
            if (sh >= 0) {
                std::string id = Pages()[(size_t)sh].id;
                if (id != page) {
                    page = id;
                    Rebuild(true);
                }
                return 0;
            }
            float x, y;
            Content(lp, x, y);
            Widget *w = form ? form->HitTest(x, y) : nullptr;
            if (focus && focus != w) SetFocusWidget(nullptr);
            if (!w) return 0;
            pressed = w;
            w->pressed = true;
            SetCapture(hwnd_);
            w->OnMouseDown(x, y);
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!pressed) return 0;
            Widget *w = pressed;
            pressed = nullptr;
            w->pressed = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
            float x, y;
            Content(lp, x, y);
            w->OnMouseUp(x, y, w->frame.Contains(x, y));  // 可能触发重建（延迟执行）
            Invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pressed && (HWND)lp != hwnd_) {
                pressed->pressed = false;
                pressed = nullptr;
            }
            return 0;
        case WM_MOUSEWHEEL: {
            float dy = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (hover && hover->OnWheel(dy)) return 0;
            if (edit) EndEdit();
            CloseAllPopups();
            scrollY -= dy * 60.f;
            ClampScroll();
            Invalidate();
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (focus && focus->OnKey((UINT)wp, true)) return 0;
            if (wp == VK_ESCAPE) CloseAllPopups();
            if (msg == WM_SYSKEYDOWN) break;
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (focus && focus->OnKey((UINT)wp, false)) return 0;
            if (msg == WM_SYSKEYUP) break;
            return 0;
        case WM_SYSCHAR:
            if (focus) return 0;
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                if (focus) SetFocusWidget(nullptr);
                HideInfoBubble();
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wp) == ID_EDIT && HIWORD(wp) == EN_CHANGE && edit && editCb) {
                int len = GetWindowTextLengthW(edit);
                std::wstring t((size_t)len + 1, L'\0');
                GetWindowTextW(edit, &t[0], len + 1);
                t.resize((size_t)len);
                editCb(t, false);
            }
            return 0;
        case WM_CTLCOLOREDIT:
            if ((HWND)lp == edit) {
                const auto &u = theme::Ui();
                auto ref = [](Color c) { return RGB((int)(c.r * 255 + 0.5f), (int)(c.g * 255 + 0.5f), (int)(c.b * 255 + 0.5f)); };
                if (editBrush) DeleteObject(editBrush);
                editBrush = CreateSolidBrush(ref(u.inputBg));
                SetTextColor((HDC)wp, ref(u.text));
                SetBkColor((HDC)wp, ref(u.inputBg));
                return (LRESULT)editBrush;
            }
            break;
        case WM_LS_END_TEXT:
            if (edit && (HWND)lp == edit) EndEdit();
            return 0;
        case WM_CLOSE:
            EndEdit();
            SetFocusWidget(nullptr);
            CloseAllPopups();
            DestroyWindow(hwnd_);
            return 0;
        }
        return Window::Handle(msg, wp, lp);
    }
};

PrefsWindow *g_win = nullptr;

void PrefsWindow::OnDestroyed() {
    if (g_win == this) g_win = nullptr;
    app::Defer(&DestroyLater, this);
}

void DestroyLater(void *p) { delete (PrefsWindow *)p; }

}  // namespace

void Show(const char *page) {
    if (!g_win) g_win = new PrefsWindow();
    g_win->Open(page);
}

void Close() {
    if (g_win && g_win->Hwnd()) SendMessageW(g_win->Hwnd(), WM_CLOSE, 0, 0);
}

void Refresh() {
    if (!g_win || !g_win->Hwnd()) return;
    api::SetDarkTitleBar(g_win->Hwnd(), theme::IsDark());
    g_win->RequestRebuild();
}

bool IsOpen() { return g_win && g_win->Hwnd(); }

bool IsRecordingShortcut() { return IsRecordingNow(); }

void OnSettingChanged(const std::string &path) {
    if (!g_win || !g_win->Hwnd()) return;
    if (NeedsRebuild(g_win->page, path))
        g_win->RequestRebuild();
    else
        g_win->Invalidate();
}

std::vector<std::string> PageIds() {
    std::vector<std::string> v;
    for (auto &p : Pages()) v.push_back(p.id);
    return v;
}

SizeF PageSize(const std::string &page) {
    std::unique_ptr<FormPage> f(BuildPage(page));
    f->frame = RectF(SidebarW(), 0, kWinW - SidebarW(), 0);
    float h = f->LayoutPage(kWinW - SidebarW());
    return SizeF{kWinW, std::ceil(std::fmax(kWinH, h))};
}

void RenderPage(Canvas &c, const std::string &page) {
    std::unique_ptr<FormPage> f(BuildPage(page));
    f->frame = RectF(SidebarW(), 0, kWinW - SidebarW(), 0);
    float h = std::ceil(std::fmax(kWinH, f->LayoutPage(kWinW - SidebarW())));
    f->frame.h = h;
    c.FillRect(RectF(SidebarW(), 0, kWinW - SidebarW(), h), theme::Ui().surface);
    f->Draw(c);
    DrawSidebar(c, h, page, -1);
}

}  // namespace prefs
}  // namespace ls
