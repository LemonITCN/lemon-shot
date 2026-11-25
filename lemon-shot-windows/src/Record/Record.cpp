// 录屏控制器（spec 8）：界面线程上的状态机。
//   倒计时（采集与编码器在后台准备）→ 录制（时间线开始，控制条、托盘红点）→ 收尾（编码器写完 / GIF 编码）→ 录完之后
#include "Record/Record.h"

#include <cmath>
#include <cstring>
#include <memory>

#include "App/App.h"
#include "App/Tray.h"
#include "Capture/Capture.h"
#include "Capture/Export.h"
#include "Capture/StyleSettings.h"
#include "Record/Camera.h"
#include "Record/Hooks.h"
#include "Record/MF.h"
#include "Record/Overlays.h"
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
namespace record {

using namespace rec;

namespace {

enum Phase { kCountdown, kRecording, kFinishing };
enum { TIMER_TICK = 1, TIMER_ARM = 2 };

int ClampFps(int v) {
    const int list[4] = {15, 24, 30, 60};
    int best = 2;
    for (int i = 0; i < 4; ++i)
        if (std::abs(list[i] - v) < std::abs(list[best] - v)) best = i;
    return list[best];
}

class Session : public Window {
public:
    Phase phase = kCountdown;
    HMONITOR mon = nullptr;
    RECT region = {};
    RECT monRect = {}, workRect = {};
    UINT dpi = 96;
    bool fullscreen = false;
    bool gif = false;
    std::wstring path;
    std::string afterAction;
    int maxSeconds = 0;
    bool pipeReady = false, countdownDone = false;
    bool paused = false;
    bool barOnlyWhenPaused = false;
    SizeF barSize;

    std::unique_ptr<Pipeline> pipe;
    std::unique_ptr<BarWindow> bar;
    std::unique_ptr<BorderWindows> border;
    std::unique_ptr<CountdownWindow> countdown;
    std::unique_ptr<PenWindow> pen;
    std::unique_ptr<ClickRipples> clicks;
    std::unique_ptr<KeysWindow> keys;
    std::unique_ptr<CameraWindow> camera;
    std::unique_ptr<ProgressWindow> progress;

