// 文字识别（spec 4）。结果窗口布局与 Mac 端 OCR/OCRWindow.swift 一致；Windows 用原生标题栏，所以客户区从工具行开始。
#include "OCR/OCR.h"

#include <cmath>
#include <vector>

#include "Capture/Capture.h"
#include "Capture/Screen.h"
#include "OCR/Engine.h"
#include "OCR/ToolUI.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "Translate/Translate.h"
#include "Util/Api.h"
#include "Util/Json.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace ocr {

namespace {

const float kWinW = 520, kWinH = 420, kMinW = 520, kMinH = 360, kThumbW = 120;

// 结果窗口的部件树（屏幕与自测共用）
struct Content {
    tool::ToggleChip *joinLines, *ascii, *continuous, *fixedRegion;
    tool::IconButton *keepOnTop;
    tool::ImageBox *thumb;
    tool::TextArea *area;
    tool::IconButton *thumbToggle;
    tool::Status *status;
    prefs::ButtonW *openLink, *translate, *copyClose, *copy;
    bool thumbVisible = true;
    RectF footer;

    explicit Content(prefs::Widget &root) {
        joinLines = (tool::ToggleChip *)root.Add(new tool::ToggleChip(T("ocr.joinLines"), settings::Bool("ocr.joinLines"), nullptr));
        ascii = (tool::ToggleChip *)root.Add(
            new tool::ToggleChip(T("ocr.forceAscii"), settings::Bool("ocr.forceAsciiPunctuation"), nullptr));
        continuous =
            (tool::ToggleChip *)root.Add(new tool::ToggleChip(T("ocr.continuous"), settings::Bool("ocr.continuous"), nullptr));
        fixedRegion =
            (tool::ToggleChip *)root.Add(new tool::ToggleChip(T("ocr.fixedRegion"), settings::Bool("ocr.fixedRegion"), nullptr));
        keepOnTop = (tool::IconButton *)root.Add(new tool::IconButton("pin", T("ocr.keepOnTop"), nullptr));
        keepOnTop->selected = settings::Bool("ocr.keepOnTop");
        thumb = (tool::ImageBox *)root.Add(new tool::ImageBox());
        thumb->topAligned = true;
        area = (tool::TextArea *)root.Add(new tool::TextArea());
        thumbToggle = (tool::IconButton *)root.Add(new tool::IconButton("image", T("ocr.thumbnail"), nullptr));
        thumbToggle->selected = true;
        status = (tool::Status *)root.Add(new tool::Status());
        openLink = (prefs::ButtonW *)root.Add(new prefs::ButtonW(T("ocr.openLink"), prefs::ButtonW::Normal, "external-link"));
        openLink->hidden = true;
        translate = (prefs::ButtonW *)root.Add(new prefs::ButtonW(T("ocr.translate")));
        copyClose = (prefs::ButtonW *)root.Add(new prefs::ButtonW(T("ocr.copyClose")));
        copy = (prefs::ButtonW *)root.Add(new prefs::ButtonW(T("ocr.copy"), prefs::ButtonW::Primary));
    }

    void Layout(float w, float h) {
        float x = 16;
        for (tool::ToggleChip *c : {joinLines, ascii, continuous, fixedRegion}) {
            SizeF s = c->Preferred();
            c->frame = RectF(x, std::floor((tool::kToolRowH - s.h) / 2), s.w, s.h);
            x += s.w + 6;
        }
        keepOnTop->frame = RectF(w - 12 - 28, (tool::kToolRowH - 28) / 2, 28, 28);
        float top = tool::kToolRowH;
        float bottom = h - tool::kFooterH;
        footer = RectF(0, bottom, w, tool::kFooterH);
        bool hasThumb = thumb->HasImage();
        bool showThumb = thumbVisible && hasThumb;
        thumb->hidden = !showThumb;
        float textX = 16;
        if (showThumb) {
            thumb->frame = RectF(16, top, kThumbW, bottom - 12 - top);
            textX = thumb->frame.r() + 12;
        }
        area->frame = RectF(textX, top, w - 16 - textX, bottom - 12 - top);
        thumbToggle->hidden = !hasThumb;
        thumbToggle->selected = thumbVisible;
        float left = tool::LayoutRight(footer, {copy, copyClose, translate});
        float lx = 12;
        if (hasThumb) {
            thumbToggle->frame = RectF(lx, footer.cy() - 14, 28, 28);
            lx += 28 + 8;
        } else {
            lx += 4;
        }
        if (!openLink->hidden) {
            SizeF s = openLink->Preferred();
            float sw = std::fmin(status->Preferred().w, std::fmax(0.f, left - 12 - lx - s.w - 8));
            status->frame = RectF(lx, footer.cy() - 10, sw, 20);
            openLink->frame = RectF(status->frame.r() + 8, std::floor(footer.cy() - s.h / 2), s.w, s.h);
        } else {
            status->frame = RectF(lx, footer.cy() - 10, std::fmax(0.f, left - 12 - lx), 20);
        }
    }

