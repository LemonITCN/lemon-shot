#include "Audio/Audio.h"

#include <windows.h>

#include <cmath>
#include <memory>

#include "App/App.h"
#include "Capture/Export.h"
#include "Record/MF.h"
#include "Record/Pipeline.h"
#include "Record/RecordWindows.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Toast.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace audio {

namespace {

enum { BTN_CLOSE = 1, BTN_MIC, BTN_SYSTEM, BTN_ACTION };
const float kPad = 14;
const float kMargin = 18;  // 面板四周留给阴影的边距
enum { TIMER_TICK = 1, TIMER_TIP = 2 };

const Tokens &K() { return theme::Tok(); }
const UiColors &U() { return theme::Ui(); }

TextStyle Ts(float size, int weight, Color c, HAlign h = HAlign::Left) {
    TextStyle t;
    t.size = size;
    t.weight = weight;
    t.color = c;
    t.h = h;
    t.v = VAlign::Center;
    return t;
}

struct Layout {
    RectF close, mic, system, action, meter, title, clock;
};

float ChipW(const std::wstring &title) { return std::ceil(10 + 16 + 6 + gfx::Measure(title, K().fontSize.body - 1, kRegular).w + 12); }

Layout LayoutPanel(RectF r, const PanelView &v) {
    Layout l;
    float ch = K().window.controlHeight;
    l.close = RectF(r.r() - kPad - 24 + 6, r.y + 12, 24, 24);
    l.mic = RectF(r.x + kPad, r.y + 46, ChipW(T("record.microphone")), ch);
    l.system = RectF(l.mic.r() + 8, r.y + 46, ChipW(T("record.systemAudio")), ch);
    const std::wstring &label = T(v.recording ? "audio.stop" : "audio.start");
    float bw = std::ceil(gfx::Measure(label, K().fontSize.body, kMedium).w + 2 * K().window.buttonPaddingX);
    if (bw < 84) bw = 84;
    l.action = RectF(r.r() - kPad - bw, r.y + 80, bw, ch);
    l.meter = RectF(r.x + kPad, l.action.cy() - 3, l.action.x - 14 - (r.x + kPad), 6);
    l.title = RectF(r.x + kPad, r.y + 12, 150, 24);
    float cw = std::ceil(gfx::Measure(v.clock, K().fontSize.body, kMedium).w) + 2;
    l.clock = RectF(l.close.x - 8 - cw, r.y + 12, cw, 24);
    return l;
}

void DrawSourceChip(Canvas &c, RectF r, const char *icon, const char *offIcon, const std::wstring &title, bool on, bool hover,
                    bool enabled) {
    const auto &u = U();
    float a = enabled ? 1.f : 0.45f;
    float rad = K().window.controlRadius;
    Color bg = on ? u.surfaceSelected : (hover && enabled ? u.surfaceHover : WithAlpha(u.controlTrack, 0.6f));
    c.FillRound(r, rad, WithAlpha(bg, a));
    Color fg = on ? u.iconActive : u.textSecondary;
    c.Icon(on ? icon : offIcon, r.x + 18, r.cy(), 16, WithAlpha(fg, a), 1.6f);
    c.Text(title, RectF(r.x + 32, r.y, r.w - 38, r.h), Ts(K().fontSize.body - 1, kRegular, WithAlpha(on ? u.text : u.textSecondary, a)));
}

}  // namespace

SizeF PanelSize() { return SizeF{300, 120}; }

float Meter(float peak) {
    if (peak <= 0.00001f) return 0;
    double db = 20 * std::log10((double)peak);
    double v = (db + 50) / 50;
    return (float)(v < 0 ? 0 : (v > 1 ? 1 : v));
}

