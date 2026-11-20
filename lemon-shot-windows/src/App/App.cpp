#include "App/App.h"

#include <mmsystem.h>
#include <shellapi.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "App/Hotkeys.h"
#include "App/Tray.h"
#include "Audio/Audio.h"
#include "Capture/Capture.h"
#include "Capture/Export.h"
#include "LongShot/LongShot.h"
#include "OCR/OCR.h"
#include "Pin/Pins.h"
#include "Prefs/Prefs.h"
#include "Record/Record.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "Settings/Theme.h"
#include "Shell/Shell.h"
#include "Translate/Translate.h"
#include "UI/Gfx.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/ImageIO.h"
#include "Util/Resources.h"
#include "../../res/resource.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace app {

namespace {

const wchar_t kMainTitle[] = L"cn.lemonit.shot.main";
enum { TIMER_SAVE = 1 };

HINSTANCE g_inst = nullptr;
UINT g_taskbarCreated = 0;

struct Deferred {
    void (*fn)(void *);
    void *ctx;
};
std::vector<Deferred> g_deferred;

class MainWindow : public Window {
protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;
};

MainWindow *g_main = nullptr;

void ScheduleSave() {
    if (g_main && g_main->Hwnd()) SetTimer(g_main->Hwnd(), TIMER_SAVE, 300, nullptr);
}

void ApplyLaunchAtLogin() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS)
        return;
    if (settings::Bool("general.launchAtLogin")) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(key, L"LemonShot", 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"LemonShot");
    }
    RegCloseKey(key);
}

void ReportHotkeyFailures() {
    std::vector<std::string> failed = hotkeys::FailedIds();
    if (failed.empty()) return;
    std::wstring keys;
    for (size_t i = 0; i < failed.size(); ++i) {
        if (i) keys += i18n::IsChinese() ? L"、" : L", ";
        keys += hotkeys::Display(hotkeys::Global(failed[i]));
    }
    tray::Balloon(T("app.name"), i18n::T("notify.hotkeyFailed", {{"key", keys}}));
}

void OnSettingsChanged(void *, const std::string &path) {
    bool all = path.empty();
    auto is = [&](const char *prefix) { return all || StartsWith(path, prefix); };
    if (is("general.language")) OnLanguageChanged();
    if (is("general.theme")) {
        theme::Refresh();
        prefs::Refresh();
    }
    if (is("general.showStatusIcon")) tray::Refresh();
    if (is("general.launchAtLogin")) ApplyLaunchAtLogin();
    if (is("shortcuts")) {
        if (!prefs::IsRecordingShortcut() && g_main) {
            hotkeys::RegisterAll(g_main->Hwnd());
            tray::Refresh();
        }
    }
    if (is("capture") || is("color")) capture::OnSettingsChanged();
    if (is("pin")) pins::OnSettingsChanged();
    prefs::OnSettingChanged(path);
}

LRESULT MainWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        tray::OnTaskbarCreated();
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY: {
        std::string id = hotkeys::IdForHotkey((int)wp);
        int cmd = CommandForShortcut(id);
        if (cmd) Execute(cmd);
        return 0;
    }
    case WM_LS_TRAY:
        if (tray::HandleCallback(hwnd_, wp, lp)) return 0;
        break;
    case WM_LS_SHOW_PREFS: prefs::Show(nullptr); return 0;
    case WM_LS_DEFERRED: {
        std::vector<Deferred> list;
        list.swap(g_deferred);
        for (auto &d : list) d.fn(d.ctx);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_SAVE) {
            KillTimer(hwnd_, TIMER_SAVE);
            settings::SaveNow();
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
            if (theme::Refresh()) prefs::Refresh();
            tray::Refresh();
        }
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            record::Shutdown();  // 注销/关机：录制中的文件写完再退出
            audio::Shutdown();
            settings::SaveNow();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return Window::Handle(msg, wp, lp);
}

// 快门声：内存里合成两下短促的噪声咔嗒（Windows 没有系统截图声）
std::string g_shutter;

void BuildShutter() {
    const int rate = 22050;
    const int n = rate * 140 / 1000;
    std::vector<int16_t> pcm((size_t)n);
    uint32_t seed = 0x1234567u;
    for (int i = 0; i < n; ++i) {
        double t = (double)i / rate;
        seed = seed * 1664525u + 1013904223u;
        double noise = ((double)(seed >> 9) / (double)(1u << 23)) * 2.0 - 1.0;
        double env = 0;
        if (t < 0.035) env += std::exp(-t * 110.0);
        if (t >= 0.062 && t < 0.13) env += 0.75 * std::exp(-(t - 0.062) * 85.0);
        double v = noise * env * 0.32;
        pcm[(size_t)i] = (int16_t)(v * 32767.0);
    }
    uint32_t dataBytes = (uint32_t)n * 2;
    g_shutter.clear();
    auto u32 = [](std::string &s, uint32_t v) { s.append((const char *)&v, 4); };
    auto u16 = [](std::string &s, uint16_t v) { s.append((const char *)&v, 2); };
    g_shutter += "RIFF";
    u32(g_shutter, 36 + dataBytes);
    g_shutter += "WAVEfmt ";
    u32(g_shutter, 16);
    u16(g_shutter, 1);
    u16(g_shutter, 1);
    u32(g_shutter, rate);
    u32(g_shutter, rate * 2);
    u16(g_shutter, 2);
    u16(g_shutter, 16);
    g_shutter += "data";
    u32(g_shutter, dataBytes);
    g_shutter.append((const char *)pcm.data(), dataBytes);
}

}  // namespace