    void DrawChrome(Canvas &c) { c.FillRect(RectF(footer.x, footer.y, footer.w, 1), theme::Ui().divider); }
};

// ---------------------------------------------------------------- 固定区域（state.json 的 ocrRegion，屏幕像素）

void SaveRegion(const RECT &r) {
    if (settings::IsSelfTest()) return;
    Json arr = Json::MakeArray();
    arr.Push(Json::MakeNumber(r.left));
    arr.Push(Json::MakeNumber(r.top));
    arr.Push(Json::MakeNumber(r.right - r.left));
    arr.Push(Json::MakeNumber(r.bottom - r.top));
    settings::State().Set("ocrRegion", arr);
    settings::SaveState();
}

bool LoadRegion(RECT &r) {
    const Json *a = settings::State().Find("ocrRegion");
    if (!a || !a->IsArray() || a->Size() != 4) return false;
    int x = (int)a->At(0).AsNumber(), y = (int)a->At(1).AsNumber(), w = (int)a->At(2).AsNumber(), h = (int)a->At(3).AsNumber();
    if (w <= 0 || h <= 0) return false;
    r = RECT{x, y, x + w, y + h};
    return true;
}

// ---------------------------------------------------------------- 结果窗口

class OcrWindow;
OcrWindow *g_ocrWin = nullptr;  // 当前结果窗口（窗口关闭时清空）

class OcrWindow : public tool::ToolWindow {
public:
    Content *ui = nullptr;
    HWND edit = nullptr;
    std::vector<Output> blocks;
    int gen = 0;
    std::string link;

    bool Create() {
        ui = new Content(root_);
        if (!Open(T("ocr.title"), kWinW, kWinH, kMinW, kMinH, true)) return false;
        edit = CreateTextEdit(false);
        Bind();
        settings::AddListener(&OcrWindow::SettingsChanged, this);
        SetTopmost(settings::Bool("ocr.keepOnTop"));
        return true;
    }

    void Bind() {
        ui->joinLines->onChange = [](bool v) { settings::SetBool("ocr.joinLines", v); };
        ui->ascii->onChange = [](bool v) { settings::SetBool("ocr.forceAsciiPunctuation", v); };
        ui->continuous->onChange = [](bool v) { settings::SetBool("ocr.continuous", v); };
        ui->fixedRegion->onChange = [](bool v) { settings::SetBool("ocr.fixedRegion", v); };
        ui->keepOnTop->action = []() { settings::SetBool("ocr.keepOnTop", !settings::Bool("ocr.keepOnTop")); };
        ui->thumbToggle->action = [this]() {
            ui->thumbVisible = !ui->thumbVisible;
            Relayout();
        };
        ui->openLink->action = [this]() {
            if (!link.empty()) ShellOpen(Widen(link));
        };
        ui->copy->action = [this]() { CopyResult(settings::Bool("ocr.closeAfterCopy")); };
        ui->copyClose->action = [this]() { CopyResult(true); };
        ui->translate->action = [this]() {
            std::wstring t = EditText(edit);
            if (!Trim(t).empty()) translate::FromText(t);
        };
        ui->area->onActivate = [this]() {
            ui->area->overlay.clear();
            Relayout();
            if (edit) SetFocus(edit);
        };
    }