    bool Begin(HMONITOR m, RECT r);
    void Arm();
    void ShowBar();
    bool BarFrame(SizeF size, bool full, bool outsideOnly, POINT &out, int *side = nullptr);
    void Tick();
    void TogglePause();
    void TogglePen();
    void ClosePen();
    void RaiseChrome();
    void Stop();
    void Cancel();
    void Fail(int err);
    void TeardownUi();
    void Finished(bool ok);
    void Destroy();

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;
};

Session *g_session = nullptr;

void DeleteSession(void *p) { delete (Session *)p; }

float Scale(UINT dpi) { return dpi / 96.f; }

// ---------------------------------------------------------------- 开始

bool Session::Begin(HMONITOR m, RECT r) {
    mon = m;
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(m, &mi);
    monRect = mi.rcMonitor;
    workRect = mi.rcWork;
    dpi = api::MonitorDpi(m);
    // 夹在屏内
    RECT in;
    if (!IntersectRect(&in, &r, &monRect)) return false;
    region = in;
    if (region.right - region.left < 2 || region.bottom - region.top < 2) return false;
    fullscreen = EqualRect(&region, &monRect) != 0;
    float s = Scale(dpi);

    gif = settings::Str("record.format") == "gif";
    afterAction = settings::Str("record.afterAction");
    maxSeconds = settings::Int("record.maxDurationSec");
    if (maxSeconds < 0) maxSeconds = 0;
    std::wstring folder = settings::ExpandPath(settings::Str("record.folder"));
    if (!EnsureDir(folder)) {
        toast::Show(T("notify.folderMissing"), "triangle-alert");
        return false;
    }
    path = UniquePath(folder, exporter::ExpandFileName("LemonShot {yyyy-MM-dd HH.mm.ss}"), gif ? L"gif" : L"mp4");

    PipelineConfig pc;
    pc.kind = gif ? OutputKind::Gif : OutputKind::Mp4;
    pc.monitor = m;
    pc.region = region;
    pc.cursor = settings::Bool("record.showCursor");
    int pw = region.right - region.left, ph = region.bottom - region.top;
    if (gif) {
        // GIF：区域逻辑尺寸（1 倍）× gifScale，不受清晰度影响
        double gs = Clamp(settings::Num("record.gifScale"), 0.1, 1.0);
        pc.fps = Clamp(settings::Int("record.gifFps"), 5, 30);  // 与偏好设置的范围一致
        pc.outW = (int)std::lround(pw / s * gs);
        pc.outH = (int)std::lround(ph / s * gs);
        if (pc.outW < 2) pc.outW = 2;
        if (pc.outH < 2) pc.outH = 2;
        if (pc.outW > pw) pc.outW = pw;
        if (pc.outH > ph) pc.outH = ph;
    } else {
        pc.fps = ClampFps(settings::Int("record.fps"));
        VideoSize(pw, ph, settings::Str("record.quality"), &pc.outW, &pc.outH);
        if (pc.outW > pw) pc.outW = pw & ~1;
        if (pc.outH > ph) pc.outH = ph & ~1;
        pc.bitrate = VideoBitrate(pc.outW, pc.outH, pc.fps);
        pc.mic = settings::Bool("record.microphone");
        pc.micDevice = settings::Str("record.microphoneDevice");
        pc.system = settings::Bool("record.systemAudio");
    }
    pc.path = path;
    LS_LOG("record: 开始 %s 区域 %dx%d 输出 %dx%d %d 帧/秒", gif ? "GIF" : "MP4", pw, ph, pc.outW, pc.outH, pc.fps);

    if (!CreateWin(0, 0, 0, 0, 0, 0, HWND_MESSAGE, L"")) return false;
    pipe.reset(new Pipeline());
    if (!pipe->Prepare(pc, hwnd_)) {
        pipe.reset();
        return false;
    }

    // 不进画面的边框；进画面的浮层（开始前就显示）
    if (!fullscreen) {
        border.reset(new BorderWindows());
        border->Show(region, dpi);
    }
    bool wantClicks = settings::Bool("record.showClicks");
    bool wantKeys = settings::Bool("record.showKeys");
    if (wantClicks) {
        clicks.reset(new ClickRipples());
        if (!clicks->Init(m)) clicks.reset();
    }
    if (wantKeys) {
        keys.reset(new KeysWindow());
        if (!keys->Init(region, dpi)) keys.reset();
    }
    if (clicks || keys) hooks::Start(hwnd_, keys != nullptr, clicks != nullptr);
    if (settings::Bool("record.camera") && mf::Available()) {
        camera.reset(new CameraWindow());
        camera->onFailed = [this]() {
            toast::Show(T("record.noCamera"), "triangle-alert");
            if (camera) camera->Close();
        };
        if (!camera->Open(settings::Str("record.cameraDevice"), region, dpi, settings::Str("record.cameraShape") == "rounded"))
            camera.reset();
    }

    phase = kCountdown;
    int seconds = Clamp(settings::Int("record.countdown"), 0, 10);
    if (seconds > 0) {
        countdown.reset(new CountdownWindow());
        countdown->onDone = [this]() {
            countdownDone = true;
            Arm();
        };
        countdown->onCancel = [this]() { Cancel(); };
        if (!countdown->Start(region, dpi, seconds)) {
            countdown.reset();
            SetTimer(hwnd_, TIMER_ARM, 150, nullptr);
        }
    } else {
        // 没有倒计时：等截图层从屏幕上消失再开始
        SetTimer(hwnd_, TIMER_ARM, 150, nullptr);
    }
    (void)s;
    return true;
}

// 倒计时走完且编码器、采集都准备好：时间线从现在开始
void Session::Arm() {
    if (phase != kCountdown || !pipeReady || !countdownDone) return;
    phase = kRecording;
    pipe->Begin();
    ShowBar();
    SetTimer(hwnd_, TIMER_TICK, 250, nullptr);
    tray::Refresh();
    LS_LOG("record: 录制开始");
}

// 控制条位置（spec 8.2）：区域下方居中，放不下放上方；全屏放屏幕底部居中。outsideOnly：只接受区域外的位置
bool Session::BarFrame(SizeF size, bool full, bool outsideOnly, POINT &out, int *side) {
    if (side) *side = 0;
    float s = Scale(dpi);
    const auto &t = theme::Tok();
    int w = (int)std::ceil(size.w * s), h = (int)std::ceil(size.h * s);
    // 排除不了的系统：控制条的阴影也不能压进区域
    float gapPt = t.capture.toolbarGap + t.recordBar.regionBorderWidth + (outsideOnly ? t.toolbar.shadowBlur + t.toolbar.shadowOffsetY : 0);
    int gap = (int)std::lround(gapPt * s);
    int margin = (int)std::lround(8 * s);
    auto clampX = [&](int x) {
        if (x > monRect.right - w - margin) x = monRect.right - w - margin;
        if (x < monRect.left + margin) x = monRect.left + margin;
        return x;
    };
    if (full) {
        if (outsideOnly) return false;
        out = POINT{clampX((monRect.left + monRect.right) / 2 - w / 2), workRect.bottom - (int)std::lround(24 * s) - h};
        return true;
    }
    int x = clampX((region.left + region.right) / 2 - w / 2);
    int below = region.bottom + gap;
    if (below + h <= workRect.bottom - margin) {
        out = POINT{x, below};
        if (side) *side = 1;
        return true;
    }
    int above = region.top - gap - h;
    if (above >= workRect.top + margin) {
        out = POINT{x, above};
        if (side) *side = -1;
        return true;
    }
    if (outsideOnly) return false;
    out = POINT{x, region.bottom - gap - h};
    return true;
}

void Session::ShowBar() {
    bar.reset(new BarWindow());
    if (!bar->Create(dpi)) {
        bar.reset();
        return;
    }
    bar->onButton = [this](int id) {
        switch (id) {
        case BAR_PAUSE: TogglePause(); break;
        case BAR_PEN: TogglePen(); break;
        case BAR_STOP: Stop(); break;
        case BAR_CANCEL: Cancel(); break;
        }
    };
    barSize = bar->BarSize();
    bool outsideOnly = !bar->Excluded();
    POINT p;
    int side = 0;
    if (BarFrame(barSize, fullscreen, outsideOnly, p, &side)) {
        bar->tipSide = side;
        bar->Place(p.x, p.y);
        bar->Show();
        bar->SetAutoFade(fullscreen);
    } else if (BarFrame(barSize, true, false, p)) {
        // 排除不了窗口、区域外又放不下：只在暂停时显示
        barOnlyWhenPaused = true;
        bar->Place(p.x, p.y);
    }
}

void Session::Tick() {
    if (phase != kRecording) return;
    double e = pipe->Seconds();
    if (bar) {
        bar->view.clock = ClockText(e);
        SizeF sz = bar->BarSize();
        if (std::fabs(sz.w - barSize.w) > 0.5f) {
            // 满一小时计时变宽：重新摆放保持居中
            barSize = sz;
            POINT p;
            bool outsideOnly = !bar->Excluded();
            if (barOnlyWhenPaused ? BarFrame(sz, true, false, p) : BarFrame(sz, fullscreen, outsideOnly, p)) bar->Place(p.x, p.y);
        }
        if (bar->Visible()) bar->Render();
    }
    if (maxSeconds > 0 && e >= maxSeconds) Stop();
}

void Session::TogglePause() {
    if (phase != kRecording) return;
    paused = !paused;
    if (paused)
        pipe->Pause();
    else
        pipe->Resume();
    if (bar) {
        bar->view.paused = paused;
        if (barOnlyWhenPaused) {
            if (paused)
                bar->Show();
            else
                bar->Hide();
        } else {
            bar->Render();
        }
    }
    tray::Refresh();
}

void Session::RaiseChrome() {
    // 画笔层盖在区域上：把按键浮层、摄像头、控制条重新提到它上面
    UINT f = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    if (keys && keys->Hwnd() && IsWindowVisible(keys->Hwnd())) SetWindowPos(keys->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, f);
    if (camera && camera->Hwnd() && IsWindowVisible(camera->Hwnd())) SetWindowPos(camera->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, f);
    if (bar && bar->Hwnd() && bar->Visible()) SetWindowPos(bar->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0, f);
}

void Session::TogglePen() {
    if (phase != kRecording) return;
    if (pen && pen->IsOpen()) {
        ClosePen();
        return;
    }
    if (!pen) pen.reset(new PenWindow());
    pen->onEscape = [this]() { app::Defer([](void *p) { ((Session *)p)->ClosePen(); }, this); };
    std::vector<Color> pal = style::Palette();
    Color col = pal.empty() ? ParseColor("#FF3B30") : pal[0];
    col.a = 1;
    if (!pen->Open(region, dpi, col)) return;
    RaiseChrome();
    if (bar) {
        bar->view.penOn = true;
        bar->Render();
    }
}

void Session::ClosePen() {
    if (g_session != this) return;
    if (pen) pen->Close();
    if (bar) {
        bar->view.penOn = false;
        if (bar->Visible()) bar->Render();
    }
}

// ---------------------------------------------------------------- 结束

void Session::TeardownUi() {
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_TICK);
        KillTimer(hwnd_, TIMER_ARM);
    }
    hooks::Stop();
    if (countdown) countdown->Close();
    if (bar) bar->Close();
    if (border) border->Close();
    if (pen) pen->Close();
    if (clicks) clicks->Close();
    if (keys) keys->Close();
    if (camera) camera->Close();
}