void DrawPanel(Canvas &c, RectF r, const PanelView &v) {
    const auto &u = U();
    const auto &t = K().toolbar;
    c.Shadow(r, 12, t.shadowBlur, t.shadowOffsetY, u.shadow);
    c.FillRound(r, 12, u.popoverBg);
    c.StrokeRound(r.Inflate(-0.5f), 11.5f, u.border, 1);
    Layout l = LayoutPanel(r, v);
    float body = K().fontSize.body;
    // 标题行：图标 + 「录音」，右侧计时（录音中前面红点）与关闭
    c.Icon("audio-lines", l.title.x + 8, l.title.cy(), 16, u.icon, 1.6f);
    c.Text(T("audio.title"), l.title.Offset(22, 0), Ts(body, kSemibold, u.text));
    TextStyle cs = Ts(body, kMedium, v.recording ? u.text : u.textSecondary);
    cs.tabular = true;
    c.Text(v.clock, RectF(l.clock.x, l.clock.y, l.clock.w + 4, l.clock.h), cs);
    if (v.recording) c.FillEllipse(l.clock.x - 6 - 4, l.clock.cy(), 4, 4, K().recordBar.dot);
    if (v.pressed == BTN_CLOSE)
        c.FillRound(l.close, 6, u.surfacePressed);
    else if (v.hover == BTN_CLOSE)
        c.FillRound(l.close, 6, u.surfaceHover);
    c.Icon("x", l.close.cx(), l.close.cy(), 14, u.icon);
    // 来源开关 + 格式
    DrawSourceChip(c, l.mic, "mic", "mic-off", T("record.microphone"), v.mic, v.hover == BTN_MIC, !v.recording);
    DrawSourceChip(c, l.system, "volume-2", "volume-x", T("record.systemAudio"), v.system, v.hover == BTN_SYSTEM, !v.recording);
    c.Text(v.format, RectF(r.r() - kPad - 60, l.mic.y, 60, l.mic.h), Ts(K().fontSize.caption, kRegular, u.textTertiary, HAlign::Right));
    // 电平条
    c.FillRound(l.meter, l.meter.h / 2, u.controlTrack);
    float lv = v.level < 0 ? 0 : (v.level > 1 ? 1 : v.level);
    if (lv > 0.001f) {
        float w = std::fmax(l.meter.h, l.meter.w * lv);
        c.FillRound(RectF(l.meter.x, l.meter.y, w, l.meter.h), l.meter.h / 2, lv > 0.92f ? u.warning : u.success);
    }
    // 开始 / 停止
    float rad = K().window.controlRadius;
    bool hot = v.hover == BTN_ACTION, down = v.pressed == BTN_ACTION;
    if (v.recording) {
        Color bg = down ? u.surfacePressed : (hot ? u.surfaceHover : u.surface);
        c.FillRound(l.action, rad, bg);
        c.StrokeRound(l.action.Inflate(-0.5f), rad - 0.5f, u.inputBorder, 1);
        c.Text(T("audio.stop"), l.action, Ts(body, kMedium, u.danger, HAlign::Center));
    } else {
        c.FillRound(l.action, rad, hot || down ? u.accentHover : u.accent);
        c.Text(T("audio.start"), l.action, Ts(body, kMedium, u.accentText, HAlign::Center));
    }
}

namespace {

class Panel : public Window {
public:
    PanelView view;
    std::unique_ptr<rec::Pipeline> pipe;
    bool stopping = false;
    float shownLevel = 0;
    UINT dpi = 96;
    POINT pos = {};  // 面板本体左上角（屏幕像素）
    LayeredSurface surface;
    rec::TipWindow tip;
    bool tracking = false;
    bool dragging = false;
    POINT dragFrom = {};
    std::wstring path;