HINSTANCE Instance() { return g_inst; }
HWND MainHwnd() { return g_main ? g_main->Hwnd() : nullptr; }

void Defer(void (*fn)(void *), void *ctx) {
    g_deferred.push_back({fn, ctx});
    if (g_main && g_main->Hwnd())
        PostMessageW(g_main->Hwnd(), WM_LS_DEFERRED, 0, 0);
}

int CommandForShortcut(const std::string &id) {
    struct Map {
        const char *id;
        int cmd;
    } map[] = {
        {"capture", CMD_CAPTURE},
        {"record", CMD_RECORD},
        {"ocr", CMD_OCR},
        {"translate", CMD_TRANSLATE},
        {"pinClipboard", CMD_PIN_CLIPBOARD},
        {"pinLibrary", CMD_PIN_LIBRARY},
        {"togglePins", CMD_TOGGLE_PINS},
        {"captureLastArea", CMD_CAPTURE_LAST_AREA},
        {"captureWindow", CMD_CAPTURE_WINDOW},
        {"captureFullscreen", CMD_CAPTURE_FULLSCREEN},
        {"capturePreset", CMD_CAPTURE_PRESET},
        {"captureDelayed", CMD_CAPTURE_DELAYED},
        {"captureShell", CMD_CAPTURE_SHELL},
        {"longshot", CMD_LONGSHOT},
        {"colorPicker", CMD_COLOR_PICKER},
        {"recordAudio", CMD_RECORD_AUDIO},
        {"recordPause", CMD_RECORD_PAUSE},
        {"recordPen", CMD_RECORD_PEN},
    };
    for (auto &m : map)
        if (id == m.id) return m.cmd;
    return CMD_NONE;
}

void Execute(int cmd) {
    switch (cmd) {
    case CMD_CAPTURE: capture::Start(capture::Mode::Normal); break;
    case CMD_CAPTURE_WINDOW: capture::CaptureWindowUnderCursor(); break;
    case CMD_CAPTURE_FULLSCREEN: capture::CaptureFullscreen(); break;
    case CMD_CAPTURE_LAST_AREA: capture::Start(capture::Mode::LastArea); break;
    case CMD_CAPTURE_PRESET: capture::Start(capture::Mode::Preset); break;
    case CMD_CAPTURE_DELAYED: capture::StartDelayed(); break;
    case CMD_CAPTURE_SHELL: shell::Capture(); break;
    case CMD_LONGSHOT: longshot::Start(); break;
    case CMD_RECORD: record::Start(); break;
    case CMD_RECORD_AUDIO: audio::Toggle(); break;
    case CMD_RECORD_PAUSE: record::TogglePause(); break;
    case CMD_RECORD_PEN: record::TogglePen(); break;
    case CMD_RECORD_STOP: record::Stop(); break;
    case CMD_OCR: ocr::Start(); break;
    case CMD_TRANSLATE: translate::Start(); break;
    case CMD_COLOR_PICKER: capture::Start(capture::Mode::ColorPicker); break;
    case CMD_PIN_CLIPBOARD: pins::PinClipboard(); break;
    case CMD_PIN_LIBRARY: pins::ShowLibrary(); break;
    case CMD_TOGGLE_PINS: pins::ToggleAll(); break;
    case CMD_CLOSE_PINS: pins::CloseAll(); break;
    case CMD_OPEN_FOLDER: {
        std::wstring dir = exporter::SaveFolder();
        EnsureDir(dir);
        ShellOpen(dir);
        break;
    }
    case CMD_PREFS: prefs::Show(nullptr); break;
    case CMD_ABOUT: prefs::Show("about"); break;
    case CMD_QUIT: Quit(); break;
    }
}

void Quit() {
    record::Shutdown();  // 录制中：停止并写完文件
    audio::Shutdown();
    capture::Cancel();
    pins::CloseAll();
    prefs::Close();
    if (g_main) DestroyWindow(g_main->Hwnd());
}

void OnLanguageChanged() {
    i18n::Reload();
    tray::Refresh();
    prefs::Refresh();
}

