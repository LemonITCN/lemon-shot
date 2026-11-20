// 长截图（spec 6）。面板外观与 Mac 端 LongShot/LongShotPanel.swift 一致。
// 线程：界面线程每 100ms BitBlt 抓一帧（上一帧没处理完就跳过），交给后台串行队列 ls_stitch_push；
// 拼接器只在那条后台线程上用，结果（状态 + 缩略预览）派回界面线程。
// 边框与面板都放在区域外，并用 WDA_EXCLUDEFROMCAPTURE 排除（Win10 2004+）；更早的系统面板实在只能放区域内时，把抓取范围让开面板。
#include "LongShot/LongShot.h"

#include <cmath>
#include <cstring>
#include <string>

#include "App/App.h"
#include "Capture/Capture.h"
#include "Capture/Screen.h"
#include "OCR/ToolUI.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace longshot {

namespace {

const float kPanelW = 160, kPad = 12, kButtonGap = 8, kMinPanelH = 380, kShadowMargin = 22;
enum { TIMER_START = 1, TIMER_TICK = 2, TIMER_AUTO = 3 };
enum { HOTKEY_ESC = 1 };

const Tokens &K() { return theme::Tok(); }
const UiColors &U() { return theme::Ui(); }

TextStyle TS(float size, Color col, int weight = kRegular, HAlign h = HAlign::Left) {
    TextStyle t;
    t.size = size;
    t.color = col;
    t.weight = weight;
    t.h = h;
    t.v = VAlign::Center;
    return t;
}

COLORREF Ref(Color c) {
    return RGB((int)(Clamp(c.r, 0.f, 1.f) * 255 + 0.5f), (int)(Clamp(c.g, 0.f, 1.f) * 255 + 0.5f),
               (int)(Clamp(c.b, 0.f, 1.f) * 255 + 0.5f));
}

// ---------------------------------------------------------------- 面板内容（屏幕与自测共用）

class PanelWidget : public prefs::Widget {
public:
    bool notes = false;
    bool autoScrolling = false;
    bool shadow = true;
    int heightPx = 0;
    std::wstring hint;
    bool hintWarning = false;
    ID2D1Bitmap *preview = nullptr;  // 不持有
    float previewPxW = 0, previewPxH = 0;
    prefs::ButtonW *autoBtn, *finishBtn, *cancelBtn, *gotItBtn, *notesCancelBtn;

    PanelWidget() {
        autoBtn = (prefs::ButtonW *)Add(new prefs::ButtonW(T("longshot.auto"), prefs::ButtonW::Normal, "play"));
        finishBtn = (prefs::ButtonW *)Add(new prefs::ButtonW(T("longshot.finish"), prefs::ButtonW::Primary, "check"));
        cancelBtn = (prefs::ButtonW *)Add(new prefs::ButtonW(T("longshot.cancel"), prefs::ButtonW::Normal, "x"));
        gotItBtn = (prefs::ButtonW *)Add(new prefs::ButtonW(T("longshot.gotIt"), prefs::ButtonW::Primary));
        notesCancelBtn = (prefs::ButtonW *)Add(new prefs::ButtonW(T("longshot.cancel")));
    }

    void SetAuto(bool on) {
        autoScrolling = on;
        autoBtn->title = T(on ? "longshot.stopAuto" : "longshot.auto");
        autoBtn->icon = on ? "pause" : "play";
    }

    RectF Inner() const { return frame.Inflate(-kPad); }
    RectF TitleRect() const {
        RectF in = Inner();
        return RectF(in.x, in.y, in.w, 22);
    }
    static float HintHeight() { return std::ceil((gfx::Measure(L"Ag", K().fontSize.caption, kRegular).h + 2) * 3); }

    // 预览框：标题下方到高度标签上方
    RectF PreviewRect() const {
        RectF in = Inner();
        float bh = K().window.controlHeight;
        float buttonsTop = in.b() - 3 * bh - 2 * kButtonGap;
        float bottom = buttonsTop - 10 - HintHeight() - 4 - 20 - 8;
        RectF t = TitleRect();
        return RectF(in.x, t.b() + 8, in.w, std::fmax(40.f, bottom - t.b() - 8));
    }

