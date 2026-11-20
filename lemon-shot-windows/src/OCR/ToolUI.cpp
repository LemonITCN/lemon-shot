#include "OCR/ToolUI.h"

#include <commctrl.h>
#include <process.h>
#include <uxtheme.h>

#include <cmath>
#include <cstring>

#include "App/App.h"
#include "Capture/Export.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Toast.h"
#include "Util/Api.h"
#include "Util/ImageIO.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace tool {

namespace {

const Tokens &K() { return theme::Tok(); }
const UiColors &U() { return theme::Ui(); }

// ---------------------------------------------------------------- 界面线程派发

const wchar_t kDispatchClass[] = L"LemonShot.Dispatch";
const UINT WM_LS_RUN = WM_APP + 40;

bool g_dispatchInit = false;
HWND g_dispatchWnd = nullptr;
CRITICAL_SECTION g_dispatchLock;
// 故意不释放：退出时后台线程可能还在投递
std::vector<std::function<void()>> *g_dispatchQueue = nullptr;

LRESULT CALLBACK DispatchProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LS_RUN) {
        std::vector<std::function<void()>> jobs;
        EnterCriticalSection(&g_dispatchLock);
        jobs.swap(*g_dispatchQueue);
        LeaveCriticalSection(&g_dispatchLock);
        for (auto &j : jobs)
            if (j) j();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

struct DetachedJob {
    std::function<void()> fn;
};

unsigned __stdcall DetachedMain(void *p) {
    DetachedJob *j = (DetachedJob *)p;
    if (j->fn) j->fn();
    delete j;
    return 0;
}

unsigned g_targetGen = 1;

COLORREF Ref(Color c) {
    return RGB((int)(Clamp(c.r, 0.f, 1.f) * 255 + 0.5f), (int)(Clamp(c.g, 0.f, 1.f) * 255 + 0.5f),
               (int)(Clamp(c.b, 0.f, 1.f) * 255 + 0.5f));
}

TextStyle TS(float size, Color col, int weight = kRegular, HAlign h = HAlign::Left) {
    TextStyle t;
    t.size = size;
    t.color = col;
    t.weight = weight;
    t.h = h;
    t.v = VAlign::Center;
    return t;
}

std::vector<ToolWindow *> g_windows;

}  // namespace

void InitDispatcher() {
    if (g_dispatchInit) return;
    g_dispatchInit = true;
    InitializeCriticalSection(&g_dispatchLock);
    g_dispatchQueue = new std::vector<std::function<void()>>();
    WNDCLASSEXW wc;
    std::memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &DispatchProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kDispatchClass;
    RegisterClassExW(&wc);
    g_dispatchWnd = CreateWindowExW(0, kDispatchClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_dispatchWnd) LS_LOG("tool: 派发窗口创建失败 %lu", GetLastError());
}

void PostToUi(std::function<void()> fn) {
    if (!g_dispatchInit || !fn) return;
    EnterCriticalSection(&g_dispatchLock);
    g_dispatchQueue->push_back(std::move(fn));
    LeaveCriticalSection(&g_dispatchLock);
    if (g_dispatchWnd) PostMessageW(g_dispatchWnd, WM_LS_RUN, 0, 0);
}

// ---------------------------------------------------------------- 串行队列

SerialQueue *SerialQueue::Create() {
    SerialQueue *q = new SerialQueue();
    InitializeCriticalSection(&q->lock_);
    q->event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // 识别、拼接都可能用到较深的栈：保留 4 MB
    uintptr_t t = q->event_ ? _beginthreadex(nullptr, 4u << 20, &SerialQueue::ThreadMain, q, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)
                            : 0;
    if (!t) {
        LS_LOG("tool: 后台线程创建失败");
        if (q->event_) CloseHandle(q->event_);
        DeleteCriticalSection(&q->lock_);
        delete q;
        return nullptr;
    }
    CloseHandle((HANDLE)t);
    return q;
}

void SerialQueue::Post(std::function<void()> fn) {
    EnterCriticalSection(&lock_);
    jobs_.push_back(std::move(fn));
    LeaveCriticalSection(&lock_);
    SetEvent(event_);
}

void SerialQueue::Shutdown() {
    EnterCriticalSection(&lock_);
    quit_ = true;
    LeaveCriticalSection(&lock_);
    SetEvent(event_);
}

