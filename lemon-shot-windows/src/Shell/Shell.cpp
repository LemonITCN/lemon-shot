// 带壳截图（spec 7）：预览窗口布局与 Mac 端 Shell/ShellWindow.swift 一致；Windows 用原生标题栏，客户区从内容开始。
#include "Shell/Shell.h"

#include <windows.h>

#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

#include "App/App.h"
#include "Capture/Capture.h"
#include "Capture/Screen.h"
#include "OCR/ToolUI.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "Shell/ShellRender.h"
#include "UI/Toast.h"
#include "Util/Api.h"
#include "Util/ImageIO.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace shell {

namespace {

const float kWinW = 920, kWinH = 600, kMinW = 760, kMinH = 520, kSidebarW = 240;
const char *kBackgrounds[] = {"transparent", "white", "black"};

const UiColors &U() { return theme::Ui(); }

std::wstring ModelTitle(int m) { return T((std::string("shell.device.") + ModelKey(m)).c_str()); }

// 机型列表：一行一个，选中行 surfaceSelected 底；「自动匹配」行右侧显示匹配到的机型
class ModelList : public prefs::Widget {
public:
    std::string selected;
    int autoModel;
    int hover = -1;
    std::function<void(const std::string &)> onChange;
    static constexpr float kRowH = 28;

    ModelList(const std::string &sel, int autoM) : selected(sel), autoModel(autoM) {}

    static std::vector<std::string> Values() {
        std::vector<std::string> v = {"auto"};
        for (int i = 0; i < kModelCount; ++i) v.push_back(ModelKey(i));
        return v;
    }
    float ContentHeight() const { return (float)(kModelCount + 1) * kRowH; }
    RectF Row(int i) const { return RectF(frame.x, frame.y + i * kRowH, frame.w, kRowH); }
    int HitRow(float x, float y) const {
        for (int i = 0; i <= kModelCount; ++i)
            if (Row(i).Contains(x, y)) return i;
        return -1;
    }

    bool Interactive() override { return true; }

    void Draw(Canvas &c) override {
        const auto &u = U();
        float body = theme::Tok().fontSize.body;
        auto values = Values();
        for (int i = 0; i < (int)values.size(); ++i) {
            RectF r = Row(i).Inflate(0, -1);
            bool sel = values[(size_t)i] == selected;
            if (sel)
                c.FillRound(r, 6, u.surfaceSelected);
            else if (i == hover)
                c.FillRound(r, 6, u.surfaceHover);
            bool isAuto = i == 0;
            int m = isAuto ? -1 : i - 1;
            c.Icon(isAuto ? "sparkles" : ModelIcon(m), r.x + 16, r.cy(), 16, sel ? u.iconActive : u.icon, 1.6f);
            std::wstring title = isAuto ? T("shell.auto") : ModelTitle(m);
            float tx = r.x + 32;
            int weight = sel ? kMedium : kRegular;
            float tw = gfx::Measure(title, body, weight).w;
            TextStyle ts;
            ts.size = body;
            ts.weight = weight;
            ts.color = u.text;
            ts.ellipsis = true;
            c.Text(title, RectF(tx, r.y, r.r() - 8 - tx, r.h), ts);
            if (isAuto) {
                TextStyle cs;
                cs.size = theme::Tok().fontSize.caption;
                cs.color = u.textTertiary;
                cs.h = HAlign::Right;
                cs.ellipsis = true;
                float sx = tx + tw + 6;
                if (r.r() - 8 > sx) c.Text(ModelTitle(autoModel), RectF(sx, r.y + 1, r.r() - 8 - sx, r.h), cs);
            }
        }
    }

    void OnMouseMove(float x, float y) override {
        int h = HitRow(x, y);
        if (h != hover) {
            hover = h;
            Redraw();
        }
    }
    void OnMouseLeave() override {
        hover = -1;
        Redraw();
    }
    void OnMouseUp(float x, float y, bool inside) override {
        if (!inside) return;
        int i = HitRow(x, y);
        auto values = Values();
        if (i < 0 || i >= (int)values.size() || values[(size_t)i] == selected) return;
        selected = values[(size_t)i];
        Redraw();
        if (onChange) onChange(selected);
    }
};

int BackgroundIndex(const std::string &v) {
    for (int i = 0; i < 3; ++i)
        if (v == kBackgrounds[i]) return i;
    return 0;
}

std::string SettingModel() {
    std::string m = settings::Str("shell.model");
    if (m != "auto" && ModelFromKey(m) < 0) m = "auto";
    return m;
}

// 带壳预览窗口内容（屏幕与自测共用）
struct Content {
    tool::ImageBox *preview;
    prefs::Label *modelCaption, *bgCaption;
    ModelList *list;
    prefs::SegmentedW *background;
    prefs::ButtonW *copy, *save;
    RectF sidebar;