    static void SettingsChanged(void *ctx, const std::string &path) {
        OcrWindow *w = (OcrWindow *)ctx;
        if (!w->hwnd_ || !(path.empty() || StartsWith(path, "ocr"))) return;
        Content *u = w->ui;
        u->joinLines->on = settings::Bool("ocr.joinLines");
        u->ascii->on = settings::Bool("ocr.forceAsciiPunctuation");
        u->continuous->on = settings::Bool("ocr.continuous");
        u->fixedRegion->on = settings::Bool("ocr.fixedRegion");
        u->keepOnTop->selected = settings::Bool("ocr.keepOnTop");
        w->SetTopmost(u->keepOnTop->selected);
        if (path.empty() || path == "ocr.joinLines" || path == "ocr.forceAsciiPunctuation" || path == "ocr.preferQRCode")
            w->RefreshText();
        w->Relayout();
    }

    void Layout(float w, float h) override {
        ui->Layout(w, h);
        PlaceEdit(edit, ui->area->frame, ui->area->overlay.empty());
    }

    void DrawContent(Canvas &c, float w, float h) override {
        c.Clear(theme::Ui().windowBg);
        ui->DrawChrome(c);
        root_.Draw(c);
    }

    void OnClosed() override {
        settings::RemoveListener(&OcrWindow::SettingsChanged, this);
        ++gen;
        if (g_ocrWin == this) g_ocrWin = nullptr;
    }

    void SetStatus(const std::wstring &text, const char *icon, bool danger) {
        ui->status->text = text;
        ui->status->icon = icon;
        ui->status->danger = danger;
        Relayout();
    }

    void Begin(ls_image img, bool append) {
        int my = ++gen;
        ui->thumb->SetImage(&img, 480, 1440);
        if (!append) {
            blocks.clear();
            SetEditText(edit, L"");
            link.clear();
            ui->openLink->hidden = true;
        }
        if (Trim(EditText(edit)).empty()) ui->area->overlay = T("ocr.recognizing");
        SetStatus(T("ocr.recognizing"), nullptr, false);
        RecognizeAsync(img, [my, append](Output &o) {
            OcrWindow *w = g_ocrWin;
            if (!w || !w->IsOpen() || w->gen != my) return;
            w->OnResult(o, append);
        });
    }

    std::wstring ComposeOne(const Output &o) {
        return Widen(Compose(o, settings::Bool("ocr.joinLines"), settings::Bool("ocr.forceAsciiPunctuation"),
                             settings::Bool("ocr.preferQRCode")));
    }

    void OnResult(Output &o, bool append) {
        ui->area->overlay.clear();
        if (!o.ok) {
            Relayout();
            SetStatus(i18n::T("ocr.failed", {{"error", ErrorText(o.error)}}), "triangle-alert", true);
            return;
        }
        std::wstring piece = ComposeOne(o);
        std::wstring cur = EditText(edit);
        if (append && !Trim(cur).empty()) {
            if (!piece.empty()) AppendEditText(edit, L"\n\n" + piece);
            blocks.push_back(o);
        } else {
            blocks.clear();
            blocks.push_back(o);
            SetEditText(edit, piece);
        }
        UpdateStatus();
    }

    void RefreshText() {
        if (blocks.empty()) return;
        std::wstring all;
        for (auto &b : blocks) {
            std::wstring t = ComposeOne(b);
            if (t.empty()) continue;
            if (!all.empty()) all += L"\n\n";
            all += t;
        }
        SetEditText(edit, all);
        UpdateStatus();
    }

