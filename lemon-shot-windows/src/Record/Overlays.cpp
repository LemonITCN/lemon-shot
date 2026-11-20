#include "Record/Overlays.h"

#include <cmath>
#include <cstring>
#include <cwctype>

#include "Record/Hooks.h"
#include "Settings/Settings.h"
#include "Util/Api.h"
#include "Util/ComPtr.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace rec {

namespace {

const DWORD kOverlayEx = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
const float kPenWidth = 4;

// 让窗口拿到前台（与截图层同样的 AttachThreadInput 技巧）
void TakeForeground(HWND hwnd) {
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    bool attached = fgThread && fgThread != me && AttachThreadInput(fgThread, me, TRUE);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (attached) AttachThreadInput(fgThread, me, FALSE);
}

}  // namespace

// ---------------------------------------------------------------- 画笔

PenWindow::~PenWindow() { Close(); }

void PenWindow::FreeDib() {
    if (dc_) {
        if (old_) SelectObject(dc_, old_);
        DeleteDC(dc_);
    }
    if (dib_) DeleteObject(dib_);
    dc_ = nullptr;
    dib_ = nullptr;
    old_ = nullptr;
    bits_ = nullptr;
}

bool PenWindow::Open(RECT region, UINT dpi, Color color) {
    Close();
    region_ = region;
    dpi_ = dpi;
    scale_ = dpi / 96.f;
    color_ = color;
    int w = region.right - region.left, h = region.bottom - region.top;
    if (w <= 0 || h <= 0) return false;
    if (!off_.Create(w, h, scale_)) return false;
    BITMAPINFO bi;
    std::memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc_ = CreateCompatibleDC(nullptr);
    if (!dc_) return false;
    dib_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!dib_ || !bits_) {
        FreeDib();
        return false;
    }
    old_ = SelectObject(dc_, dib_);
    // 不带 WS_EX_NOACTIVATE：要拿键盘焦点接收 Esc、Ctrl+Z
    if (!CreateWin(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP, region.left, region.top, w, h, nullptr,
                   L"LemonShot")) {
        FreeDib();
        return false;
    }
    strokes_.clear();
    cur_.clear();
    drawing_ = false;
    RedrawAll();
    previous_ = GetForegroundWindow();
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    TakeForeground(hwnd_);
    return true;
}

void PenWindow::Close() {
    if (hwnd_) {
        bool wasForeground = GetForegroundWindow() == hwnd_;
        DestroyWin();
        if (wasForeground && previous_ && IsWindow(previous_)) SetForegroundWindow(previous_);
    }
    previous_ = nullptr;
    FreeDib();
    strokes_.clear();
    cur_.clear();
    drawing_ = false;
}

// WIC 位图（持久内容）→ DIB 的脏矩形 → 分层窗口
void PenWindow::Present(const RECT *dirty) {
    if (!hwnd_ || !bits_) return;
    int w = region_.right - region_.left, h = region_.bottom - region_.top;
    RECT all = {0, 0, w, h};
    RECT d = all;
    if (dirty && !IntersectRect(&d, dirty, &all)) return;
    WICRect wr = {d.left, d.top, d.right - d.left, d.bottom - d.top};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(off_.Wic()->Lock(&wr, WICBitmapLockRead, lock.Put()))) return;
    UINT stride = 0, size = 0;
    BYTE *p = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&size, &p);
    if (!p) return;
    uint8_t *dst = (uint8_t *)bits_;
    for (int y = 0; y < wr.Height; ++y)
        std::memcpy(dst + ((size_t)(d.top + y) * w + d.left) * 4, p + (size_t)y * stride, (size_t)wr.Width * 4);
    lock.Reset();
    GdiFlush();
    POINT pos = {region_.left, region_.top}, src = {0, 0};
    SIZE sz = {w, h};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UPDATELAYEREDWINDOWINFO info;
    std::memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    info.pptDst = &pos;
    info.psize = &sz;
    info.hdcSrc = dc_;
    info.pptSrc = &src;
    info.pblend = &bf;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = dirty ? &d : nullptr;
    UpdateLayeredWindowIndirect(hwnd_, &info);
}