    void Layout() override {
        RectF in = Inner();
        float bh = K().window.controlHeight;
        float y = in.b() - bh;
        for (prefs::ButtonW *b : {cancelBtn, finishBtn, autoBtn}) {
            b->frame = RectF(in.x, y, in.w, bh);
            b->hidden = notes;
            y -= bh + kButtonGap;
        }
        notesCancelBtn->frame = RectF(in.x, in.b() - bh, in.w, bh);
        gotItBtn->frame = RectF(in.x, in.b() - 2 * bh - kButtonGap, in.w, bh);
        notesCancelBtn->hidden = gotItBtn->hidden = !notes;
    }

    void Draw(Canvas &c) override {
        const auto &u = U();
        float rad = K().window.radius;
        if (shadow) c.Shadow(frame, rad, K().toolbar.shadowBlur, K().toolbar.shadowOffsetY, u.shadow);
        c.FillRound(frame, rad, u.popoverBg);
        c.StrokeRound(frame.Inflate(-0.5f), rad - 0.5f, u.border, 1);
        if (notes)
            DrawNotes(c);
        else
            DrawRunning(c);
        prefs::Widget::Draw(c);
    }

    void DrawRunning(Canvas &c) {
        const auto &u = U();
        const auto &fs = K().fontSize;
        RectF in = Inner();
        c.Text(T("longshot.title"), TitleRect(), TS(fs.body, u.text, kSemibold));
        RectF pr = PreviewRect();
        c.FillRound(pr, 6, u.inputBg);
        if (preview && previewPxW > 0 && previewPxH > 0) {
            // 等宽缩放，太高时显示底部（自动滚到底）
            float s = pr.w / previewPxW;
            float h = previewPxH * s;
            RectF r(pr.x, h > pr.h ? pr.b() - h : pr.y, pr.w, h);
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            D2D1_ROUNDED_RECT rr = {pr.D2(), 6, 6};
            gfx::D2D()->CreateRoundedRectangleGeometry(&rr, clip.Put());
            if (clip) c.PushGeometryMask(clip.Get());
            c.Bitmap(preview, r, nullptr, 1.f, false);
            if (clip) c.PopLayer();
        } else {
            c.Icon("scroll-text", pr.cx(), pr.cy(), 28, u.textTertiary, 1.5f);
        }
        c.StrokeRound(pr.Inflate(-0.5f), 5.5f, u.border, 1);
        RectF hr(in.x, pr.b() + 8, in.w, 20);
        TextStyle hs = TS(fs.body, u.text, kMedium, HAlign::Center);
        hs.tabular = true;
        c.Text(i18n::T("longshot.height", {{"h", FormatW(L"%d", heightPx)}}), hr, hs);
        if (!hint.empty()) {
            RectF tr(in.x, hr.b() + 4, in.w, HintHeight());
            c.PushClip(tr);
            TextStyle ts = TS(fs.caption, hintWarning ? u.warning : u.textSecondary, kRegular, HAlign::Center);
            ts.wrap = true;
            ts.v = VAlign::Top;
            c.Text(hint, tr, ts);
            c.PopClip();
        }
    }

    void DrawNotes(Canvas &c) {
        const auto &u = U();
        const auto &fs = K().fontSize;
        RectF in = Inner();
        RectF t = TitleRect();
        c.Text(T("longshot.notes"), t, TS(fs.body, u.text, kSemibold));
        float y = t.b() + 10;
        float size = fs.caption + 1;
        for (int i = 1; i <= 3; ++i) {
            RectF badge(in.x, y + 1, 16, 16);
            c.FillEllipse(badge.cx(), badge.cy(), 8, 8, u.surfaceSelected);
            TextStyle ns = TS(fs.caption, u.iconActive, kSemibold, HAlign::Center);
            ns.tabular = true;
            c.Text(FormatW(L"%d", i), badge, ns);
            std::wstring text = T(Format("longshot.note%d", i).c_str());
            float tw = in.w - 24;
            float h = gfx::Measure(text, size, kRegular, false, tw).h;
            TextStyle ts = TS(size, u.text);
            ts.wrap = true;
            ts.v = VAlign::Top;
            c.Text(text, RectF(in.x + 24, y, tw, h + 2), ts);
            y += h + 12;
        }
    }
};

// ---------------------------------------------------------------- 区域边框：四条细窗口放在区域外侧（不进画面，鼠标穿透）

class Strip : public Window {
public:
    HBRUSH brush = nullptr;
    ~Strip() override {
        if (brush) DeleteObject(brush);
    }
    bool Create(RECT r) {
        brush = CreateSolidBrush(Ref(K().recordBar.regionBorder));
        if (!CreateWin(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP, r.left,
                       r.top, r.right - r.left, r.bottom - r.top, nullptr, L"LemonShot"))
            return false;
        SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        api::ExcludeFromCapture(hwnd_);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        return true;
    }

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            FillRect(dc, &rc, brush);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        }
        return Window::Handle(msg, wp, lp);
    }
};

