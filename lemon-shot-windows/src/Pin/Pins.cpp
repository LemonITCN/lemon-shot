// 贴图（spec 3）：分层窗口（逐像素透明，圆角阴影），滚轮缩放、拖动、右键菜单、悬停按钮、标注、贴图库。
#include "Pin/Pins.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "App/App.h"
#include "Capture/Export.h"
#include "Capture/Overlay.h"
#include "Capture/Screen.h"
#include "Capture/Toolbar.h"
#include "OCR/OCR.h"
#include "Pin/PinLibrary.h"
#include "Pin/VirtualDesktop.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Controls.h"
#include "UI/Toast.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/ImageIO.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace pins {

namespace {

// 悬停按钮（spec 3）：右上角深色胶囊（tipBg，圆角 = 高度一半），距贴图上、右边缘各 8pt；
// 胶囊里四个 24pt 按钮并排、四周各留 4pt（同 Mac PinWidget.buttonsRect），图标 16pt 白色
const float kBtn = 24, kBtnPad = 4, kBtnInset = 8, kBtnIcon = 16;
const char *kBtnIcons[4] = {"copy", "download", "pencil", "x"};
enum { TIMER_ZOOM = 1 };
enum {
    MENU_COPY = 1,
    MENU_SAVE,
    MENU_ANNOTATE,
    MENU_OCR,
    MENU_OPACITY_100,
    MENU_OPACITY_80,
    MENU_OPACITY_60,
    MENU_OPACITY_40,
    MENU_OPACITY_20,
    MENU_ZOOM_100,
    MENU_ZOOM_50,
    MENU_ZOOM_200,
    MENU_ROTATE_LEFT,
    MENU_ROTATE_RIGHT,
    MENU_FLIP_H,
    MENU_FLIP_V,
    MENU_CORNERS,
    MENU_SHADOW,
    MENU_ALL_DESKTOPS,
    MENU_CLOSE,
    MENU_CLOSE_ALL,
    MENU_HIDE_ALL,
};

float Margin() { return theme::Tok().pin.shadowBlur + theme::Tok().pin.shadowOffsetY; }

RectF ButtonsCapsule(RectF img) {
    float w = 4 * kBtn + 2 * kBtnPad, h = kBtn + 2 * kBtnPad;
    return RectF(img.r() - kBtnInset - w, img.y + kBtnInset, w, h);
}

RectF ButtonRect(RectF img, int i) {
    RectF cap = ButtonsCapsule(img);
    return RectF(cap.x + kBtnPad + (float)i * kBtn, cap.y + kBtnPad, kBtn, kBtn);
}

// 贴图窗内容（屏幕与自测共用）
void DrawPin(Canvas &c, ID2D1Bitmap *bmp, RectF ir, bool corners, bool shadow, bool active, bool buttons, int hoverBtn,
             const std::wstring &zoomText) {
    const auto &pt = theme::Tok().pin;
    float radius = corners ? (float)settings::Num("pin.cornerRadius") : 0.f;
    if (shadow) c.Shadow(ir, radius, pt.shadowBlur, pt.shadowOffsetY, theme::Ui().shadow);
    if (bmp) {
        if (radius > 0) {
            ComPtr<ID2D1RoundedRectangleGeometry> g;
            D2D1_ROUNDED_RECT rr = {ir.D2(), radius, radius};
            gfx::D2D()->CreateRoundedRectangleGeometry(&rr, g.Put());
            c.PushGeometryMask(g.Get());
            c.Bitmap(bmp, ir, nullptr, 1.f, false);
            c.PopLayer();
        } else {
            c.Bitmap(bmp, ir, nullptr, 1.f, false);
        }
    }
    c.StrokeRound(ir.Inflate(pt.borderWidth / 2), radius > 0 ? radius + pt.borderWidth / 2 : 0,
                  active ? pt.activeBorder : pt.inactiveBorder, pt.borderWidth);
    if (buttons) {
        RectF cap = ButtonsCapsule(ir);
        c.FillRound(cap, cap.h / 2, theme::Cap().tipBg);
        for (int i = 0; i < 4; ++i) {
            RectF b = ButtonRect(ir, i);
            // 悬停底色白色 15%，圆角同 Mac IconButton（radius 6）
            if (hoverBtn == i) c.FillRound(b, 6, Color{1, 1, 1, 0.15f});
            c.Icon(kBtnIcons[i], b.cx(), b.cy(), kBtnIcon, Color{1, 1, 1, 1}, 1.75f);
        }
    }
    if (!zoomText.empty()) {
        // 缩放百分比：同 Mac PinWidget（15pt 半粗等宽数字，高 32，左右各 12，圆角 8）
        TextStyle ts;
        ts.size = 15;
        ts.weight = kSemibold;
        ts.color = theme::Cap().tipText;
        ts.h = HAlign::Center;
        ts.tabular = true;
        float w = gfx::Measure(zoomText, ts.size, kSemibold).w + 24;
        RectF z(ir.cx() - w / 2, ir.cy() - 16, w, 32);
        c.FillRound(z, 8, theme::Cap().tipBg);
        c.Text(zoomText, z, ts);
    }
}

class PinEditor;

class PinWindow : public Window {
public:
    ls_image img = {};
    float scale = 1;
    float zoom = 1;
    float opacity = 1;
    bool corners = true, shadow = true, allDesktops = false;
    POINT imgPos = {0, 0};  // 图片左上角的屏幕像素位置
    UINT dpi = 96;
    LayeredSurface surface;
    ComPtr<ID2D1Bitmap> bmp;
    unsigned bmpGen = 0;
    bool hover = false, active = false, tracking = false;
    int hoverBtn = -1;
    bool dragging = false;
    POINT dragStart = {}, posStart = {};
    double zoomShown = 0;
    PinEditor *editor = nullptr;
    bool hidden = false;