    Content(prefs::Widget &root, int autoModel) {
        const auto &fs = theme::Tok().fontSize;
        preview = (tool::ImageBox *)root.Add(new tool::ImageBox());
        preview->checker = true;
        preview->border = false;
        preview->radius = 0;
        modelCaption = (prefs::Label *)root.Add(new prefs::Label(T("shell.model"), fs.caption, "textSecondary", kMedium));
        list = (ModelList *)root.Add(new ModelList(SettingModel(), autoModel));
        bgCaption = (prefs::Label *)root.Add(new prefs::Label(T("shell.background"), fs.caption, "textSecondary", kMedium));
        background = (prefs::SegmentedW *)root.Add(new prefs::SegmentedW(
            {T("shell.transparent"), T("shell.white"), T("shell.black")}, BackgroundIndex(settings::Str("shell.background")), nullptr));
        save = (prefs::ButtonW *)root.Add(new prefs::ButtonW(T("action.save")));
        copy = (prefs::ButtonW *)root.Add(new prefs::ButtonW(T("action.copy"), prefs::ButtonW::Primary));
    }

    void Layout(float w, float h) {
        sidebar = RectF(w - kSidebarW, 0, kSidebarW, h);
        preview->frame = RectF(24, 24, sidebar.x - 48, h - 48);
        float x = sidebar.x + 16, wd = sidebar.w - 32;
        float y = sidebar.y + 16;
        modelCaption->frame = RectF(x + 2, y, wd, 20);
        y += 22;
        list->frame = RectF(x, y, wd, list->ContentHeight());
        y = list->frame.b() + 18;
        bgCaption->frame = RectF(x + 2, y, wd, 20);
        y += 24;
        float bh = theme::Tok().window.controlHeight;
        background->frame = RectF(x, y, wd, bh);
        float bw = std::floor((wd - 8) / 2);
        save->frame = RectF(x, h - 16 - bh, bw, bh);
        copy->frame = RectF(x + bw + 8, h - 16 - bh, wd - bw - 8, bh);
    }

    void DrawChrome(Canvas &c) {
        c.FillRect(sidebar, U().surface);
        c.FillRect(RectF(sidebar.x, sidebar.y, 1, sidebar.h), U().divider);
    }

    int Model() const {
        if (list->selected == "auto") return list->autoModel;
        int m = ModelFromKey(list->selected);
        return m >= 0 ? m : list->autoModel;
    }
    std::string Background() const { return kBackgrounds[Clamp(background->selected, 0, 2)]; }
};

// ---------------------------------------------------------------- 窗口

class ShellWindow;
ShellWindow *g_win = nullptr;

class ShellWindow : public tool::ToolWindow {
public:
    Content *ui = nullptr;
    ls_image shot = {};
    ls_image result = {};
    float scale = 1;
    int gen = 0;

    ~ShellWindow() override {
        delete ui;
        ls_image_free(&shot);
        ls_image_free(&result);
    }

    bool Create(ls_image img, float sc, int autoModel, HMONITOR mon) {
        shot = img;
        scale = sc;
        ui = new Content(root_, autoModel);
        ui->list->onChange = [this](const std::string &v) {
            settings::SetStr("shell.model", v);
            Rerender();
        };
        ui->background->onChange = [this](int i) {
            settings::SetStr("shell.background", kBackgrounds[Clamp(i, 0, 2)]);
            Rerender();
        };
        ui->copy->action = [this]() {
            if (result.data) tool::CopyImage(result, scale);
        };
        ui->save->action = [this]() {
            if (result.data) tool::SaveImage(result, scale);
        };
        UpdateButtons();
        return Open(T("shell.title"), kWinW, kWinH, kMinW, kMinH, true, mon);
    }

    void UpdateButtons() {
        ui->copy->enabled = result.data != nullptr;
        ui->save->enabled = result.data != nullptr;
    }

    void Layout(float w, float h) override { ui->Layout(w, h); }