RECT PenWindow::DirtyFor(const D2D1_POINT_2F *pts, int n) const {
    float x0 = pts[0].x, y0 = pts[0].y, x1 = x0, y1 = y0;
    for (int i = 1; i < n; ++i) {
        x0 = std::fmin(x0, pts[i].x);
        y0 = std::fmin(y0, pts[i].y);
        x1 = std::fmax(x1, pts[i].x);
        y1 = std::fmax(y1, pts[i].y);
    }
    float pad = kPenWidth / 2 + 2;
    RECT r = {(LONG)std::floor((x0 - pad) * scale_), (LONG)std::floor((y0 - pad) * scale_), (LONG)std::ceil((x1 + pad) * scale_),
              (LONG)std::ceil((y1 + pad) * scale_)};
    return r;
}

static void StrokePath(ID2D1RenderTarget *rt, ID2D1Brush *brush, const std::vector<D2D1_POINT_2F> &s, bool finish) {
    if (s.empty()) return;
    if (s.size() == 1) {
        D2D1_ELLIPSE e = {s[0], kPenWidth / 2, kPenWidth / 2};
        rt->FillEllipse(&e, brush);
        return;
    }
    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(gfx::D2D()->CreatePathGeometry(geo.Put()))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(sink.Put()))) return;
    sink->BeginFigure(s[0], D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < s.size(); ++i) {
        D2D1_POINT_2F mid = {(s[i - 1].x + s[i].x) / 2, (s[i - 1].y + s[i].y) / 2};
        D2D1_QUADRATIC_BEZIER_SEGMENT q = {s[i - 1], mid};
        sink->AddQuadraticBezier(&q);
    }
    if (finish) sink->AddLine(s.back());
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    rt->DrawGeometry(geo.Get(), brush, kPenWidth, gfx::RoundStroke());
}

void PenWindow::RedrawAll() {
    ID2D1RenderTarget *rt = off_.RT();
    if (!rt) return;
    rt->BeginDraw();
    {
        Canvas c(rt);
        c.Clear(Color{0, 0, 0, 1.f / 255.f});
        for (auto &s : strokes_) StrokePath(rt, c.Brush(color_), s, true);
        StrokePath(rt, c.Brush(color_), cur_, false);
    }
    rt->EndDraw(nullptr, nullptr);
    Present(nullptr);
}

// 只画最新的一小段（中点二次平滑，与整条重画结果一致）
void PenWindow::DrawLatest(bool finish) {
    ID2D1RenderTarget *rt = off_.RT();
    size_t n = cur_.size();
    if (!rt || n == 0) return;
    std::vector<D2D1_POINT_2F> seg;
    if (finish) {
        if (n < 2) return;
        seg = {Mid(cur_[n - 2], cur_[n - 1]), cur_[n - 1]};
    } else if (n == 1) {
        seg = {cur_[0]};
    } else if (n == 2) {
        seg = {cur_[0], Mid(cur_[0], cur_[1])};
    } else {
        seg = {Mid(cur_[n - 3], cur_[n - 2]), cur_[n - 2], Mid(cur_[n - 2], cur_[n - 1])};
    }
    rt->BeginDraw();
    {
        Canvas c(rt);
        ID2D1Brush *b = c.Brush(color_);
        if (seg.size() == 1) {
            D2D1_ELLIPSE e = {seg[0], kPenWidth / 2, kPenWidth / 2};
            rt->FillEllipse(&e, b);
        } else if (seg.size() == 2) {
            rt->DrawLine(seg[0], seg[1], b, kPenWidth, gfx::RoundStroke());
        } else {
            ComPtr<ID2D1PathGeometry> geo;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(gfx::D2D()->CreatePathGeometry(geo.Put())) && SUCCEEDED(geo->Open(sink.Put()))) {
                sink->BeginFigure(seg[0], D2D1_FIGURE_BEGIN_HOLLOW);
                D2D1_QUADRATIC_BEZIER_SEGMENT q = {seg[1], seg[2]};
                sink->AddQuadraticBezier(&q);
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                if (SUCCEEDED(sink->Close())) rt->DrawGeometry(geo.Get(), b, kPenWidth, gfx::RoundStroke());
            }
        }
    }
    rt->EndDraw(nullptr, nullptr);
    RECT dirty = DirtyFor(seg.data(), (int)seg.size());
    Present(&dirty);
}