    ~PinWindow() override { ls_image_free(&img); }

    bool Open(ls_image image, float imgScale, POINT origin) {
        img = image;
        scale = imgScale > 0 ? imgScale : 1;
        imgPos = origin;
        corners = settings::Bool("pin.corners");
        shadow = settings::Bool("pin.shadow");
        opacity = (float)settings::Num("pin.opacity");
        if (opacity < 0.1f) opacity = 1;
        dpi = api::MonitorDpi(MonitorFromPoint(origin, MONITOR_DEFAULTTONEAREST));
        if (!CreateWin(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP, 0, 0, 1, 1, nullptr, L"LemonShot"))
            return false;
        Render();
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        if (settings::Bool("pin.allDesktops")) SetAllDesktops(true);
        return true;
    }

    float S() const { return dpi / 96.f; }
    RectF ImageRect() const {
        float m = Margin();
        return RectF(m, m, img.width / scale * zoom, img.height / scale * zoom);
    }

    void Render() {
        if (!hwnd_ || !img.data) return;
        float s = S(), m = Margin();
        RectF ir = ImageRect();
        int pw = (int)std::ceil((ir.w + 2 * m) * s), ph = (int)std::ceil((ir.h + 2 * m) * s);
        ID2D1RenderTarget *rt = surface.Begin(pw, ph, dpi);
        if (!rt) return;
        if (!bmp || bmpGen != surface.Generation()) {
            bmp = gfx::CreateBitmap(rt, img);
            bmpGen = surface.Generation();
        }
        {
            Canvas c(rt);
            std::wstring zt;
            if (zoomShown > 0 && NowSeconds() - zoomShown < 1.0) zt = FormatW(L"%d%%", (int)std::lround(zoom * 100));
            bool buttons = hover && settings::Bool("pin.showButtons") && !editor && !dragging;
            DrawPin(c, bmp.Get(), ir, corners, shadow, active || editor != nullptr, buttons, hoverBtn, zt);
        }
        int x = imgPos.x - (int)std::lround(m * s), y = imgPos.y - (int)std::lround(m * s);
        surface.End(hwnd_, x, y, (BYTE)std::lround(Clamp(opacity, 0.1f, 1.f) * 255));
    }