void Session::Stop() {
    if (phase == kCountdown) {
        Cancel();
        return;
    }
    if (phase != kRecording) return;
    phase = kFinishing;
    TeardownUi();
    pipe->RequestStop();
    tray::Refresh();
    if (gif) {
        progress.reset(new ProgressWindow());
        progress->Show(mon, T("record.encodingGif"));
    }
    LS_LOG("record: 停止，时长 %.2f 秒", pipe->DurationSeconds());
}

void Session::Cancel() {
    if (phase == kFinishing) return;
    TeardownUi();
    if (pipe) pipe->Abort();
    LS_LOG("record: 已取消");
    Destroy();
}

void Session::Fail(int err) {
    TeardownUi();
    if (pipe) pipe->Abort();
    std::wstring msg;
    switch (err) {
    case kErrEncoder: msg = T("record.encoderMissing"); break;
    case kErrFile: msg = T("notify.folderMissing"); break;
    default: msg = i18n::T("record.failed", {{"error", FormatW(L"%d", err)}}); break;
    }
    toast::Show(msg, "triangle-alert");
    Destroy();
}

void Session::Finished(bool ok) {
    if (pipe) pipe->Join();
    if (progress) progress->Close();
    std::wstring file = path;
    std::string after = afterAction;
    Destroy();
    if (!ok) {
        toast::Show(i18n::T("record.failed", {{"error", T("common.error")}}), "triangle-alert");
        return;
    }
    if (after == "play")
        ShellOpen(file);
    else if (after == "reveal")
        RevealInFolder(file);
    toast::Action a;
    a.kind = toast::kReveal;
    a.path = file;
    toast::Show(T("record.saved"), "circle-check", a);
}