LRESULT PenWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32515) /* IDC_CROSS */));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        SetCapture(hwnd_);
        drawing_ = true;
        cur_.clear();
        cur_.push_back(D2D1_POINT_2F{MouseX(lp) / scale_, MouseY(lp) / scale_});
        DrawLatest(false);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (drawing_) {
            D2D1_POINT_2F p = {MouseX(lp) / scale_, MouseY(lp) / scale_};
            D2D1_POINT_2F last = cur_.back();
            if (std::hypot(p.x - last.x, p.y - last.y) < 1) return 0;
            cur_.push_back(p);
            DrawLatest(false);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (drawing_) {
            drawing_ = false;
            DrawLatest(true);
            strokes_.push_back(cur_);
            cur_.clear();
            if (msg == WM_LBUTTONUP && GetCapture() == hwnd_) ReleaseCapture();
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (onEscape) onEscape();  // 调用方会关掉本窗口
            return 0;
        }
        if (wp == 'Z' && GetKeyState(VK_CONTROL) < 0) {
            if (!drawing_ && !strokes_.empty()) {
                strokes_.pop_back();
                RedrawAll();
            }
            return 0;
        }
        return 0;
    case WM_CLOSE:
        if (onEscape) onEscape();
        return 0;
    }
    return Window::Handle(msg, wp, lp);
}

// ---------------------------------------------------------------- 点击扩散圆

ClickRipples::~ClickRipples() { Close(); }

bool ClickRipples::Init(HMONITOR mon) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    mon_ = mi.rcMonitor;
    dpi_ = api::MonitorDpi(mon);
    if (!hwnd_ && !CreateWin(0, 0, 0, 0, 0, 0, HWND_MESSAGE, L"")) return false;
    return true;
}

void ClickRipples::Add(int x, int y, bool right) {
    if (!hwnd_) return;
    POINT pt = {x, y};
    if (!PtInRect(&mon_, pt)) return;  // 只在录制的那块屏上
    Ripple *r = nullptr;
    for (auto &p : pool_)
        if (!p->busy) r = p.get();
    if (!r) {
        // 最多 8 个同时显示：再多就复用最旧的
        if (pool_.size() >= 8) {
            Ripple *oldest = pool_[0].get();
            for (auto &p : pool_)
                if (p->start < oldest->start) oldest = p.get();
            r = oldest;
        } else {
            pool_.emplace_back(new Ripple());
            r = pool_.back().get();
            if (!r->CreateWin(kOverlayEx | WS_EX_TRANSPARENT, WS_POPUP, 0, 0, 1, 1, nullptr, L"LemonShot")) return;
        }
    }
    r->busy = true;
    r->right = right;
    r->center = pt;
    r->dpi = dpi_;
    r->start = NowSeconds();
    Render(*r, 0);
    SetWindowPos(r->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetTimer(hwnd_, 1, 15, nullptr);
}

void ClickRipples::Render(Ripple &r, double t) {
    const OverlayTokens &o = OTok();
    float s = r.dpi / 96.f;
    float radius = o.clickRadius;
    int px = (int)std::ceil(radius * 2 * s) + 2;
    // 先快后慢（ease-out）：缩放 0.3 → 1，不透明度 1 → 0
    double e = 1.0 - (1.0 - t) * (1.0 - t);
    float k = (float)(0.3 + 0.7 * e);
    float a = (float)(1.0 - e);
    ID2D1RenderTarget *rt = r.surface.Begin(px, px, r.dpi);
    if (!rt) return;
    {
        Canvas c(rt);
        Color col = r.right ? o.clickRightColor : o.clickColor;
        float half = px / s / 2;
        c.FillEllipse(half, half, radius * k, radius * k, WithAlpha(col, a));
    }
    r.surface.End(r.Hwnd(), r.center.x - px / 2, r.center.y - px / 2, 255);
}

void ClickRipples::Close() {
    if (hwnd_) KillTimer(hwnd_, 1);
    pool_.clear();
    DestroyWin();
}

LRESULT ClickRipples::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == 1) {
        double dur = OTok().clickMs / 1000.0;
        if (dur <= 0) dur = 0.45;
        double now = NowSeconds();
        bool any = false;
        for (auto &p : pool_) {
            if (!p->busy) continue;
            double t = (now - p->start) / dur;
            if (t >= 1) {
                p->busy = false;
                ShowWindow(p->Hwnd(), SW_HIDE);
                continue;
            }
            any = true;
            Render(*p, t);
        }
        if (!any) KillTimer(hwnd_, 1);
        return 0;
    }
    return Window::Handle(msg, wp, lp);
}