    bool Open();
    void Render();
    void Begin();
    void StopRecording();
    void CloseOrStop();
    int Hit(float x, float y);

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;
};

Panel *g_panel = nullptr;

void DeletePanel(void *p) { delete (Panel *)p; }

void ClosePanel() {
    Panel *p = g_panel;
    if (!p) return;
    g_panel = nullptr;
    p->tip.DestroyWin();
    if (p->Hwnd()) ShowWindow(p->Hwnd(), SW_HIDE);
    app::Defer(&DeletePanel, p);
}

std::string EffectiveFormat() {
    std::string f = settings::Str("audio.format");
    bool mf = mf::Available();
    if (f == "mp3" && !(mf && mf::HasMp3Encoder())) f = "m4a";
    if (f != "wav" && f != "mp3") f = "m4a";
    if (!mf) f = "wav";  // 没有 Media Foundation：只能写 WAV
    return f;
}

std::wstring FormatLabel(const std::string &f) {
    if (f == "wav") return L"WAV";
    if (f == "mp3") return L"MP3";
    return L"M4A";
}

bool Panel::Open() {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    dpi = api::MonitorDpi(mon);
    float s = dpi / 96.f;
    SizeF sz = PanelSize();
    pos = POINT{mi.rcWork.right - (int)std::lround((16 + sz.w) * s), mi.rcWork.top + (int)std::lround(16 * s)};
    view.mic = settings::Bool("audio.microphone");
    view.system = settings::Bool("audio.systemAudio");
    view.format = FormatLabel(EffectiveFormat());
    if (!CreateWin(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP, 0, 0, 1, 1, nullptr, L"Lemon Shot"))
        return false;
    api::ExcludeFromCapture(hwnd_);
    Render();
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

void Panel::Render() {
    if (!hwnd_) return;
    float s = dpi / 96.f;
    SizeF sz = PanelSize();
    int w = (int)std::ceil((sz.w + 2 * kMargin) * s), h = (int)std::ceil((sz.h + 2 * kMargin) * s);
    ID2D1RenderTarget *rt = surface.Begin(w, h, dpi);
    if (!rt) return;
    {
        Canvas c(rt);
        DrawPanel(c, RectF(kMargin, kMargin, sz.w, sz.h), view);
    }
    int m = (int)std::lround(kMargin * s);
    surface.End(hwnd_, pos.x - m, pos.y - m, 255);
}

int Panel::Hit(float x, float y) {
    SizeF sz = PanelSize();
    Layout l = LayoutPanel(RectF(0, 0, sz.w, sz.h), view);
    if (l.close.Contains(x, y)) return BTN_CLOSE;
    if (!view.recording && l.mic.Contains(x, y)) return BTN_MIC;
    if (!view.recording && l.system.Contains(x, y)) return BTN_SYSTEM;
    if (l.action.Contains(x, y)) return BTN_ACTION;
    return 0;
}

void Panel::Begin() {
    if (pipe || stopping) return;
    bool wantMic = view.mic, wantSys = view.system;
    if (!wantMic && !wantSys) return;
    std::string fmt = EffectiveFormat();
    std::wstring folder = settings::ExpandPath(settings::Str("audio.folder"));
    if (!EnsureDir(folder)) {
        toast::Show(T("notify.folderMissing"), "triangle-alert");
        return;
    }
    path = UniquePath(folder, exporter::ExpandFileName("Lemon Shot {yyyy-MM-dd HH.mm.ss}"), Widen(fmt));
    rec::PipelineConfig pc;
    pc.kind = fmt == "wav" ? rec::OutputKind::Wav : (fmt == "mp3" ? rec::OutputKind::Mp3 : rec::OutputKind::M4a);
    pc.mic = wantMic;
    pc.micDevice = settings::Str("audio.microphoneDevice");
    pc.system = wantSys;
    pc.path = path;
    pipe.reset(new rec::Pipeline());
    if (!pipe->Prepare(pc, hwnd_)) {
        pipe.reset();
        toast::Show(i18n::T("audio.failed", {{"error", T("common.error")}}), "triangle-alert");
        return;
    }
    view.format = FormatLabel(fmt);
    view.recording = true;
    view.clock = rec::ClockText(0);
    view.hover = view.pressed = 0;
    shownLevel = 0;
    view.level = 0;
    Render();
    LS_LOG("audio: 开始录音 %s", fmt.c_str());
}

void Panel::StopRecording() {
    if (!pipe || stopping) return;
    stopping = true;
    KillTimer(hwnd_, TIMER_TICK);
    pipe->RequestStop();
    view.recording = false;
    view.level = 0;
    Render();
}

void Panel::CloseOrStop() {
    if (pipe)
        StopRecording();
    else
        ClosePanel();
}

LRESULT Panel::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    float s = dpi / 96.f;
    float x = MouseX(lp) / s - kMargin, y = MouseY(lp) / s - kMargin;
    switch (msg) {
    case rec::WM_REC_READY:
        if (!pipe || stopping) return 0;  // 准备好之前已经点了停止：写入线程会自己收尾
        if (!wp || ((!view.mic || pipe->MicFailed()) && (!view.system || pipe->SystemFailed()))) {
            // 打不开任何来源
            std::wstring err = pipe->MicFailed() ? T("record.noMicrophone") : T("common.error");
            pipe->Abort();
            pipe.reset();
            view.recording = false;
            Render();
            toast::Show(i18n::T("audio.failed", {{"error", err}}), "triangle-alert");
            return 0;
        }
        if (pipe->MicFailed()) toast::Show(T("record.noMicrophone"), "triangle-alert");
        pipe->Begin();
        SetTimer(hwnd_, TIMER_TICK, 50, nullptr);
        return 0;
    case rec::WM_REC_FAILED: StopRecording(); return 0;
    case rec::WM_REC_DONE: {
        if (pipe) pipe->Join();
        pipe.reset();
        stopping = false;
        Render();
        if (wp) {
            toast::Action a;
            a.kind = toast::kReveal;
            a.path = path;
            toast::Show(T("audio.saved"), "circle-check", a);
        } else {
            toast::Show(i18n::T("audio.failed", {{"error", T("common.error")}}), "triangle-alert");
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_TICK && pipe && !stopping) {
            view.clock = rec::ClockText(pipe->Seconds());
            float m = Meter(pipe->TakeLevel());
            if (m > shownLevel) shownLevel = m;
            view.level = shownLevel;
            shownLevel *= 0.82f;
            Render();
            return 0;
        }
        if (wp == TIMER_TIP) {
            KillTimer(hwnd_, TIMER_TIP);
            if (view.hover == BTN_CLOSE && !view.pressed) {
                SizeF sz = PanelSize();
                Layout l = LayoutPanel(RectF(0, 0, sz.w, sz.h), view);
                RECT a = {pos.x + (LONG)std::floor(l.close.x * s), pos.y + (LONG)std::floor(l.close.y * s),
                          pos.x + (LONG)std::ceil(l.close.r() * s), pos.y + (LONG)std::ceil(l.close.b() * s)};
                tip.Show(T("common.close"), a, true, dpi);
            }
            return 0;
        }
        break;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
        return TRUE;
    case WM_MOUSEMOVE: {
        if (!tracking) {
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking = true;
        }
        if (dragging) {
            POINT pt;
            GetCursorPos(&pt);
            pos.x += pt.x - dragFrom.x;
            pos.y += pt.y - dragFrom.y;
            dragFrom = pt;
            Render();
            return 0;
        }
        int h = Hit(x, y);
        if (view.pressed) h = h == view.pressed ? h : 0;
        if (h != view.hover) {
            view.hover = h;
            tip.Hide();
            KillTimer(hwnd_, TIMER_TIP);
            if (h == BTN_CLOSE) SetTimer(hwnd_, TIMER_TIP, 500, nullptr);
            Render();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        if (!view.pressed && view.hover) {
            view.hover = 0;
            tip.Hide();
            Render();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int h = Hit(x, y);
        tip.Hide();
        SetCapture(hwnd_);
        if (h) {
            view.pressed = h;
            view.hover = h;
            Render();
        } else {
            // 空白处拖动面板
            dragging = true;
            GetCursorPos(&dragFrom);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (dragging) {
            dragging = false;
            return 0;
        }
        int id = view.pressed;
        view.pressed = 0;
        bool inside = id && Hit(x, y) == id;
        view.hover = Hit(x, y);
        Render();
        if (!inside) return 0;
        switch (id) {
        case BTN_CLOSE: CloseOrStop(); break;
        case BTN_MIC:
            view.mic = !view.mic;
            settings::SetBool("audio.microphone", view.mic);
            Render();
            break;
        case BTN_SYSTEM:
            view.system = !view.system;
            settings::SetBool("audio.systemAudio", view.system);
            Render();
            break;
        case BTN_ACTION:
            if (pipe)
                StopRecording();
            else
                Begin();
            break;
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd_) {
            dragging = false;
            if (view.pressed) {
                view.pressed = 0;
                Render();
            }
        }
        return 0;
    }
    return Window::Handle(msg, wp, lp);
}

}  // namespace

void Toggle() {
    if (!g_panel) {
        Panel *p = new Panel();
        g_panel = p;
        if (!p->Open()) {
            g_panel = nullptr;
            delete p;
        }
        return;
    }
    if (g_panel->pipe) {
        g_panel->StopRecording();
        return;
    }
    if (!g_panel->stopping) ClosePanel();
}

bool IsRecording() { return g_panel && g_panel->pipe && !g_panel->stopping; }

void Shutdown() {
    Panel *p = g_panel;
    if (!p) return;
    g_panel = nullptr;
    if (p->pipe) {
        if (!p->stopping) p->pipe->RequestStop();
        p->pipe->Join();
        p->pipe.reset();
    }
    delete p;
}

}  // namespace audio
}  // namespace ls