unsigned __stdcall SerialQueue::ThreadMain(void *p) {
    SerialQueue *q = (SerialQueue *)p;
    for (;;) {
        WaitForSingleObject(q->event_, INFINITE);
        bool quit = false;
        for (;;) {
            std::function<void()> job;
            EnterCriticalSection(&q->lock_);
            if (!q->jobs_.empty()) {
                job = std::move(q->jobs_.front());
                q->jobs_.erase(q->jobs_.begin());
            }
            quit = q->quit_ && q->jobs_.empty() && !job;
            LeaveCriticalSection(&q->lock_);
            if (!job) break;
            job();
        }
        if (quit) break;
    }
    CloseHandle(q->event_);
    DeleteCriticalSection(&q->lock_);
    delete q;
    return 0;
}

bool RunDetached(std::function<void()> fn) {
    DetachedJob *j = new DetachedJob{std::move(fn)};
    uintptr_t t = _beginthreadex(nullptr, 1u << 20, &DetachedMain, j, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) {
        delete j;
        return false;
    }
    CloseHandle((HANDLE)t);
    return true;
}

// ---------------------------------------------------------------- 输出

static bool ApplyResolution(const ls_image &img, float scale, ls_image &out, float &outScale) {
    outScale = scale;
    if (settings::Str("save.resolution") == "standard" && scale > 1.01f) {
        int w = (int)std::lround(img.width / scale), h = (int)std::lround(img.height / scale);
        if (w > 0 && h > 0 && ls_image_resize(&img, w, h, &out) == 0 && out.data) {
            outScale = 1;
            return true;
        }
    }
    return false;
}

void CopyImage(const ls_image &img, float scale) {
    if (!img.data) return;
    ls_image small = {};
    float s = scale;
    bool resized = ApplyResolution(img, scale, small, s);
    bool ok = CopyImageToClipboard(app::MainHwnd(), resized ? small : img);
    if (resized) ls_image_free(&small);
    if (ok) toast::Show(T("notify.copied"));
}

void SaveImage(const ls_image &img, float scale) {
    if (!img.data) return;
    ls_image small = {};
    float s = scale;
    bool resized = ApplyResolution(img, scale, small, s);
    std::wstring path, err;
    bool ok = exporter::SaveImage(resized ? small : img, s, settings::Bool("save.askLocation"), path, err);
    if (resized) ls_image_free(&small);
    if (ok) {
        toast::Action a;
        a.kind = toast::kReveal;
        a.path = path;
        toast::Show(T("notify.saved"), "circle-check", a);
    } else if (!err.empty()) {
        toast::Show(i18n::T("notify.saveFailed", {{"error", err}}), "triangle-alert");
    }
}

void CopyText(const std::wstring &text) {
    if (CopyTextToClipboard(app::MainHwnd(), text)) toast::Show(T("common.copied"));
}

ls_image ShrinkCopy(const ls_image &img, int maxW, int maxH) {
    ls_image out = {};
    if (!img.data || img.width <= 0 || img.height <= 0) return out;
    double k = 1;
    if (maxW > 0 && img.width > maxW) k = std::fmin(k, (double)maxW / img.width);
    if (maxH > 0 && img.height > maxH) k = std::fmin(k, (double)maxH / img.height);
    if (k >= 1) return CloneImage(img);
    int w = (int)std::lround(img.width * k), h = (int)std::lround(img.height * k);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (ls_image_resize(&img, w, h, &out) != 0) out = ls_image{};
    return out;
}

unsigned TargetGeneration() { return g_targetGen; }
void BumpTargetGeneration() { ++g_targetGen; }

// ---------------------------------------------------------------- 部件

const float ToggleChip::kHeight = 26;

ToggleChip::ToggleChip(const std::wstring &t, bool v, std::function<void(bool)> cb) : title(t), on(v), onChange(cb) {
    SizeF s = Preferred();
    frame = RectF(0, 0, s.w, s.h);
}

SizeF ToggleChip::Preferred() {
    return SizeF{std::ceil(gfx::Measure(title, K().fontSize.body - 1, kMedium).w + 22), kHeight};
}