// ---------------------------------------------------------------- 面板窗口（分层窗口，不抢焦点）

class Session;

class PanelWindow : public Window, public prefs::Host {
public:
    Session *owner = nullptr;
    PanelWidget panel;
    LayeredSurface surface;
    UINT dpi = 96;
    float margin = kShadowMargin;
    POINT pos = {};
    prefs::Widget *hover = nullptr, *pressed = nullptr;
    bool tracking = false;
    ls_image previewImg = {};
    ComPtr<ID2D1Bitmap> previewBmp;
    unsigned previewGen = 0;
    bool previewDirty = false;

    ~PanelWindow() override { ls_image_free(&previewImg); }

    float S() const { return dpi / 96.f; }

    bool Create(POINT panelPx, float w, float h, UINT d, bool withShadow) {
        dpi = d;
        margin = withShadow ? kShadowMargin : 0;
        panel.shadow = withShadow;
        panel.frame = RectF(margin, margin, w, h);
        panel.SetHost(this);
        panel.Layout();
        float s = S();
        pos = POINT{panelPx.x - (LONG)std::lround(margin * s), panelPx.y - (LONG)std::lround(margin * s)};
        int pw = (int)std::ceil((w + 2 * margin) * s), ph = (int)std::ceil((h + 2 * margin) * s);
        if (!CreateWin(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP, pos.x, pos.y, pw, ph, nullptr,
                       L"LemonShot"))
            return false;
        return true;
    }

    // 接管 img
    void SetPreview(ls_image *img) {
        ls_image_free(&previewImg);
        previewImg = *img;
        previewDirty = true;
    }

    void Render() {
        if (!hwnd_) return;
        float s = S();
        int pw = (int)std::ceil((panel.frame.w + 2 * margin) * s), ph = (int)std::ceil((panel.frame.h + 2 * margin) * s);
        ID2D1RenderTarget *rt = surface.Begin(pw, ph, dpi);
        if (!rt) return;
        if (previewImg.data && (!previewBmp || previewDirty || previewGen != surface.Generation())) {
            previewBmp = gfx::CreateBitmap(rt, previewImg);
            previewGen = surface.Generation();
            previewDirty = false;
        }
        panel.preview = previewBmp.Get();
        panel.previewPxW = (float)previewImg.width;
        panel.previewPxH = (float)previewImg.height;
        panel.Layout();
        {
            Canvas c(rt);
            panel.Draw(c);
        }
        surface.End(hwnd_, pos.x, pos.y, 255);
    }

    void SetHover(prefs::Widget *w) {
        if (w == hover) return;
        if (hover) {
            hover->hovered = false;
            hover->OnMouseLeave();
        }
        hover = w;
        if (hover) hover->hovered = true;
        Render();
    }

    // prefs::Host
    void Invalidate() override { Render(); }
    HWND Hwnd() override { return hwnd_; }
    RECT ScreenRect(RectF r) override {
        float s = S();
        return RECT{pos.x + (LONG)std::floor(r.x * s), pos.y + (LONG)std::floor(r.y * s), pos.x + (LONG)std::ceil(r.r() * s),
                    pos.y + (LONG)std::ceil(r.b() * s)};
    }
    UINT Dpi() override { return dpi; }
    void BeginEdit(RectF, const std::wstring &, bool, bool, std::function<void(const std::wstring &, bool)>) override {}
    void EndEdit() override {}
    void SetFocusWidget(prefs::Widget *) override {}
    prefs::Widget *FocusWidget() override { return nullptr; }
    void RequestRebuild() override { Render(); }

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;
};