    void ReplaceImage(ls_image image, float imgScale) {
        ls_image_free(&img);
        img = image;
        scale = imgScale;
        bmp.Reset();
        AddToLibrary(img, "capture");
        Render();
    }

    void SetZoom(float z, POINT anchor) {
        const auto &pt = theme::Tok().pin;
        z = Clamp(z, pt.zoomMin, pt.zoomMax);
        if (std::fabs(z - zoom) < 1e-4f) return;
        // 以光标为中心缩放：光标下的图片点不动
        imgPos.x = anchor.x - (LONG)std::lround((anchor.x - imgPos.x) * (double)z / zoom);
        imgPos.y = anchor.y - (LONG)std::lround((anchor.y - imgPos.y) * (double)z / zoom);
        zoom = z;
        zoomShown = NowSeconds();
        SetTimer(hwnd_, TIMER_ZOOM, 1000, nullptr);
        Render();
    }

    void Rotate(bool clockwise) {
        ls_image out;
        if (ls_image_rotate90(&img, clockwise ? 1 : 0, &out) == 0 && out.data) {
            ls_image_free(&img);
            img = out;
            bmp.Reset();
            Render();
        }
    }

    void Flip(bool horizontal) {
        ls_image out;
        if (ls_image_flip(&img, horizontal ? 1 : 0, &out) == 0 && out.data) {
            ls_image_free(&img);
            img = out;
            bmp.Reset();
            Render();
        }
    }

    void Copy() {
        if (CopyImageToClipboard(app::MainHwnd(), img)) toast::Show(T("notify.copied"));
    }

    void Save() {
        std::wstring path, err;
        if (exporter::SaveImage(img, scale, settings::Bool("save.askLocation"), path, err)) {
            toast::Action a;
            a.kind = toast::kReveal;
            a.path = path;
            toast::Show(T("notify.saved"), "circle-check", a);
        } else if (!err.empty()) {
            toast::Show(i18n::T("notify.saveFailed", {{"error", err}}), "triangle-alert");
        }
    }

    void SetAllDesktops(bool on) {
        if (vdesk::Pin(hwnd_, on)) allDesktops = on;
    }

    void Annotate();
    void CloseEditor();
    void ShowMenu(POINT pt);
    void Close();

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;
    void OnDestroyed() override;
};

std::vector<PinWindow *> g_pins;

// ---------------------------------------------------------------- 贴图标注：复用截图层（lock_selection 会话，工具栏贴在图下方）

void DeleteEditor(void *p);

class PinEditor : public capture::OverlayHost {
public:
    PinWindow *pin;
    MonitorShot shot;
    std::unique_ptr<capture::Overlay> overlay;
    bool closed = false;

    explicit PinEditor(PinWindow *p) : pin(p) {}

    bool Open() {
        float s = pin->S();
        float iw = pin->img.width / pin->scale, ih = pin->img.height / pin->scale;
        int ipw = (int)std::lround(iw * s), iph = (int)std::lround(ih * s);
        float tw = ToolbarWidth(true);
        // 会话区域：图片 + 下方工具栏、样式面板与气泡的空间（窗口区域会裁成只有这几块）
        float W = (iw > tw + 12 ? iw : tw + 12);
        float H = ih + 8 + ToolbarHeight() + 6 + 120 + 80;
        int bw = (int)std::ceil(W * s), bh = (int)std::ceil(H * s);
        void *bits = nullptr;
        BITMAPINFO bi;
        std::memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = bw;
        bi.bmiHeader.biHeight = -bh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        shot.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!shot.dib || !bits) return false;
        std::memset(bits, 0, (size_t)bw * bh * 4);
        // 图片按窗口 DPI 的像素密度放进左上角
        ls_image src = pin->img;
        ls_image resized = {};
        if (ipw != src.width || iph != src.height) {
            if (ls_image_resize(&src, ipw, iph, &resized) == 0 && resized.data) src = resized;
        }
        int cw = src.width < bw ? src.width : bw, ch = src.height < bh ? src.height : bh;
        for (int y = 0; y < ch; ++y)
            std::memcpy((uint8_t *)bits + (size_t)y * bw * 4, src.data + (size_t)y * src.stride, (size_t)cw * 4);
        if (resized.data) ls_image_free(&resized);
        shot.img = ls_image{(uint8_t *)bits, bw, bh, bw * 4};
        shot.dpi = pin->dpi;
        shot.scale = s;
        shot.lw = bw / s;
        shot.lh = bh / s;
        shot.rc = RECT{pin->imgPos.x, pin->imgPos.y, pin->imgPos.x + bw, pin->imgPos.y + bh};
        shot.mon = MonitorFromPoint(pin->imgPos, MONITOR_DEFAULTTONEAREST);
        ls_rect initial = {0, 0, ipw / s, iph / s};
        overlay.reset(new capture::Overlay(this, &shot, 2));
        std::vector<WindowEntry> none;
        if (!overlay->Open(none, &initial)) return false;
        ShowWindow(overlay->Hwnd(), SW_SHOW);
        UpdateWindow(overlay->Hwnd());
        SetForegroundWindow(overlay->Hwnd());
        return true;
    }