// ---------------------------------------------------------------- 按键浮层

bool KeysWindow::Init(RECT region, UINT dpi) {
    region_ = region;
    dpi_ = dpi;
    maxKeys_ = Clamp(settings::Int("record.keys.maxKeys"), 1, 20);
    fade_ = settings::Num("record.keys.fadeMs") / 1000.0;
    if (fade_ < 0.2) fade_ = 0.2;
    onlyShortcuts_ = settings::Bool("record.keys.onlyShortcuts");
    hideRepeats_ = settings::Bool("record.keys.hideRepeats");
    view_.position = settings::Str("record.keys.position");
    std::string sz = settings::Str("record.keys.size");
    view_.size = (sz == "small" || sz == "large") ? sz : "medium";
    std::string th = settings::Str("record.keys.theme");
    view_.dark = th == "dark" ? true : (th == "light" ? false : theme::IsDark());
    if (!hwnd_ && !CreateWin(kOverlayEx | WS_EX_TRANSPARENT, WS_POPUP, 0, 0, 1, 1, nullptr, L"LemonShot")) return false;
    return true;
}

std::wstring KeysWindow::ComboText(UINT vk, int mods, UINT scan, bool ext, bool *hasModifier) {
    std::wstring key;
    bool printable = true;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        key = std::wstring(1, (wchar_t)vk);
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        key = std::wstring(1, (wchar_t)('0' + vk - VK_NUMPAD0));
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        key = FormatW(L"F%u", vk - VK_F1 + 1);
        printable = false;
    } else {
        struct Name {
            UINT vk;
            const wchar_t *name;
        };
        static const Name names[] = {
            {VK_RETURN, L"Enter"},   {VK_ESCAPE, L"Esc"},       {VK_TAB, L"Tab"},         {VK_SPACE, L"Space"},
            {VK_BACK, L"Backspace"}, {VK_DELETE, L"Delete"},    {VK_INSERT, L"Insert"},   {VK_HOME, L"Home"},
            {VK_END, L"End"},        {VK_PRIOR, L"PageUp"},     {VK_NEXT, L"PageDown"},   {VK_LEFT, L"←"},
            {VK_RIGHT, L"→"},   {VK_UP, L"↑"},        {VK_DOWN, L"↓"},     {VK_CAPITAL, L"CapsLock"},
            {VK_SNAPSHOT, L"PrintScreen"}, {VK_PAUSE, L"Pause"}, {VK_APPS, L"Menu"},      {VK_NUMLOCK, L"NumLock"},
            {VK_SCROLL, L"ScrollLock"},
        };
        for (auto &n : names) {
            if (n.vk == vk) {
                key = n.name;
                printable = false;
                break;
            }
        }
        if (key.empty()) {
            UINT ch = MapVirtualKeyW(vk, 2 /* MAPVK_VK_TO_CHAR */) & 0x7FFF;
            if (ch >= 0x20) {
                key = std::wstring(1, (wchar_t)towupper((wint_t)ch));
            } else {
                wchar_t buf[64] = {};
                LONG lp = (LONG)((scan & 0xFF) << 16) | (ext ? (1L << 24) : 0);
                if (GetKeyNameTextW(lp, buf, 63) > 0) key = buf;
                printable = false;
            }
        }
    }
    if (key.empty()) key = FormatW(L"#%u", vk);
    std::wstring s;
    if (mods & kModCtrl) s += L"Ctrl ";
    if (mods & kModAlt) s += L"Alt ";
    if (mods & kModShift) s += L"Shift ";
    if (mods & kModWin) s += L"Win ";
    s += key;
    // 只按 Shift 加可打印字符不算快捷键
    if (hasModifier)
        *hasModifier = (mods & (kModCtrl | kModAlt | kModWin)) != 0 || ((mods & kModShift) && !printable);
    return s;
}