// ---------------------------------------------------------------- 一次长截图

struct Worker {
    ls_stitcher *st = nullptr;
    double lastPreview = 0;
};

Session *g_session = nullptr;
int g_nextId = 1;

class Session {
public:
    int id = 0;
    HMONITOR mon = nullptr;
    RECT region = {};  // 用户选的区域（屏幕像素）
    RECT grab = {};    // 实际抓取范围（老系统面板只能放区域内时让开面板）
    float scale = 1;
    UINT dpi = 96;
    PanelWindow *panel = nullptr;
    Strip *strips[4] = {};
    tool::SerialQueue *queue = nullptr;
    Worker *worker = nullptr;
    bool running = false, inFlight = false, stopped = false, finished = false, autoScrolling = false;
    int unchanged = 0, scrolledSinceMatch = 0, step = 80;

    ~Session() {
        delete panel;
        for (Strip *s : strips) delete s;
    }

    bool Begin(HMONITOR m, RECT r) {
        mon = m;
        region = grab = r;
        dpi = api::MonitorDpi(mon);
        scale = dpi / 96.f;
        float speed = (float)settings::Num("longshot.scrollSpeed");
        if (speed < 1) speed = 1;
        step = (int)std::lround(120.f * speed / 3.f);
        queue = tool::SerialQueue::Create();
        if (!queue) return false;
        worker = new Worker();
        tool::InitDispatcher();

        MONITORINFO mi = {};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(mon, &mi);
        const RECT &wa = mi.rcWork;
        float s = scale;
        auto px = [s](float v) { return (int)std::lround(v * s); };
        // 边框：四条放在区域外侧
        int bw = std::max(1, px(K().recordBar.regionBorderWidth));
        RECT sr[4] = {{r.left - bw, r.top - bw, r.right + bw, r.top},
                      {r.left - bw, r.bottom, r.right + bw, r.bottom + bw},
                      {r.left - bw, r.top, r.left, r.bottom},
                      {r.right, r.top, r.right + bw, r.bottom}};
        for (int i = 0; i < 4; ++i) {
            strips[i] = new Strip();
            strips[i]->Create(sr[i]);
        }
        // 面板：区域右侧，放不下放左侧，再放不下贴在区域内右侧；高度跟区域走（至少 380）
        int gap = px(10) + bw;
        int pw = px(kPanelW);
        int ph = std::min(std::max((int)(r.bottom - r.top), px(kMinPanelH)), (int)(wa.bottom - wa.top) - px(16));
        int x;
        bool inside = false;
        if (r.right + gap + pw <= wa.right - px(8)) {
            x = r.right + gap;
        } else if (r.left - gap - pw >= wa.left + px(8)) {
            x = r.left - gap - pw;
        } else {
            x = r.right - pw - px(12);
            inside = true;
        }
        int y = std::max((int)wa.top + px(8), std::min((int)r.top, (int)wa.bottom - px(8) - ph));
        bool canExclude = api::IsWin10OrLater() && api::OsBuild() >= 19041;
        panel = new PanelWindow();
        panel->owner = this;
        panel->panel.notes = !settings::Bool("longshot.tutorialSeen");
        panel->panel.hint = T("longshot.hint");
        if (!panel->Create(POINT{x, y}, kPanelW, ph / s, dpi, canExclude)) return false;
        bool excluded = api::ExcludeFromCapture(panel->Hwnd());
        if (!excluded) panel->panel.shadow = false;
        if (inside && !excluded) {
            // 面板挡在区域里又排除不掉：抓取范围让开面板
            int right = x - px(4);
            if (right - grab.left >= 16) grab.right = right;
        }
        Bind();
        panel->Render();
        ShowWindow(panel->Hwnd(), SW_SHOWNOACTIVATE);
        RegisterHotKey(panel->Hwnd(), HOTKEY_ESC, 0x4000 /* MOD_NOREPEAT */, VK_ESCAPE);
        LS_LOG("longshot: 区域 %ldx%ld，面板%s，%s排除", r.right - r.left, r.bottom - r.top, inside ? "在区域内" : "在区域外",
               excluded ? "已" : "未");
        if (!panel->panel.notes) StartCapturing();
        return true;
    }