void ToggleChip::Draw(Canvas &c) {
    const auto &u = U();
    RectF r(frame.x, frame.cy() - kHeight / 2, frame.w, kHeight);
    float rad = kHeight / 2;
    if (on) {
        c.FillRound(r, rad, u.surfaceSelected);
        c.StrokeRound(r.Inflate(-0.5f), rad - 0.5f, WithAlpha(u.accent, 0.55f), 1);
    } else {
        c.FillRound(r, rad, pressed ? u.surfacePressed : (hovered ? u.surfaceHover : u.surface));
        c.StrokeRound(r.Inflate(-0.5f), rad - 0.5f, u.inputBorder, 1);
    }
    c.Text(title, r, TS(K().fontSize.body - 1, on ? u.iconActive : u.textSecondary, on ? kMedium : kRegular, HAlign::Center));
}

void ToggleChip::OnMouseUp(float, float, bool inside) {
    if (!inside) return;
    on = !on;
    Redraw();
    if (onChange) onChange(on);
}

IconButton::IconButton(const char *ic, const std::wstring &tip, std::function<void()> a) : icon(ic), tooltip(tip), action(a) {
    frame = RectF(0, 0, 28, 28);
}

void IconButton::Draw(Canvas &c) {
    const auto &u = U();
    float rad = K().toolbar.buttonRadius;
    if (selected)
        c.FillRound(frame, rad, u.surfaceSelected);
    else if (pressed)
        c.FillRound(frame, rad, u.surfacePressed);
    else if (hovered)
        c.FillRound(frame, rad, u.surfaceHover);
    Color ic = !enabled ? u.textTertiary : (selected ? u.iconActive : u.icon);
    c.Icon(icon, frame.cx(), frame.cy(), iconSize, ic, 1.6f);
}

void IconButton::OnMouseMove(float, float) {
    if (!tooltip.empty() && host && host->Hwnd()) prefs::ShowInfoBubble(host, frame, tooltip);
}

void IconButton::OnMouseLeave() { prefs::HideInfoBubble(); }

void IconButton::OnMouseUp(float, float, bool inside) {
    prefs::HideInfoBubble();
    if (inside && enabled && action) action();
}

const float TextArea::kTextInsetX = 13, TextArea::kTextInsetY = 8;

RectF TextArea::EditRect(RectF f) { return RectF(f.x + 1, f.y + 6, f.w - 2, f.h - 12); }

void TextArea::Draw(Canvas &c) {
    const auto &u = U();
    float rad = K().window.controlRadius;
    c.FillRound(frame, rad, u.inputBg);
    c.StrokeRound(frame.Inflate(-0.5f), rad - 0.5f, u.inputBorder, 1);
    if (drawText && !text.empty()) {
        c.PushClip(frame.Inflate(-1));
        TextStyle ts = TS(K().fontSize.body, u.text);
        ts.wrap = true;
        ts.v = VAlign::Top;
        c.Text(text, RectF(frame.x + kTextInsetX, frame.y + kTextInsetY, frame.w - 2 * kTextInsetX, frame.h - 2 * kTextInsetY), ts);
        c.PopClip();
    }
    if (!overlay.empty() && text.empty()) c.Text(overlay, frame, TS(K().fontSize.body, u.textTertiary, kRegular, HAlign::Center));
}

ImageBox::~ImageBox() { ls_image_free(&img_); }

void ImageBox::SetImage(const ls_image *img, int maxW, int maxH) {
    ls_image_free(&img_);
    bmp_.Reset();
    bmpRt_ = nullptr;
    srcW_ = srcH_ = 0;
    if (!img || !img->data) return;
    srcW_ = img->width;
    srcH_ = img->height;
    img_ = ShrinkCopy(*img, maxW, maxH);
    if (!img_.data) srcW_ = srcH_ = 0;
}

RectF ImageBox::ImageRect() const {
    if (srcW_ <= 0 || srcH_ <= 0) return RectF();
    RectF inner = frame.Inflate(-1);
    float s = std::fmin(inner.w / srcW_, inner.h / srcH_);
    float w = srcW_ * s, h = srcH_ * s;
    return RectF(inner.cx() - w / 2, topAligned ? inner.y : inner.cy() - h / 2, w, h);
}