void KeysWindow::OnKey(UINT vk, int mods, UINT scan, bool ext) {
    if (!hwnd_) return;
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        return;  // 单独的修饰键不显示
    }
    bool hasMod = false;
    std::wstring text = ComboText(vk, mods, scan, ext, &hasMod);
    if (onlyShortcuts_ && !hasMod) return;
    double now = NowSeconds();
    if (hideRepeats_ && !entries_.empty() && entries_.back().text == text) {
        entries_.back().shownAt = now;
        entries_.back().alpha = 1;
    } else {
        entries_.push_back({text, now, 1.f});
        if ((int)entries_.size() > maxKeys_) entries_.erase(entries_.begin(), entries_.end() - maxKeys_);
    }
    Render();
    if (!timer_) {
        SetTimer(hwnd_, 1, 33, nullptr);
        timer_ = true;
    }
}

void KeysWindow::Tick() {
    double now = NowSeconds();
    double fadeOut = theme::Tok().animation.normalMs / 1000.0;
    if (fadeOut <= 0) fadeOut = 0.2;
    bool changed = false;
    for (auto &e : entries_) {
        double age = now - e.shownAt;
        float a = age <= fade_ ? 1.f : (float)std::fmax(0.0, 1.0 - (age - fade_) / fadeOut);
        if (a != e.alpha) {
            e.alpha = a;
            changed = true;
        }
    }
    size_t before = entries_.size();
    for (size_t i = 0; i < entries_.size();) {
        if (entries_[i].alpha <= 0) entries_.erase(entries_.begin() + (long)i);
        else ++i;
    }
    if (changed || before != entries_.size()) Render();
    if (entries_.empty()) {
        KillTimer(hwnd_, 1);
        timer_ = false;
    }
}

void KeysWindow::Render() {
    if (!hwnd_) return;
    if (entries_.empty()) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    view_.items.clear();
    for (auto &e : entries_) view_.items.push_back({e.text, e.alpha});
    float s = dpi_ / 96.f;
    RectF area(0, 0, (region_.right - region_.left) / s, (region_.bottom - region_.top) / s);
    auto lay = view_.Layout(area);
    if (lay.empty()) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (auto &p : lay) {
        RectF r = p.second.Inflate(14);  // 阴影
        x0 = std::fmin(x0, r.x);
        y0 = std::fmin(y0, r.y);
        x1 = std::fmax(x1, r.r());
        y1 = std::fmax(y1, r.b());
    }
    int px = (int)std::floor(x0 * s), py = (int)std::floor(y0 * s);
    int pw = (int)std::ceil(x1 * s) - px, ph = (int)std::ceil(y1 * s) - py;
    ID2D1RenderTarget *rt = surface_.Begin(pw, ph, dpi_);
    if (!rt) return;
    {
        Canvas c(rt);
        c.SetTransform(MatTranslate(-px / s, -py / s));
        view_.Draw(c, area);
        c.SetTransform(MatIdentity());
    }
    surface_.End(hwnd_, region_.left + px, region_.top + py, 255);
    if (!IsWindowVisible(hwnd_))
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void KeysWindow::Close() {
    if (hwnd_) KillTimer(hwnd_, 1);
    timer_ = false;
    entries_.clear();
    DestroyWin();
}

LRESULT KeysWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == 1) {
        Tick();
        return 0;
    }
    return Window::Handle(msg, wp, lp);
}

}  // namespace rec
}  // namespace ls