    std::vector<capture::Overlay *> Overlays() override {
        std::vector<capture::Overlay *> v;
        if (overlay) v.push_back(overlay.get());
        return v;
    }
    capture::Overlay *MouseOverlay() override { return overlay.get(); }
    void SetMouseOverlay(capture::Overlay *) override {}
    capture::Overlay *KeyTarget() override { return overlay.get(); }
    void ProcessActions(capture::Overlay *o) override {
        for (int i = 0; i < 16 && !closed; ++i) {
            ls_action a = ls_session_take_action(o->Session());
            if (a == LS_ACTION_NONE) break;
            HandleAction(o, a);
        }
    }
    void HandleAction(capture::Overlay *o, ls_action a) override {
        if (closed) return;
        switch (a) {
        case LS_ACTION_CANCEL: Close(true); break;
        case LS_ACTION_DONE: {
            ls_image out;
            if (o->RenderResult(out)) {
                PinWindow *p = pin;
                float s = shot.scale;
                Close(true);
                p->ReplaceImage(out, s);
            } else {
                Close(true);
            }
            break;
        }
        case LS_ACTION_COPY:
        case LS_ACTION_SAVE: {
            ls_image out;
            if (!o->RenderResult(out)) break;
            if (a == LS_ACTION_COPY) {
                if (CopyImageToClipboard(app::MainHwnd(), out)) toast::Show(T("notify.copied"));
            } else {
                std::wstring path, err;
                if (exporter::SaveImage(out, shot.scale, settings::Bool("save.askLocation"), path, err)) {
                    toast::Action act;
                    act.kind = toast::kReveal;
                    act.path = path;
                    toast::Show(T("notify.saved"), "circle-check", act);
                }
            }
            ls_image_free(&out);
            break;
        }
        default: break;
        }
    }
    void Close(bool) override {
        if (closed) return;
        closed = true;
        if (overlay) overlay->Hide();
        if (pin) {
            pin->editor = nullptr;
            pin->Render();
            if (pin->Hwnd()) SetForegroundWindow(pin->Hwnd());
        }
        app::Defer(&DeleteEditor, this);
    }
};

void DeleteEditor(void *p) { delete (PinEditor *)p; }

void PinWindow::Annotate() {
    if (editor) return;
    zoom = 1;
    Render();
    editor = new PinEditor(this);
    if (!editor->Open()) {
        PinEditor *e = editor;
        editor = nullptr;
        e->closed = true;
        delete e;
    }
    Render();
}

void PinWindow::CloseEditor() {
    if (editor) {
        editor->pin = nullptr;
        editor->Close(false);
        editor = nullptr;
    }
}

void PinWindow::Close() {
    CloseEditor();
    DestroyWin();
}

void PinWindow::OnDestroyed() {
    for (size_t i = 0; i < g_pins.size(); ++i)
        if (g_pins[i] == this) {
            g_pins.erase(g_pins.begin() + (long)i);
            break;
        }
    delete this;
}