void ImageBox::DrawChecker(Canvas &c, RectF r, float cell) {
    bool dark = theme::IsDark();
    Color light = dark ? Color{0.24f, 0.24f, 0.24f, 1} : Color{1, 1, 1, 1};
    Color darkC = dark ? Color{0.18f, 0.18f, 0.18f, 1} : Color{0.9f, 0.9f, 0.9f, 1};
    c.FillRect(r, light);
    int row = 0;
    for (float y = r.y; y < r.b(); y += cell, ++row) {
        for (float x = r.x + (row % 2 ? cell : 0); x < r.r(); x += 2 * cell)
            c.FillRect(RectF(x, y, std::fmin(cell, r.r() - x), std::fmin(cell, r.b() - y)), darkC);
    }
}

void ImageBox::Draw(Canvas &c) {
    if (!img_.data) return;
    RectF ir = ImageRect();
    if (ir.Empty()) return;
    ID2D1RenderTarget *rt = c.RT();
    // 按显示尺寸重采样一份（面积平均，缩小时比 D2D 线性插值清楚）
    float dpiX = 96, dpiY = 96;
    rt->GetDpi(&dpiX, &dpiY);
    int tw = (int)std::lround(ir.w * dpiX / 96.f), th = (int)std::lround(ir.h * dpiY / 96.f);
    if (tw > img_.width || th > img_.height) {
        tw = img_.width;
        th = img_.height;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    bool stale = !bmp_ || bmpRt_ != rt || bmpGen_ != TargetGeneration();
    if (!stale) {
        D2D1_SIZE_U sz = bmp_->GetPixelSize();
        stale = (int)sz.width != tw || (int)sz.height != th;
    }
    if (stale) {
        bmp_.Reset();
        if (tw == img_.width && th == img_.height) {
            bmp_ = gfx::CreateBitmap(rt, img_);
        } else {
            ls_image scaled;
            if (ls_image_resize(&img_, tw, th, &scaled) == 0 && scaled.data) {
                bmp_ = gfx::CreateBitmap(rt, scaled);
                ls_image_free(&scaled);
            }
        }
        bmpRt_ = rt;
        bmpGen_ = TargetGeneration();
    }
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    D2D1_ROUNDED_RECT rr = {ir.D2(), radius, radius};
    if (radius > 0) gfx::D2D()->CreateRoundedRectangleGeometry(&rr, clip.Put());
    if (clip) c.PushGeometryMask(clip.Get());
    if (checker) DrawChecker(c, ir, 8);
    if (bmp_) c.Bitmap(bmp_.Get(), ir, nullptr, 1.f, false);
    if (clip) c.PopLayer();
    if (border) c.StrokeRound(ir.Inflate(-0.5f), radius > 0.5f ? radius - 0.5f : 0, U().border, 1);
}

SizeF Status::Preferred() {
    return SizeF{std::ceil(gfx::Measure(text, K().fontSize.body - 1, kRegular).w + (icon ? 22.f : 0.f)), 20};
}

void Status::Draw(Canvas &c) {
    if (text.empty() && !icon) return;
    const auto &u = U();
    Color col = danger ? u.danger : u.textSecondary;
    float x = frame.x;
    if (icon) {
        c.Icon(icon, x + 8, frame.cy(), 16, col, 1.6f);
        x += 22;
    }
    TextStyle ts = TS(K().fontSize.body - 1, col);
    ts.ellipsis = true;
    if (frame.r() > x) c.Text(text, RectF(x, frame.y, frame.r() - x, frame.h), ts);
}

float LayoutRight(RectF bar, const std::vector<prefs::Widget *> &buttons, float margin, float gap) {
    float x = bar.r() - margin;
    for (prefs::Widget *b : buttons) {
        if (!b || b->hidden) continue;
        SizeF s = b->Preferred();
        x -= s.w;
        b->frame = RectF(x, std::floor(bar.cy() - s.h / 2), s.w, s.h);
        x -= gap;
    }
    return x + gap;
}

// ---------------------------------------------------------------- 工具窗口

ToolWindow::ToolWindow() { g_windows.push_back(this); }

ToolWindow::~ToolWindow() {
    for (size_t i = 0; i < g_windows.size(); ++i)
        if (g_windows[i] == this) {
            g_windows.erase(g_windows.begin() + (long)i);
            break;
        }
    if (editFont_) DeleteObject(editFont_);
    if (editBrush_) DeleteObject(editBrush_);
}

static HICON AppIconCached(int size) {
    static HICON big = nullptr, small = nullptr;
    HICON &h = size >= 32 ? big : small;
    if (!h) h = app::AppIcon(size);
    return h;
}

bool ToolWindow::Open(const std::wstring &title, float w, float h, float minW, float minH, bool resizable, HMONITOR mon) {
    if (hwnd_) return true;
    if (!mon) {
        POINT pt;
        GetCursorPos(&pt);
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }
    dpi_ = api::MonitorDpi(mon);
    minW_ = minW;
    minH_ = minH;
    resizable_ = resizable;
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    if (resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    RECT rc = {0, 0, (LONG)std::lround(w * S()), (LONG)std::lround(h * S())};
    api::AdjustRectForDpi(&rc, style, 0, dpi_);
    int ow = rc.right - rc.left, oh = rc.bottom - rc.top;
    const RECT &wa = mi.rcWork;
    int x = (wa.left + wa.right - ow) / 2, y = (wa.top + wa.bottom - oh) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    if (!CreateWin(0, style, x, y, ow, oh, nullptr, title.c_str())) {
        LS_LOG("tool: 窗口创建失败 %lu", GetLastError());
        return false;
    }
    UINT actual = api::WindowDpi(hwnd_);
    if (actual != dpi_) {
        dpi_ = actual;
        RECT r2 = {0, 0, (LONG)std::lround(w * S()), (LONG)std::lround(h * S())};
        api::AdjustRectForDpi(&r2, style, 0, dpi_);
        SetWindowPos(hwnd_, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    api::SetDarkTitleBar(hwnd_, theme::IsDark());
    SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)AppIconCached(32));
    SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)AppIconCached(16));
    UpdateEditFont();
    ApplyEditTheme();
    root_.SetHost(this);
    return true;
}