    void Bind() {
        PanelWidget &p = panel->panel;
        p.autoBtn->action = [this]() { SetAuto(!autoScrolling); };
        p.finishBtn->action = [this]() { Finish(); };
        p.cancelBtn->action = [this]() { Cancel(); };
        p.notesCancelBtn->action = [this]() { Cancel(); };
        p.gotItBtn->action = [this]() {
            settings::SetBool("longshot.tutorialSeen", true);
            panel->panel.notes = false;
            panel->Render();
            StartCapturing();
        };
    }

    void StartCapturing() {
        if (running || finished) return;
        running = true;
        int maxH = std::max(1000, settings::Int("longshot.maxHeight"));
        Worker *wk = worker;
        queue->Post([wk, maxH]() { wk->st = ls_stitch_create(maxH); });
        // 截图层刚收起：等它真正离开屏幕再开始抓
        SetTimer(panel->Hwnd(), TIMER_START, 150, nullptr);
    }

    void OnTimer(UINT_PTR t) {
        HWND h = panel ? panel->Hwnd() : nullptr;
        if (!h || finished) return;
        if (t == TIMER_START) {
            KillTimer(h, TIMER_START);
            SetTimer(h, TIMER_TICK, 100, nullptr);
            Tick();
            if (settings::Bool("longshot.autoScroll")) SetTimer(h, TIMER_AUTO, 300, nullptr);
        } else if (t == TIMER_AUTO) {
            KillTimer(h, TIMER_AUTO);
            if (!autoScrolling) SetAuto(true);
        } else if (t == TIMER_TICK) {
            Tick();
        }
    }

    // 预览框的像素宽与高宽比
    void PreviewBox(int &pw, float &ratio) {
        RectF pr = panel->panel.PreviewRect();
        pw = std::max(1, (int)std::lround(pr.w * scale));
        ratio = pr.w > 0 ? pr.h / pr.w : 1.f;
    }

    void Tick() {
        if (!running || inFlight || stopped || finished) return;
        ls_image img;
        if (!CaptureScreenRect(grab, false, img)) return;
        inFlight = true;
        Worker *wk = worker;
        int sid = id;
        int pw = 1;
        float ratio = 1;
        PreviewBox(pw, ratio);
        queue->Post([wk, img, sid, pw, ratio]() mutable {
            ls_stitch_result r;
            std::memset(&r, 0, sizeof r);
            r.status = LS_STITCH_NO_MATCH;
            if (wk->st) ls_stitch_push(wk->st, &img, &r);
            ls_image_free(&img);
            ls_image *prev = nullptr;
            bool grew = r.status == LS_STITCH_FIRST || r.status == LS_STITCH_APPENDED;
            double now = NowSeconds();
            if (wk->st && grew && (now - wk->lastPreview > 0.3 || r.status == LS_STITCH_FIRST)) {
                wk->lastPreview = now;
                ls_image p;
                if (ls_stitch_preview(wk->st, pw, 1000000, &p) == 0 && p.data) {
                    // 面板只显示底部：只留预览框能装下的那一截
                    int need = (int)std::ceil(p.width * ratio) + 2;
                    if (p.height > need) {
                        ls_image tail;
                        if (ls_image_crop(&p, 0, p.height - need, p.width, need, &tail) == 0 && tail.data) {
                            ls_image_free(&p);
                            p = tail;
                        }
                    }
                    prev = new ls_image(p);
                }
            }
            tool::PostToUi([sid, r, prev]() {
                Session *s = g_session;
                if (!s || s->id != sid) {
                    if (prev) {
                        ls_image_free(prev);
                        delete prev;
                    }
                    return;
                }
                s->OnFrame(r, prev);
            });
        });
    }

    void SetHint(const std::wstring &s, bool warning) {
        panel->panel.hint = s;
        panel->panel.hintWarning = warning;
    }