void PinWindow::ShowMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    HMENU op = CreatePopupMenu();
    HMENU zm = CreatePopupMenu();
    int pct[5] = {100, 80, 60, 40, 20};
    for (int i = 0; i < 5; ++i) {
        UINT flags = MF_STRING;
        if (std::lround(opacity * 100) == pct[i]) flags |= MF_CHECKED;
        AppendMenuW(op, flags, MENU_OPACITY_100 + i, i18n::T("common.percent", {{"n", FormatW(L"%d", pct[i])}}).c_str());
    }
    AppendMenuW(zm, MF_STRING | (std::fabs(zoom - 1) < 0.01f ? MF_CHECKED : 0), MENU_ZOOM_100, T("pin.actualSize").c_str());
    AppendMenuW(zm, MF_STRING | (std::fabs(zoom - 0.5f) < 0.01f ? MF_CHECKED : 0), MENU_ZOOM_50,
                i18n::T("common.percent", {{"n", L"50"}}).c_str());
    AppendMenuW(zm, MF_STRING | (std::fabs(zoom - 2) < 0.01f ? MF_CHECKED : 0), MENU_ZOOM_200,
                i18n::T("common.percent", {{"n", L"200"}}).c_str());
    AppendMenuW(m, MF_STRING, MENU_COPY, (T("pin.copy") + L"\tCtrl+C").c_str());
    AppendMenuW(m, MF_STRING, MENU_SAVE, (T("pin.save") + L"\tCtrl+S").c_str());
    AppendMenuW(m, MF_STRING, MENU_ANNOTATE, T("pin.annotate").c_str());
    AppendMenuW(m, MF_STRING, MENU_OCR, T("pin.ocr").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)op, T("pin.opacity").c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)zm, T("pin.zoom").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, MENU_ROTATE_LEFT, T("action.rotateLeft").c_str());
    AppendMenuW(m, MF_STRING, MENU_ROTATE_RIGHT, T("action.rotateRight").c_str());
    AppendMenuW(m, MF_STRING, MENU_FLIP_H, T("action.flipH").c_str());
    AppendMenuW(m, MF_STRING, MENU_FLIP_V, T("action.flipV").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (corners ? MF_CHECKED : 0), MENU_CORNERS, T("pin.corners").c_str());
    AppendMenuW(m, MF_STRING | (shadow ? MF_CHECKED : 0), MENU_SHADOW, T("pin.shadow").c_str());
    UINT vd = MF_STRING | (allDesktops ? MF_CHECKED : 0) | (vdesk::Available(hwnd_) ? 0 : MF_GRAYED);
    AppendMenuW(m, vd, MENU_ALL_DESKTOPS, T("pin.allDesktops").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, MENU_CLOSE, (T("pin.close") + L"\tEsc").c_str());
    AppendMenuW(m, MF_STRING, MENU_CLOSE_ALL, T("pin.closeAll").c_str());
    AppendMenuW(m, MF_STRING, MENU_HIDE_ALL, T("pin.hideAll").c_str());
    SetForegroundWindow(hwnd_);
    int cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(m);  // 连同子菜单一起销毁
    POINT center = {imgPos.x + (LONG)std::lround(ImageRect().w * S() / 2), imgPos.y + (LONG)std::lround(ImageRect().h * S() / 2)};
    switch (cmd) {
    case MENU_COPY: Copy(); break;
    case MENU_SAVE: Save(); break;
    case MENU_ANNOTATE: Annotate(); break;
    case MENU_OCR: ocr::Recognize(CloneImage(img), scale); break;
    case MENU_OPACITY_100:
    case MENU_OPACITY_80:
    case MENU_OPACITY_60:
    case MENU_OPACITY_40:
    case MENU_OPACITY_20:
        opacity = pct[cmd - MENU_OPACITY_100] / 100.f;
        Render();
        break;
    case MENU_ZOOM_100: SetZoom(1, center); break;
    case MENU_ZOOM_50: SetZoom(0.5f, center); break;
    case MENU_ZOOM_200: SetZoom(2, center); break;
    case MENU_ROTATE_LEFT: Rotate(false); break;
    case MENU_ROTATE_RIGHT: Rotate(true); break;
    case MENU_FLIP_H: Flip(true); break;
    case MENU_FLIP_V: Flip(false); break;
    case MENU_CORNERS:
        corners = !corners;
        Render();
        break;
    case MENU_SHADOW:
        shadow = !shadow;
        Render();
        break;
    case MENU_ALL_DESKTOPS: SetAllDesktops(!allDesktops); break;
    case MENU_CLOSE: Close(); break;
    case MENU_CLOSE_ALL: CloseAll(); break;
    case MENU_HIDE_ALL: HideAll(); break;
    }
}

