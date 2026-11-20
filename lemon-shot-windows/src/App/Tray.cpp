#include "App/Tray.h"

#include <shellapi.h>

#include <cmath>
#include <cstring>

#include "App/App.h"
#include "App/Hotkeys.h"
#include "Record/Record.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace tray {

namespace {

NOTIFYICONDATAW g_nid;
bool g_added = false;
bool g_v4 = false;
HICON g_icon = nullptr;
const UINT kTrayId = 1;

HICON MakeTrayIcon() {
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size <= 0) size = 16;
    return app::AppIcon(-size);  // 负数：托盘单色图标
}

void FillBase() {
    std::memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = app::MainHwnd();
    g_nid.uID = kTrayId;
}

}  // namespace

std::vector<MenuItem> Items() {
    std::vector<MenuItem> v;
    if (record::IsRecording()) {
        v.push_back({app::CMD_RECORD_STOP, "menu.stopRecording", nullptr});
        v.push_back({app::CMD_RECORD_PAUSE, record::IsPaused() ? "menu.resumeRecording" : "menu.pauseRecording", "recordPause"});
        v.push_back({0, nullptr, nullptr});
    }
    const MenuItem items[] = {
        {app::CMD_CAPTURE, "menu.capture", "capture"},
        {app::CMD_CAPTURE_WINDOW, "menu.captureWindow", "captureWindow"},
        {app::CMD_CAPTURE_FULLSCREEN, "menu.captureFullscreen", "captureFullscreen"},
        {app::CMD_CAPTURE_LAST_AREA, "menu.captureLastArea", "captureLastArea"},
        {app::CMD_CAPTURE_DELAYED, "menu.captureDelayed", "captureDelayed"},
        {app::CMD_CAPTURE_SHELL, "menu.captureShell", "captureShell"},
        {app::CMD_LONGSHOT, "menu.longshot", "longshot"},
        {0, nullptr, nullptr},
        {app::CMD_RECORD, "menu.record", "record"},
        {app::CMD_RECORD_AUDIO, "menu.recordAudio", "recordAudio"},
        {0, nullptr, nullptr},
        {app::CMD_OCR, "menu.ocr", "ocr"},
        {app::CMD_TRANSLATE, "menu.translate", "translate"},
        {app::CMD_COLOR_PICKER, "menu.colorPicker", "colorPicker"},
        {0, nullptr, nullptr},
        {app::CMD_PIN_CLIPBOARD, "menu.pinClipboard", "pinClipboard"},
        {app::CMD_PIN_LIBRARY, "menu.pinLibrary", "pinLibrary"},
        {app::CMD_TOGGLE_PINS, "menu.togglePins", "togglePins"},
        {app::CMD_CLOSE_PINS, "menu.closePins", nullptr},
        {0, nullptr, nullptr},
        {app::CMD_OPEN_FOLDER, "menu.openFolder", nullptr},
        {app::CMD_PREFS, "menu.preferences", nullptr},
        {app::CMD_ABOUT, "menu.about", nullptr},
        {0, nullptr, nullptr},
        {app::CMD_QUIT, "menu.quit", nullptr},
    };
    for (auto &i : items) v.push_back(i);
    return v;
}

void Create(HWND owner) {
    (void)owner;
    Refresh();
}

void Remove() {
    if (g_added) {
        FillBase();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_added = false;
    }
}

void Refresh() {
    bool show = settings::Bool("general.showStatusIcon");
    if (!show) {
        Remove();
        return;
    }
    HICON old = g_icon;
    g_icon = MakeTrayIcon();
    FillBase();
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_LS_TRAY;
    g_nid.hIcon = g_icon;
    std::wstring tip = T("app.name");
    std::wstring cap = hotkeys::Display(hotkeys::Global("capture"));
    if (!cap.empty()) tip += L"  " + cap;
    wcsncpy(g_nid.szTip, tip.c_str(), 127);
    if (!g_added) {
        g_added = Shell_NotifyIconW(NIM_ADD, &g_nid) != 0;
        if (g_added) {
            g_nid.uVersion = 4;  // NOTIFYICON_VERSION_4（Vista+）
            g_v4 = Shell_NotifyIconW(NIM_SETVERSION, &g_nid) != 0;
        }
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    if (old) DestroyIcon(old);
}

void OnTaskbarCreated() {
    g_added = false;
    Refresh();
}

bool HandleCallback(HWND owner, WPARAM, LPARAM lp) {
    UINT ev = g_v4 ? LOWORD(lp) : (UINT)lp;
    switch (ev) {
    case WM_LBUTTONUP: app::Execute(app::CMD_CAPTURE); return true;
    case WM_CONTEXTMENU: ShowMenu(owner); return true;
    case WM_RBUTTONUP:
        if (!g_v4) ShowMenu(owner);
        return true;
    }
    return false;
}

void ShowMenu(HWND owner) {
    HMENU m = CreatePopupMenu();
    for (auto &it : Items()) {
        if (!it.cmd) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        std::wstring text = T(it.key);
        if (it.shortcut) {
            std::wstring sc = hotkeys::Display(hotkeys::Global(it.shortcut));
            if (!sc.empty()) text += L"\t" + sc;
        }
        AppendMenuW(m, MF_STRING, (UINT_PTR)it.cmd, text.c_str());
    }
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(owner);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd) app::Execute(cmd);
}

void Balloon(const std::wstring &title, const std::wstring &text) {
    if (!g_added) return;
    FillBase();
    g_nid.uFlags = NIF_INFO;
    wcsncpy(g_nid.szInfoTitle, title.c_str(), 63);
    wcsncpy(g_nid.szInfo, text.c_str(), 255);
    g_nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------------------------------------------------------------- 自测：自绘菜单（与 Mac 端 MenuRenderWidget 同尺寸）

static const float kRowH = 22, kSepH = 11, kPadY = 5, kMenuW = 300;

RectF MenuPreviewSize() {
    float h = kPadY * 2;
    for (auto &it : Items()) h += it.cmd ? kRowH : kSepH;
    return RectF(0, 0, kMenuW, h);
}

void DrawMenuPreview(Canvas &c, RectF r) {
    const auto &u = theme::Ui();
    float fs = theme::Tok().fontSize.body;
    c.Shadow(r, 8, 16, 4, u.shadow);
    c.FillRound(r, 8, u.popoverBg);
    c.StrokeRound(r.Inflate(-0.5f), 7.5f, u.border, 1);
    float y = r.y + kPadY;
    int i = 0;
    for (auto &it : Items()) {
        if (!it.cmd) {
            c.FillRect(RectF(r.x + 10, y + kSepH / 2, r.w - 20, 1), u.divider);
            y += kSepH;
            ++i;
            continue;
        }
        RectF row(r.x + 5, y, r.w - 10, kRowH);
        bool hot = i == 0;
        if (hot) c.FillRound(row, 4, u.accent);
        TextStyle ts;
        ts.size = fs;
        ts.color = hot ? u.accentText : u.text;
        c.Text(T(it.key), RectF(row.x + 10, row.y, row.w - 90, row.h), ts);
        if (it.shortcut) {
            std::wstring sc = hotkeys::Display(hotkeys::Global(it.shortcut));
            ts.color = hot ? u.accentText : u.textSecondary;
            ts.h = HAlign::Right;
            c.Text(sc, RectF(row.r() - 90, row.y, 80, row.h), ts);
        }
        y += kRowH;
        ++i;
    }
}

}  // namespace tray
}  // namespace ls