    void OnFrame(const ls_stitch_result &r, ls_image *prev) {
        inFlight = false;
        if (prev) {
            panel->SetPreview(prev);
            delete prev;
        }
        if (finished) return;
        panel->panel.heightPx = r.total_height;
        switch (r.status) {
        case LS_STITCH_FIRST:
        case LS_STITCH_APPENDED:
            unchanged = 0;
            scrolledSinceMatch = 0;
            SetHint(T("longshot.hint"), false);
            break;
        case LS_STITCH_UNCHANGED:
            scrolledSinceMatch = 0;
            if (autoScrolling && ++unchanged >= 5) {
                SetHint(T("longshot.reachedEnd"), false);
                Finish();
                return;
            }
            break;
        case LS_STITCH_NO_MATCH:
            SetHint(T("longshot.noMatch"), true);
            if (autoScrolling && scrolledSinceMatch > 0) {
                // 滚过头了：退回一半，之后步长减半
                int back = scrolledSinceMatch / 2;
                if (back > 0) Scroll(back);
                scrolledSinceMatch -= back;
                step = std::max(30, step / 2);
                panel->Render();
                return;
            }
            break;
        case LS_STITCH_MAX:
            stopped = true;
            SetAuto(false);
            SetHint(T("longshot.reachedMax"), true);
            panel->Render();
            return;
        default: break;
        }
        if (autoScrolling && Scroll(-step)) scrolledSinceMatch += step;
        panel->Render();
    }

    // 滚轮（WHEEL_DELTA 单位，负数向下）：发在光标处，光标不在区域里时不发（用户去点面板时自动暂停）
    bool Scroll(int delta) {
        POINT pt;
        GetCursorPos(&pt);
        if (!PtInRect(&grab, pt)) return false;
        INPUT in;
        std::memset(&in, 0, sizeof in);
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = (DWORD)delta;
        return SendInput(1, &in, sizeof in) == 1;
    }

    void SetAuto(bool on) {
        if (on == autoScrolling || (on && stopped)) return;
        autoScrolling = on;
        panel->panel.SetAuto(on);
        unchanged = 0;
        scrolledSinceMatch = 0;
        if (on) {
            POINT c = {(grab.left + grab.right) / 2, (grab.top + grab.bottom) / 2};
            POINT pt;
            GetCursorPos(&pt);
            if (!PtInRect(&grab, pt)) SetCursorPos(c.x, c.y);
            // Win7/8 的滚轮发给焦点窗口：把区域下的窗口切到前台
            HWND t = WindowFromPoint(c);
            if (t) t = GetAncestor(t, GA_ROOT);
            DWORD pid = 0;
            if (t) GetWindowThreadProcessId(t, &pid);
            if (t && pid != GetCurrentProcessId() && GetForegroundWindow() != t) SetForegroundWindow(t);
        }
        panel->Render();
    }

    void Teardown() {
        running = false;
        autoScrolling = false;
        if (panel && panel->Hwnd()) {
            HWND h = panel->Hwnd();
            KillTimer(h, TIMER_START);
            KillTimer(h, TIMER_TICK);
            KillTimer(h, TIMER_AUTO);
            UnregisterHotKey(h, HOTKEY_ESC);
            ShowWindow(h, SW_HIDE);
        }
        for (Strip *s : strips)
            if (s && s->Hwnd()) ShowWindow(s->Hwnd(), SW_HIDE);
        prefs::HideInfoBubble();
        if (g_session == this) g_session = nullptr;
        // 可能在面板自己的消息处理里：延迟释放
        app::Defer([](void *p) { delete (Session *)p; }, this);
    }

    void Cancel() {
        if (finished) return;
        finished = true;
        Teardown();
        Worker *wk = worker;
        worker = nullptr;
        if (queue) {
            queue->Post([wk]() {
                if (wk && wk->st) ls_stitch_destroy(wk->st);
                delete wk;
            });
            queue->Shutdown();
            queue = nullptr;
        }
    }

    void Finish() {
        if (finished) return;
        finished = true;
        Teardown();
        Worker *wk = worker;
        worker = nullptr;
        float sc = scale;
        HMONITOR m = mon;
        if (!queue) return;
        queue->Post([wk, sc, m]() {
            ls_image out = {};
            bool ok = wk && wk->st && ls_stitch_height(wk->st) > 0 && ls_stitch_render(wk->st, &out) == 0 && out.data;
            if (wk && wk->st) ls_stitch_destroy(wk->st);
            delete wk;
            if (!ok) return;
            tool::PostToUi([out, sc, m]() {
                if (settings::Bool("general.playSound")) app::PlayShutter();
                ShowResult(out, sc, m);
            });
        });
        queue->Shutdown();
        queue = nullptr;
    }
};