void PlayShutter() {
    if (g_shutter.empty()) BuildShutter();
    if (!PlaySoundW((LPCWSTR)g_shutter.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT))
        PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}

namespace {

// 预乘 BGRA 位图 → HICON（图标位图要直通 alpha）
HICON IconFromImage(const ls_image &img) {
    const int w = img.width, h = img.height;
    BITMAPV5HEADER bi;
    std::memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = w;
    bi.bV5Height = -h;
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
        for (int y = 0; y < h; ++y) {
            const uint8_t *src = img.data + (size_t)y * img.stride;
            for (int x = 0; x < w; ++x, src += 4, d += 4) {
                uint8_t a = src[3];
                d[0] = a ? (uint8_t)((src[0] * 255 + a / 2) / a) : 0;
                d[1] = a ? (uint8_t)((src[1] * 255 + a / 2) / a) : 0;
                d[2] = a ? (uint8_t)((src[2] * 255 + a / 2) / a) : 0;
                d[3] = a;
            }
        }
        HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);
        ICONINFO ii;
        std::memset(&ii, 0, sizeof ii);
        ii.fIcon = TRUE;
        ii.hbmMask = mask;
        ii.hbmColor = color;
        icon = CreateIconIndirect(&ii);
        if (mask) DeleteObject(mask);
    }
    if (color) DeleteObject(color);
    return icon;
}

// 选不小于 size 的最小一张单色剪影（没有就取最大的）
int GlyphResource(int size) {
    static const int sizes[] = {16, 18, 20, 24, 32, 36, 40, 48};
    static const int ids[] = {IDR_GLYPH_16, IDR_GLYPH_18, IDR_GLYPH_20, IDR_GLYPH_24,
                              IDR_GLYPH_32, IDR_GLYPH_36, IDR_GLYPH_40, IDR_GLYPH_48};
    for (int i = 0; i < 8; ++i)
        if (sizes[i] >= size) return ids[i];
    return IDR_GLYPH_48;
}

}  // namespace

HICON AppIcon(int size) {
    bool tray = size < 0;
    if (tray) size = -size;
    if (size <= 0) size = 16;
    if (!tray) {
        // 应用图标：exe 里的多尺寸 .ico，系统挑最接近的一张（16–48 是简化版，64 以上是完整版）
        return (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);
    }
    // 托盘：剪刀单色剪影，按任务栏深浅着成白色或近黑色（与 Mac 菜单栏模板图同一形状）
    std::string png = LoadResourceData(GlyphResource(size));
    ls_image src;
    std::memset(&src, 0, sizeof src);
    if (png.empty() || !DecodeImage(png.data(), png.size(), src)) return nullptr;
    ls_image img = src;
    if (src.width != size || src.height != size) {
        if (ls_image_resize(&src, size, size, &img) != 0) {
            ls_image_free(&src);
            return nullptr;
        }
        ls_image_free(&src);
    }
    const bool dark = theme::SystemTaskbarDark();
    const uint8_t r = dark ? 255 : 28, g = dark ? 255 : 28, b = dark ? 255 : 31;
    for (int y = 0; y < img.height; ++y) {
        uint8_t *p = img.data + (size_t)y * img.stride;
        for (int x = 0; x < img.width; ++x, p += 4) {
            const unsigned a = p[3];
            p[0] = (uint8_t)((b * a + 127) / 255);
            p[1] = (uint8_t)((g * a + 127) / 255);
            p[2] = (uint8_t)((r * a + 127) / 255);
        }
    }
    HICON icon = IconFromImage(img);
    ls_image_free(&img);
    return icon;
}

int Run(HINSTANCE inst) {
    g_inst = inst;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\cn.lemonit.shot");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已在运行：通知旧实例打开偏好设置
        HWND other = FindWindowW(kWindowClass, kMainTitle);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            if (pid) AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_LS_SHOW_PREFS, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }
    g_main = new MainWindow();
    if (!g_main->CreateWin(WS_EX_TOOLWINDOW, WS_POPUP, 0, 0, 0, 0, nullptr, kMainTitle)) {
        LS_LOG("app: 主窗口创建失败 %lu", GetLastError());
        return 1;
    }
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    settings::SetSaveScheduler(&ScheduleSave);
    settings::AddListener(&OnSettingsChanged, nullptr);
    tray::Create(g_main->Hwnd());
    hotkeys::RegisterAll(g_main->Hwnd());
    ReportHotkeyFailures();
    ApplyLaunchAtLogin();
    LS_LOG("app: 启动完成，Windows %lu.%lu.%lu，系统 DPI %u", api::OsMajor(), api::OsMinor(), api::OsBuild(), api::SystemDpi());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    hotkeys::UnregisterAll(nullptr);
    tray::Remove();
    settings::SaveNow();
    settings::SaveState();
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}

}  // namespace app
}  // namespace ls