// 从全局状态摘下并延迟删除（可能正处在本对象某个窗口的消息处理里）
void Session::Destroy() {
    if (g_session == this) g_session = nullptr;
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_TICK);
        KillTimer(hwnd_, TIMER_ARM);
    }
    tray::Refresh();
    app::Defer(&DeleteSession, this);
}

LRESULT Session::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (g_session != this) return Window::Handle(msg, wp, lp);
    switch (msg) {
    case WM_REC_READY:
        if (!wp) {
            Fail((int)lp);
        } else {
            pipeReady = true;
            if (pipe->MicFailed()) toast::Show(T("record.noMicrophone"), "triangle-alert");
            Arm();
        }
        return 0;
    case WM_REC_FAILED:
        if (phase == kRecording) Stop();
        return 0;
    case WM_REC_PROGRESS:
        if (progress) progress->SetProgress((double)wp / 1000.0);
        return 0;
    case WM_REC_DONE:
        if (phase == kFinishing) Finished(wp != 0);
        return 0;
    case WM_REC_KEY:
        if (keys && phase != kFinishing)
            keys->OnKey((UINT)(wp & 0xFFFF), (int)((wp >> 16) & 0xFF), (UINT)(lp & 0xFFFF), (lp & 0x10000) != 0);
        return 0;
    case WM_REC_CLICK:
        if (clicks && phase != kFinishing) clicks->Add((short)LOWORD(lp), (short)HIWORD(lp), wp == 1);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TICK) {
            Tick();
            return 0;
        }
        if (wp == TIMER_ARM) {
            KillTimer(hwnd_, TIMER_ARM);
            countdownDone = true;
            Arm();
            return 0;
        }
        break;
    }
    return Window::Handle(msg, wp, lp);
}