LRESULT PanelWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    float s = S();
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
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
        float x = MouseX(lp) / s, y = MouseY(lp) / s;
        if (pressed) return 0;
        SetHover(panel.HitTest(x, y));
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        if (!pressed) SetHover(nullptr);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        float x = MouseX(lp) / s, y = MouseY(lp) / s;
        prefs::Widget *w = panel.HitTest(x, y);
        if (!w) return 0;
        pressed = w;
        w->pressed = true;
        SetCapture(hwnd_);
        Render();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!pressed) return 0;
        prefs::Widget *w = pressed;
        pressed = nullptr;
        w->pressed = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
        float x = MouseX(lp) / s, y = MouseY(lp) / s;
        Render();
        w->OnMouseUp(x, y, w->frame.Contains(x, y));  // 可能结束长截图（对象延迟释放）
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pressed && (HWND)lp != hwnd_) {
            pressed->pressed = false;
            pressed = nullptr;
            Render();
        }
        return 0;
    case WM_TIMER:
        if (owner) owner->OnTimer(wp);
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_ESC && owner) owner->Cancel();
        return 0;
    case WM_DPICHANGED: return 0;  // 位置大小固定（按区域所在屏）
    case WM_CLOSE:
        if (owner) owner->Cancel();
        return 0;
    }
    return Window::Handle(msg, wp, lp);
}

}  // namespace

void Start() {
    if (g_session || capture::IsActive()) return;
    capture::Start(capture::Mode::LongShot);
}

void StartWithRegion(HMONITOR monitor, RECT regionPx) {
    if (g_session) return;
    if (regionPx.right - regionPx.left < 8 || regionPx.bottom - regionPx.top < 8) return;
    if (!monitor) monitor = MonitorFromRect(&regionPx, MONITOR_DEFAULTTONEAREST);
    Session *s = new Session();
    s->id = g_nextId++;
    g_session = s;
    if (!s->Begin(monitor, regionPx)) {
        LS_LOG("longshot: 启动失败");
        g_session = nullptr;
        s->finished = true;
        if (s->queue) {
            Worker *wk = s->worker;
            s->worker = nullptr;
            s->queue->Post([wk]() { delete wk; });
            s->queue->Shutdown();
            s->queue = nullptr;
        }
        delete s->worker;
        s->worker = nullptr;
        delete s;
    }
}

bool IsActive() { return g_session != nullptr; }

ls_image RenderSelfTest(float scale, const ls_image &screen, float screenW, float screenH, RectF region, const ls_image &preview,
                        int heightPx) {
    PanelWidget w;
    float bw = K().recordBar.regionBorderWidth;
    float h = std::fmin(std::fmax(region.h, kMinPanelH), screenH - 16);
    float y = Clamp(region.y, 8.f, screenH - 8 - h);
    w.frame = RectF(region.r() + 10 + bw, y, kPanelW, h);
    w.SetAuto(true);
    w.hint = T("longshot.hint");
    w.heightPx = heightPx;
    w.Layout();
    return tool::RenderOffscreen(screenW, screenH, scale, [&](Canvas &c) {
        ComPtr<ID2D1Bitmap> bg = gfx::CreateBitmap(c.RT(), screen);
        if (bg) c.Bitmap(bg.Get(), RectF(0, 0, screenW, screenH), nullptr, 1.f, true);
        Color bc = K().recordBar.regionBorder;
        c.FillRect(RectF(region.x - bw, region.y - bw, region.w + 2 * bw, bw), bc);
        c.FillRect(RectF(region.x - bw, region.b(), region.w + 2 * bw, bw), bc);
        c.FillRect(RectF(region.x - bw, region.y, bw, region.h), bc);
        c.FillRect(RectF(region.r(), region.y, bw, region.h), bc);
        ComPtr<ID2D1Bitmap> pv = gfx::CreateBitmap(c.RT(), preview);
        w.preview = pv.Get();
        w.previewPxW = (float)preview.width;
        w.previewPxH = (float)preview.height;
        w.Draw(c);
    });
}

}  // namespace longshot
}  // namespace ls