    void DrawContent(Canvas &c, float w, float h) override {
        c.Clear(U().windowBg);
        ui->DrawChrome(c);
        root_.Draw(c);
    }

    void OnClosed() override {
        ++gen;
        if (g_win == this) g_win = nullptr;
    }

    // 换机型或背景时在后台重新合成
    void Rerender() {
        int my = ++gen;
        int model = ui->Model();
        std::string bg = ui->Background();
        ls_image copyImg = CloneImage(shot);
        if (!copyImg.data) return;
        tool::InitDispatcher();
        bool started = tool::RunDetached([copyImg, model, bg, my]() mutable {
            ls_image *out = new ls_image();
            if (!shell::Render(copyImg, model, bg, *out)) *out = ls_image{};
            ls_image_free(&copyImg);
            tool::PostToUi([out, my]() {
                ShellWindow *w = g_win;
                if (w && w->IsOpen() && w->gen == my) {
                    ls_image_free(&w->result);
                    w->result = *out;
                    w->ui->preview->SetImage(w->result.data ? &w->result : nullptr, 2400, 2400);
                    w->UpdateButtons();
                    w->Invalidate();
                } else {
                    ls_image_free(out);
                }
                delete out;
            });
        });
        if (!started) ls_image_free(&copyImg);
    }
};

// 被截的屏是不是内置屏（笔记本屏幕）：QueryDisplayConfig 的输出接口类型（Win7 起都有）
bool IsInternalDisplay(const wchar_t *device) {
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || np == 0) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm ? nm : 1);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return false;
    for (UINT32 i = 0; i < np; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
        ZeroMemory(&src, sizeof src);
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, device) != 0) continue;
        DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t = paths[i].targetInfo.outputTechnology;
        if (t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
            t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED)
            return true;
    }
    return false;
}

bool g_pending = false;

void DoCapture() {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi;
    ZeroMemory(&mi, sizeof mi);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, (MONITORINFO *)&mi);
    ls_image img;
    if (!CaptureScreenRect(mi.rcMonitor, settings::Bool("capture.captureCursor"), img)) {
        if (g_win && g_win->IsOpen()) ShowWindow(g_win->Hwnd(), SW_SHOW);
        return;
    }
    float scale = api::MonitorDpi(mon) / 96.f;
    int autoModel = IsInternalDisplay(mi.szDevice) ? kLaptop : kMonitor;
    if (settings::Bool("general.playSound")) app::PlayShutter();
    if (g_win && g_win->IsOpen()) g_win->Close();
    ShellWindow *w = new ShellWindow();
    if (!w->Create(img, scale, autoModel, mon)) {
        delete w;  // 析构释放 img
        return;
    }
    g_win = w;
    w->Present(true);
    w->Rerender();
}

void CALLBACK CaptureTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    g_pending = false;
    DoCapture();
}

}  // namespace

void Capture() {
    if (capture::IsActive() || g_pending) return;
    // 预览窗口开着时先收起，免得截进去
    if (g_win && g_win->IsOpen()) ShowWindow(g_win->Hwnd(), SW_HIDE);
    toast::HideNow();
    double delay = settings::Num("shell.delay");
    if (!(delay > 0)) delay = 0;
    if (delay > 60) delay = 60;
    g_pending = true;
    // 留出一点时间让窗口真正从屏幕上消失
    if (!SetTimer(nullptr, 0, (UINT)(delay * 1000) + 60, &CaptureTimer)) {
        g_pending = false;
        DoCapture();
    }
}

SizeF SelfTestSize() { return SizeF{kWinW, kWinH}; }

ls_image RenderSelfTest(float scale, const ls_image &shotImg) {
    prefs::Widget root;
    Content ui(root, kLaptop);
    ui.list->selected = "auto";
    ui.background->selected = 0;
    root.frame = RectF(0, 0, kWinW, kWinH);
    ui.Layout(kWinW, kWinH);
    ls_image framed;
    if (Render(shotImg, kLaptop, "transparent", framed)) {
        ui.preview->SetImage(&framed, 4000, 4000);
        ls_image_free(&framed);
    }
    return tool::RenderOffscreen(kWinW, kWinH, scale, [&](Canvas &c) {
        c.FillRect(RectF(0, 0, kWinW, kWinH), U().windowBg);
        ui.DrawChrome(c);
        root.Draw(c);
    });
}

}  // namespace shell
}  // namespace ls