void ToolWindow::Present(bool activate) {
    if (!hwnd_) return;
    Relayout();
    if (IsIconic(hwnd_))
        ShowWindow(hwnd_, SW_RESTORE);
    else
        ShowWindow(hwnd_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (activate) SetForegroundWindow(hwnd_);
}

void ToolWindow::SetTopmost(bool on) {
    if (hwnd_) SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ToolWindow::Close() {
    if (hwnd_) SendMessageW(hwnd_, WM_CLOSE, 0, 0);
}

float ToolWindow::ClientW() const {
    if (!hwnd_) return root_.frame.w;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    return rc.right / S();
}

float ToolWindow::ClientH() const {
    if (!hwnd_) return root_.frame.h;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    return rc.bottom / S();
}

void ToolWindow::Relayout() {
    if (!hwnd_) return;
    root_.SetHost(this);
    float w = ClientW(), h = ClientH();
    root_.frame = RectF(0, 0, w, h);
    Layout(w, h);
    Invalidate();
}

void ToolWindow::Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

RECT ToolWindow::ScreenRect(RectF r) {
    float s = S();
    POINT a = {(LONG)std::floor(r.x * s), (LONG)std::floor(r.y * s)};
    POINT b = {(LONG)std::ceil(r.r() * s), (LONG)std::ceil(r.b() * s)};
    if (hwnd_) {
        ClientToScreen(hwnd_, &a);
        ClientToScreen(hwnd_, &b);
    }
    return RECT{a.x, a.y, b.x, b.y};
}

HWND ToolWindow::CreateTextEdit(bool readOnly) {
    if (!hwnd_) return nullptr;
    DWORD style = WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL;
    if (readOnly) style |= ES_READONLY;
    HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, hwnd_, nullptr, app::Instance(), nullptr);
    if (!e) return nullptr;
    SendMessageW(e, EM_SETLIMITTEXT, 0, 0);  // 0 = 系统上限
    SendMessageW(e, WM_SETFONT, (WPARAM)editFont_, FALSE);
    float s = S();
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM((WORD)std::lround((TextArea::kTextInsetX - 1) * s), (WORD)std::lround(4 * s)));
    SetWindowSubclass(e, &ToolWindow::EditProc, 1, (DWORD_PTR)this);
    edits_.push_back(e);
    ApplyEditTheme();
    return e;
}