LRESULT PinWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    float s = S();
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_ACTIVATE:
        active = LOWORD(wp) != WA_INACTIVE;
        Render();
        break;
    case WM_DPICHANGED:
        dpi = HIWORD(wp);
        bmp.Reset();
        Render();
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking) {
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking = true;
        }
        POINT p;
        GetCursorPos(&p);
        if (dragging) {
            imgPos.x = posStart.x + (p.x - dragStart.x);
            imgPos.y = posStart.y + (p.y - dragStart.y);
            float m = Margin();
            SetWindowPos(hwnd_, nullptr, imgPos.x - (int)std::lround(m * s), imgPos.y - (int)std::lround(m * s), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        float x = MouseX(lp) / s, y = MouseY(lp) / s;
        int hb = -1;
        RectF ir = ImageRect();
        if (settings::Bool("pin.showButtons") && !editor)
            for (int i = 0; i < 4; ++i)
                if (ButtonRect(ir, i).Contains(x, y)) hb = i;
        bool h = ir.Contains(x, y);
        if (h != hover || hb != hoverBtn) {
            hover = h;
            hoverBtn = hb;
            Render();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        if (hover || hoverBtn >= 0) {
            hover = false;
            hoverBtn = -1;
            Render();
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, hoverBtn >= 0 ? MAKEINTRESOURCEW(32649) : MAKEINTRESOURCEW(32646)));
        return TRUE;
    case WM_LBUTTONDOWN: {
        if (editor) return 0;
        if (hoverBtn >= 0) return 0;
        RectF ir = ImageRect();
        if (!ir.Contains(MouseX(lp) / s, MouseY(lp) / s)) return 0;
        dragging = true;
        GetCursorPos(&dragStart);
        posStart = imgPos;
        SetCapture(hwnd_);
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging) {
            dragging = false;
            ReleaseCapture();
            Render();
            return 0;
        }
        if (hoverBtn >= 0 && !editor) {
            int b = hoverBtn;
            if (b == 0) Copy();
            else if (b == 1) Save();
            else if (b == 2) Annotate();
            else Close();
        }
        return 0;
    case WM_CAPTURECHANGED: dragging = false; return 0;
    case WM_LBUTTONDBLCLK:
        if (!editor && hoverBtn < 0 && settings::Bool("pin.doubleClickCloses")) Close();
        return 0;
    case WM_RBUTTONUP: {
        if (editor) return 0;
        POINT p;
        GetCursorPos(&p);
        ShowMenu(p);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (editor) return 0;
        float steps = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        const auto &pt = theme::Tok().pin;
        if (GetKeyState(VK_CONTROL) < 0) {
            opacity = Clamp(opacity + steps * pt.opacityStep, 0.1f, 1.f);
            Render();
        } else {
            POINT p = {MouseX(lp), MouseY(lp)};
            SetZoom(std::floor((zoom + steps * pt.zoomStep) * 100.f + 0.5f) / 100.f, p);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (editor) return 0;
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
        switch (wp) {
        case VK_ESCAPE: Close(); return 0;
        case VK_LEFT: imgPos.x -= step; break;
        case VK_RIGHT: imgPos.x += step; break;
        case VK_UP: imgPos.y -= step; break;
        case VK_DOWN: imgPos.y += step; break;
        case 'C':
            if (ctrl) Copy();
            return 0;
        case 'S':
            if (ctrl) Save();
            return 0;
        default: return 0;
        }
        Render();
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_ZOOM) {
            KillTimer(hwnd_, TIMER_ZOOM);
            zoomShown = 0;
            Render();
        }
        return 0;
    case WM_CLOSE: Close(); return 0;
    }
    return Window::Handle(msg, wp, lp);
}

// 剪贴板文字渲染成图：最大宽 600pt，surface 底 + 12pt 内边距，字 13pt
bool RenderText(const std::wstring &text, float scale, ls_image &out) {
    const float maxW = 600, pad = 12, size = 13;
    gfx::TextSize ts = gfx::Measure(text, size, kRegular, false, 100000.f);
    float tw = ts.w;
    if (tw > maxW - pad * 2) {
        ts = gfx::Measure(text, size, kRegular, false, maxW - pad * 2);
        tw = maxW - pad * 2;
    }
    float w = std::ceil(tw + pad * 2), h = std::ceil(ts.h + pad * 2);
    if (h > 4000) h = 4000;
    Offscreen off;
    if (!off.Create((int)std::ceil(w * scale), (int)std::ceil(h * scale), scale)) return false;
    off.RT()->BeginDraw();
    {
        Canvas c(off.RT());
        c.Clear(theme::Ui().surface);
        TextStyle st;
        st.size = size;
        st.color = theme::Ui().text;
        st.wrap = true;
        st.v = VAlign::Top;
        c.Text(text, RectF(pad, pad, tw + 1, h - pad * 2), st);
    }
    if (FAILED(off.RT()->EndDraw(nullptr, nullptr))) return false;
    return off.ReadPixels(out);
}

}  // namespace