    // 底部状态：二维码（可打开链接）/ 没有识别到文字 / 空
    void UpdateStatus() {
        ui->area->overlay.clear();
        link.clear();
        bool preferQR = settings::Bool("ocr.preferQRCode");
        const Output *last = blocks.empty() ? nullptr : &blocks.back();
        if (last && ShowsCodes(*last, preferQR)) {
            if (last->codes.size() == 1 && IsLink(last->codes[0])) link = Narrow(Trim(Widen(last->codes[0])));
            ui->openLink->hidden = link.empty();
            SetStatus(link.empty() ? T("ocr.qrcode") : L"", "qr-code", false);
            return;
        }
        ui->openLink->hidden = true;
        if (Trim(EditText(edit)).empty()) ui->area->overlay = T("ocr.empty");
        SetStatus(L"", nullptr, false);
    }

    void CopyResult(bool close) {
        tool::CopyText(EditText(edit));
        if (close) Close();
    }
};

void ShowAndRecognize(ls_image img) {
    bool existing = g_ocrWin && g_ocrWin->IsOpen();
    if (!existing) {
        OcrWindow *w = new OcrWindow();
        if (!w->Create()) {
            delete w;
            ls_image_free(&img);
            return;
        }
        g_ocrWin = w;
    }
    OcrWindow *w = g_ocrWin;
    bool append = existing && IsWindowVisible(w->Hwnd()) && settings::Bool("ocr.continuous") && !w->blocks.empty();
    w->Begin(img, append);
    w->Present(true);
}

// 固定区域：结果窗口挡在区域上时先藏起来，等它从屏幕上消失再截
RECT g_pendingRegion;
bool g_pendingShow = false;

void GrabRegion(const RECT &r) {
    ls_image img;
    bool ok = CaptureScreenRect(r, false, img);
    if (g_pendingShow && g_ocrWin && g_ocrWin->IsOpen()) ShowWindow(g_ocrWin->Hwnd(), SW_SHOWNOACTIVATE);
    g_pendingShow = false;
    if (!ok) {
        capture::Start(capture::Mode::Ocr);
        return;
    }
    HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    Recognize(img, api::MonitorDpi(mon) / 96.f, &r);
}

void CALLBACK PendingTimer(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    GrabRegion(g_pendingRegion);
}

}  // namespace

void Start() {
    if (capture::IsActive()) return;
    RECT r;
    if (settings::Bool("ocr.fixedRegion") && LoadRegion(r) && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
        // 区域按所在屏夹一下（分辨率可能变过）
        HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(mon, &mi);
        RECT in;
        if (IntersectRect(&in, &r, &mi.rcMonitor) && in.right - in.left >= 4 && in.bottom - in.top >= 4) {
            RECT wr;
            bool cover = g_ocrWin && g_ocrWin->IsOpen() && IsWindowVisible(g_ocrWin->Hwnd()) && GetWindowRect(g_ocrWin->Hwnd(), &wr) &&
                         IntersectRect(&wr, &wr, &in);
            if (cover) {
                ShowWindow(g_ocrWin->Hwnd(), SW_HIDE);
                g_pendingRegion = in;
                g_pendingShow = true;
                if (SetTimer(nullptr, 0, 150, &PendingTimer)) return;
            }
            GrabRegion(in);
            return;
        }
    }
    capture::Start(capture::Mode::Ocr);
}

void Recognize(ls_image image, float scale, const RECT *regionPx) {
    (void)scale;
    if (!image.data) return;
    if (regionPx) SaveRegion(*regionPx);
    ShowAndRecognize(image);
}

SizeF SelfTestSize() { return SizeF{kWinW, kWinH}; }

ls_image RenderSelfTest(float scale, const ls_image &thumb, const std::wstring &text) {
    prefs::Widget root;
    Content ui(root);
    ui.thumb->SetImage(&thumb, (int)(kThumbW * scale * 2), 2000);
    ui.area->drawText = true;
    ui.area->text = text;
    root.frame = RectF(0, 0, kWinW, kWinH);
    ui.Layout(kWinW, kWinH);
    return tool::RenderOffscreen(kWinW, kWinH, scale, [&](Canvas &c) {
        c.FillRect(RectF(0, 0, kWinW, kWinH), theme::Ui().windowBg);
        ui.DrawChrome(c);
        root.Draw(c);
    });
}

}  // namespace ocr
}  // namespace ls