void ToolWindow::PlaceEdit(HWND e, RectF frame, bool visible) {
    if (!e) return;
    RectF r = TextArea::EditRect(frame);
    float s = S();
    int x = (int)std::lround(r.x * s), y = (int)std::lround(r.y * s);
    int w = (int)std::lround(r.r() * s) - x, h = (int)std::lround(r.b() * s) - y;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    SetWindowPos(e, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

std::wstring ToolWindow::EditText(HWND e) {
    if (!e) return std::wstring();
    int len = GetWindowTextLengthW(e);
    std::wstring t((size_t)len + 1, L'\0');
    GetWindowTextW(e, &t[0], len + 1);
    t.resize((size_t)len);
    return ReplaceAll(t, L"\r\n", L"\n");
}

void ToolWindow::SetEditText(HWND e, const std::wstring &t) {
    if (!e) return;
    std::wstring w = ReplaceAll(ReplaceAll(t, L"\r\n", L"\n"), L"\n", L"\r\n");
    SetWindowTextW(e, w.c_str());
}

void ToolWindow::AppendEditText(HWND e, const std::wstring &t) {
    if (!e) return;
    std::wstring w = ReplaceAll(ReplaceAll(t, L"\r\n", L"\n"), L"\n", L"\r\n");
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
}

void ToolWindow::UpdateEditFont() {
    HFONT old = editFont_;
    editFont_ = CreateFontW(-(int)std::lround(K().fontSize.body * S()), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            gfx::FontFamily().c_str());
    float s = S();
    for (HWND e : edits_) {
        SendMessageW(e, WM_SETFONT, (WPARAM)editFont_, TRUE);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM((WORD)std::lround((TextArea::kTextInsetX - 1) * s), (WORD)std::lround(4 * s)));
    }
    if (old) DeleteObject(old);
}

void ToolWindow::ApplyEditTheme() {
    const auto &u = U();
    editBg_ = Ref(u.inputBg);
    editText_ = Ref(u.text);
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(editBg_);
    // 深色滚动条：Win10 1809+ 才有 DarkMode_Explorer 主题，其他系统用默认
    bool darkTheme = theme::IsDark() && api::IsWin10OrLater() && api::OsBuild() >= 17763;
    for (HWND e : edits_) {
        SetWindowTheme(e, darkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(e, nullptr, TRUE);
    }
}

void ToolWindow::RefreshAllThemes() {
    for (ToolWindow *w : g_windows) {
        if (!w->hwnd_) continue;
        api::SetDarkTitleBar(w->hwnd_, theme::IsDark());
        w->ApplyEditTheme();
        w->Invalidate();
        // 标题栏颜色要重画非客户区才生效
        SetWindowPos(w->hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

LRESULT CALLBACK ToolWindow::EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    ToolWindow *self = (ToolWindow *)ref;
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (m) {
    case WM_KEYDOWN:
        if (ctrl && w == 'A') {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        if (self && self->OnEditKey(h, (UINT)w)) return 0;
        break;
    case WM_CHAR:
        // Ctrl+A（0x01）、Ctrl+回车（0x0A）不要插进文本
        if (ctrl && (w == 1 || w == 10)) return 0;
        break;
    case WM_NCDESTROY: RemoveWindowSubclass(h, &ToolWindow::EditProc, id); break;
    }
    return DefSubclassProc(h, m, w, l);
}

void ToolWindow::DrawContent(Canvas &c, float w, float h) {
    c.Clear(U().windowBg);
    root_.Draw(c);
}

void ToolWindow::Paint() {
    if (!rt_) {
        rt_ = gfx::CreateHwndTarget(hwnd_, dpi_);
        if (!rt_) return;
        BumpTargetGeneration();
        OnTargetReset();
    }
    rt_->BeginDraw();
    {
        Canvas c(rt_.Get());
        c.SetTransform(MatIdentity());
        DrawContent(c, ClientW(), ClientH());
    }
    if (rt_->EndDraw(nullptr, nullptr) == (HRESULT)D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        Invalidate();
    }
}

void ToolWindow::SetHover(prefs::Widget *w) {
    if (w == hover_) return;
    if (hover_) {
        hover_->hovered = false;
        hover_->OnMouseLeave();
    }
    hover_ = w;
    if (hover_) hover_->hovered = true;
    Invalidate();
}

void ToolWindow::OnDestroyed() {
    hover_ = pressed_ = focus_ = nullptr;
    edits_.clear();
    rt_.Reset();
    OnClosed();
    app::Defer([](void *p) { delete (ToolWindow *)p; }, this);
}

LRESULT ToolWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    float s = S();
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
        if (rt_) {
            D2D1_SIZE_U sz = {LOWORD(lp), HIWORD(lp)};
            rt_->Resize(&sz);
        }
        if (wp != SIZE_MINIMIZED) Relayout();
        return 0;
    case WM_GETMINMAXINFO:
        if (minW_ > 0 && minH_ > 0 && hwnd_) {
            DWORD style = (DWORD)GetWindowLongW(hwnd_, GWL_STYLE);
            RECT r = {0, 0, (LONG)std::lround(minW_ * s), (LONG)std::lround(minH_ * s)};
            api::AdjustRectForDpi(&r, style, 0, dpi_);
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = r.right - r.left;
            mm->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        if (rt_) rt_->SetDpi((float)dpi_, (float)dpi_);
        UpdateEditFont();
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout();
        return 0;
    }
    case WM_SETCURSOR:
        if ((HWND)wp == hwnd_ && LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, hover_ ? hover_->Cursor() : MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        float x = MouseX(lp) / s, y = MouseY(lp) / s;
        if (pressed_) {
            pressed_->OnMouseDrag(x, y);
            Invalidate();
            return 0;
        }
        prefs::Widget *w = root_.HitTest(x, y);
        SetHover(w);
        if (w) w->OnMouseMove(x, y);
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (!pressed_) SetHover(nullptr);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        float x = MouseX(lp) / s, y = MouseY(lp) / s;
        prefs::Widget *w = root_.HitTest(x, y);
        if (!w) {
            SetFocus(hwnd_);
            return 0;
        }
        pressed_ = w;
        w->pressed = true;
        SetCapture(hwnd_);
        w->OnMouseDown(x, y);
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!pressed_) return 0;
        prefs::Widget *w = pressed_;
        pressed_ = nullptr;
        w->pressed = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
        float x = MouseX(lp) / s, y = MouseY(lp) / s;
        Invalidate();
        w->OnMouseUp(x, y, w->frame.Contains(x, y));  // 可能关闭窗口（对象延迟释放）
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pressed_ && (HWND)lp != hwnd_) {
            pressed_->pressed = false;
            pressed_ = nullptr;
            Invalidate();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt = {MouseX(lp), MouseY(lp)};
        ScreenToClient(hwnd_, &pt);
        float dy = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        if (hover_ && hover_->OnWheel(dy)) return 0;
        if (OnWheel(pt.x / s, pt.y / s, dy)) return 0;
        break;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && lp) OnEditChange((HWND)lp);
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        for (HWND e : edits_) {
            if ((HWND)lp == e) {
                SetTextColor((HDC)wp, editText_);
                SetBkColor((HDC)wp, editBg_);
                return (LRESULT)editBrush_;
            }
        }
        break;
    case WM_SETTINGCHANGE:
        // 系统深浅色切换：主窗口会刷新主题，稍后再跟着重画
        if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) SetTimer(hwnd_, 0x7E01, 300, nullptr);
        break;
    case WM_TIMER:
        if (wp == 0x7E01) {
            KillTimer(hwnd_, 0x7E01);
            RefreshAllThemes();
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) prefs::HideInfoBubble();
        break;
    case WM_CLOSE:
        prefs::CloseAllPopups();
        prefs::HideInfoBubble();
        DestroyWindow(hwnd_);
        return 0;
    }
    return Window::Handle(msg, wp, lp);
}

ls_image RenderOffscreen(float w, float h, float scale, const std::function<void(Canvas &)> &draw) {
    ls_image img = {};
    Offscreen off;
    if (!off.Create((int)std::ceil(w * scale), (int)std::ceil(h * scale), scale)) return img;
    BumpTargetGeneration();
    ID2D1RenderTarget *rt = off.RT();
    rt->BeginDraw();
    {
        Canvas c(rt);
        c.Clear(Color{0, 0, 0, 0});
        draw(c);
    }
    if (FAILED(rt->EndDraw(nullptr, nullptr))) return img;
    off.ReadPixels(img);
    return img;
}

}  // namespace tool
}  // namespace ls