void Create(ls_image img, float scale, POINT originPx, const char *source) {
    if (!img.data) return;
    AddToLibrary(img, source ? source : "capture");
    PinWindow *p = new PinWindow();
    if (!p->Open(img, scale, originPx)) {
        delete p;
        return;
    }
    g_pins.push_back(p);
}

void PinClipboard() {
    POINT pt;
    GetCursorPos(&pt);
    float scale = ScaleForPoint(pt);
    ls_image img;
    if (ReadClipboardImage(app::MainHwnd(), img)) {
        Create(img, scale, pt, "clipboard");
        return;
    }
    std::wstring text;
    if (ReadClipboardText(app::MainHwnd(), text) && !Trim(text).empty()) {
        if (RenderText(text, scale, img)) {
            Create(img, scale, pt, "clipboard");
            return;
        }
    }
    toast::Show(T("pin.noClipboard"), "clipboard");
}

void ToggleAll() {
    if (g_pins.empty()) return;
    bool anyVisible = false;
    for (auto *p : g_pins)
        if (p->Hwnd() && IsWindowVisible(p->Hwnd())) anyVisible = true;
    if (anyVisible) {
        HideAll();
    } else {
        for (auto *p : g_pins) ShowWindow(p->Hwnd(), SW_SHOWNOACTIVATE);
    }
}

void HideAll() {
    for (auto *p : g_pins) {
        p->CloseEditor();
        ShowWindow(p->Hwnd(), SW_HIDE);
    }
}

void CloseAll() {
    std::vector<PinWindow *> copy = g_pins;
    for (auto *p : copy) p->Close();
}

int Count() { return (int)g_pins.size(); }

void ShowLibrary() { library::Show(); }

void OnSettingsChanged() {
    for (auto *p : g_pins) p->Render();
}

void ApplyAllDesktops(bool on) {
    for (auto *p : g_pins) p->SetAllDesktops(on);
}

RectF PinPreviewSize(float imgW, float imgH) {
    float m = Margin();
    return RectF(0, 0, imgW + 2 * m, imgH + 2 * m);
}

void DrawPinPreview(Canvas &c, ID2D1Bitmap *img, float imgW, float imgH, bool active, bool buttons, int hoverBtn) {
    float m = Margin();
    DrawPin(c, img, RectF(m, m, imgW, imgH), settings::Bool("pin.corners"), settings::Bool("pin.shadow"), active, buttons,
            hoverBtn, std::wstring());
}

}  // namespace pins
}  // namespace ls