// 托盘红点
HICON MakeDotIcon(int size, Color col) {
    if (size <= 0) size = 16;
    Offscreen off;
    if (!off.Create(size, size, 1.f)) return nullptr;
    ID2D1RenderTarget *rt = off.RT();
    rt->BeginDraw();
    {
        Canvas c(rt);
        c.Clear(Color{0, 0, 0, 0});
        float r = size * 0.36f;
        c.FillEllipse(size / 2.f, size / 2.f, r, r, col);
    }
    if (FAILED(rt->EndDraw(nullptr, nullptr))) return nullptr;
    ls_image img;
    if (!off.ReadPixels(img)) return nullptr;
    BITMAPV5HEADER bi;
    std::memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void *bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HICON icon = nullptr;
    if (color && bits) {
        uint8_t *d = (uint8_t *)bits;
        for (int y = 0; y < size; ++y) {
            const uint8_t *src = img.data + (size_t)y * img.stride;
            for (int x = 0; x < size; ++x, src += 4, d += 4) {
                uint8_t a = src[3];
                d[0] = a ? (uint8_t)((src[0] * 255 + a / 2) / a) : 0;
                d[1] = a ? (uint8_t)((src[1] * 255 + a / 2) / a) : 0;
                d[2] = a ? (uint8_t)((src[2] * 255 + a / 2) / a) : 0;
                d[3] = a;
            }
        }
        HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
        ICONINFO ii;
        std::memset(&ii, 0, sizeof ii);
        ii.fIcon = TRUE;
        ii.hbmMask = mask;
        ii.hbmColor = color;
        icon = CreateIconIndirect(&ii);
        if (mask) DeleteObject(mask);
    }
    if (color) DeleteObject(color);
    ls_image_free(&img);
    return icon;
}

}  // namespace

// ---------------------------------------------------------------- 对外接口

void Start() {
    if (g_session) {
        if (g_session->phase != kFinishing) g_session->Stop();
        return;
    }
    if (capture::IsActive()) return;
    capture::Start(capture::Mode::Record);
}

bool CanStart() {
    if (g_session) return false;
    bool gif = settings::Str("record.format") == "gif";
    if (!gif && !mf::Available()) {
        toast::Show(T("record.encoderMissing"), "triangle-alert");
        return false;
    }
    std::wstring folder = settings::ExpandPath(settings::Str("record.folder"));
    if (!EnsureDir(folder)) {
        toast::Show(T("notify.folderMissing"), "triangle-alert");
        return false;
    }
    return true;
}

bool StartWithRegion(HMONITOR monitor, RECT regionPx) {
    if (g_session) return false;
    if (!CanStart()) return false;
    Session *s = new Session();
    g_session = s;
    if (!s->Begin(monitor, regionPx)) {
        g_session = nullptr;
        s->TeardownUi();
        if (s->pipe) s->pipe->Abort();
        delete s;
        return false;
    }
    tray::Refresh();
    return true;
}

void TogglePause() {
    if (g_session) g_session->TogglePause();
}

void TogglePen() {
    if (g_session) g_session->TogglePen();
}

void Stop() {
    if (g_session) g_session->Stop();
}

bool IsRecording() { return g_session && g_session->phase != kFinishing; }

bool IsPaused() { return g_session && g_session->phase == kRecording && g_session->paused; }

void Shutdown() {
    Session *s = g_session;
    if (!s) return;
    if (s->phase == kCountdown) {
        s->Cancel();
        return;
    }
    if (s->phase == kRecording) s->Stop();
    // 同步等写完（GIF 编码也在写入线程里）
    if (s->pipe) s->pipe->Join();
    if (s->progress) s->progress->Close();
    g_session = nullptr;
    s->TeardownUi();
    delete s;
}

HICON TrayIcon(int size) {
    if (!IsRecording()) return nullptr;
    Color c = IsPaused() ? theme::Ui().textTertiary : theme::Tok().recordBar.dot;
    return MakeDotIcon(size, c);
}

}  // namespace record
}  // namespace ls
