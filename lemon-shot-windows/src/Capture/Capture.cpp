// 截图层（spec 2.1–2.10）：每块屏一个置顶无边框窗口 + 一个内核会话；事件转发给内核，按内核数据绘制，处理平台动作。
#include "Capture/Capture.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <imm.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "App/App.h"
#include "App/Hotkeys.h"
#include "Capture/CaptureView.h"
#include "Capture/Export.h"
#include "Capture/Overlay.h"
#include "Capture/Screen.h"
#include "Capture/StyleSettings.h"
#include "LongShot/LongShot.h"
#include "OCR/OCR.h"
#include "Record/Record.h"
#include "Record/SetupPanel.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "Translate/Translate.h"
#include "UI/Toast.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/ImageIO.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace capture {

namespace {

enum { TIMER_HOVER = 1 };
enum { ID_TEXT_EDIT = 100, ID_SIZE_W = 101, ID_SIZE_H = 102 };
enum { END_TEXT = 1, END_SIZE = 2, APPLY_SIZE = 3, SWITCH_SIZE = 4 };

// 美式键盘物理键位 → ls 键值（切换输入法或布局不影响）
int MapKey(WPARAM vk, LPARAM lp) {
    switch (vk) {
    case VK_ESCAPE: return LS_KEY_ESCAPE;
    case VK_RETURN: return LS_KEY_ENTER;
    case VK_TAB: return LS_KEY_TAB;
    case VK_BACK: return LS_KEY_BACKSPACE;
    case VK_DELETE: return LS_KEY_DELETE;
    case VK_LEFT: return LS_KEY_LEFT;
    case VK_RIGHT: return LS_KEY_RIGHT;
    case VK_UP: return LS_KEY_UP;
    case VK_DOWN: return LS_KEY_DOWN;
    case VK_SPACE: return 32;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return (int)('0' + (vk - VK_NUMPAD0));
    UINT scan = (UINT)((lp >> 16) & 0xFF);
    bool ext = ((lp >> 24) & 1) != 0;
    if (!ext) {
        if (scan >= 0x02 && scan <= 0x0B) return "1234567890"[scan - 0x02];
        if (scan >= 0x10 && scan <= 0x19) return "QWERTYUIOP"[scan - 0x10];
        if (scan >= 0x1E && scan <= 0x26) return "ASDFGHJKL"[scan - 0x1E];
        if (scan >= 0x2C && scan <= 0x32) return "ZXCVBNM"[scan - 0x2C];
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return (int)vk;
    return 0;
}

HCURSOR SysCursor(int c) {
    const wchar_t *id = MAKEINTRESOURCEW(32512);  // IDC_ARROW
    switch (c) {
    case LS_CURSOR_CROSSHAIR: id = MAKEINTRESOURCEW(32515); break;
    case LS_CURSOR_MOVE: id = MAKEINTRESOURCEW(32646); break;
    case LS_CURSOR_RESIZE_NS: id = MAKEINTRESOURCEW(32645); break;
    case LS_CURSOR_RESIZE_EW: id = MAKEINTRESOURCEW(32644); break;
    case LS_CURSOR_RESIZE_NWSE: id = MAKEINTRESOURCEW(32642); break;
    case LS_CURSOR_RESIZE_NESW: id = MAKEINTRESOURCEW(32643); break;
    case LS_CURSOR_IBEAM: id = MAKEINTRESOURCEW(32513); break;
    case LS_CURSOR_POINTER: id = MAKEINTRESOURCEW(32649); break;
    default: break;
    }
    return LoadCursorW(nullptr, id);
}

COLORREF ToColorRef(ls_color c) { return RGB(c.r, c.g, c.b); }

}  // namespace

// ---------------------------------------------------------------- 控制器

namespace {

class Controller : public OverlayHost {
public:
    Snapshot snap;
    std::vector<std::unique_ptr<Overlay>> overlays;
    Mode mode = Mode::Normal;
    bool closed = false;
    Overlay *mouseOverlay = nullptr;
    double lastAltUp = 0;
    bool altClean = false;

    bool Open(Mode m);
    void RestoreFocus();
    void Output(Overlay *o, int kind);
    bool RenderImage(Overlay *o, ls_image &img, POINT &origin);
    bool ComposeWindows(Overlay *o, ls_image &img, POINT &origin);
    void SaveLastArea(Overlay *o);

    std::vector<Overlay *> Overlays() override {
        std::vector<Overlay *> v;
        for (auto &o : overlays) v.push_back(o.get());
        return v;
    }
    Overlay *MouseOverlay() override { return mouseOverlay; }
    void SetMouseOverlay(Overlay *o) override { mouseOverlay = o; }
    Overlay *KeyTarget() override;
    void OnSelectionActivity(Overlay *o) override;
    void ProcessActions(Overlay *o) override;
    void HandleAction(Overlay *o, ls_action a) override;
    void CopyColor(Overlay *o, ls_action a) override;
    void Close(bool restoreFocus) override;
    void OnAltKey(Overlay *o, bool down) override;
    void OnOtherKey() override {
        altClean = false;
        lastAltUp = 0;
    }
    // 全局文字识别/翻译/长截图进来的截图层：选完区域直接交给对应模块
    ls_action PurposeAction() const {
        if (mode == Mode::Ocr) return LS_ACTION_OCR;
        if (mode == Mode::Translate) return LS_ACTION_TRANSLATE;
        if (mode == Mode::LongShot) return LS_ACTION_LONGSHOT;
        return LS_ACTION_NONE;
    }
    void OnSelectionFinished(Overlay *o) override {
        if (!closed && PurposeAction() != LS_ACTION_NONE) HandleAction(o, PurposeAction());
    }
};

Controller *g_ctl = nullptr;

void DeleteController(void *p) {
    Controller *c = (Controller *)p;
    if (g_ctl == c) g_ctl = nullptr;
    delete c;
}

}  // namespace

// ---------------------------------------------------------------- Overlay 实现

Overlay::~Overlay() {
    DestroyEditors();
    if (lastRegion_) DeleteObject(lastRegion_);
    if (session_) ls_session_destroy(session_);
    if (editFont_) DeleteObject(editFont_);
    if (sizeFont_) DeleteObject(sizeFont_);
    if (editBrush_) DeleteObject(editBrush_);
    if (sizeBrush_) DeleteObject(sizeBrush_);
}

void Overlay::Wheel(POINT screen, float dy) {
    if (!session_ || opt_.colorPicker || !hwnd_) return;
    ScreenToClient(hwnd_, &screen);
    float x = screen.x / shot_->scale, y = screen.y / shot_->scale;
    if (InUiZone(x, y)) return;
    Dispatch(ls_session_scroll(session_, x, y, dy, CurrentMods()));
}

bool Overlay::RenderResult(ls_image &out) {
    if (!session_) return false;
    return exporter::RenderSelection(session_, shot_->img, shot_->scale, out);
}

// 贴图标注：窗口区域只留图片、工具栏、样式面板与气泡，其余透明
void Overlay::UpdateRegion() {
    if (!opt_.pinMode || !hwnd_) return;
    float s = shot_->scale;
    auto px = [s](RectF r) {
        RECT o = {(LONG)std::floor(r.x * s), (LONG)std::floor(r.y * s), (LONG)std::ceil(r.r() * s), (LONG)std::ceil(r.b() * s)};
        return o;
    };
    ls_session_state st;
    ls_session_get_state(session_, &st);
    RECT img = px(RectF(st.selection));
    HRGN rgn = CreateRectRgn(img.left, img.top, img.right, img.bottom);
    auto add = [&](RectF r) {
        if (r.Empty()) return;
        RECT q = px(r);
        HRGN part = CreateRectRgn(q.left, q.top, q.right, q.bottom);
        CombineRgn(rgn, rgn, part, RGN_OR);
        DeleteObject(part);
    };
    if (ui_.hasToolbar) add(ui_.toolbar.rect);
    if (ui_.hasPopover) add(ui_.popoverRect);
    // 气泡可能出现在工具栏上下：把工具栏上下各一条带区域都留出来
    if (ui_.hasToolbar && (ui_.tooltipBtn || ui_.popHover >= 0)) {
        RectF band(0, ui_.toolbar.rect.y - 120, shot_->lw, ui_.toolbar.rect.h + 240);
        if (ui_.hasPopover) band = RectF(0, 0, shot_->lw, shot_->lh);
        add(band.Intersect(RectF(0, st.selection.y + st.selection.h, shot_->lw, shot_->lh)));
    }
    if (lastRegion_ && EqualRgn(lastRegion_, rgn)) {
        DeleteObject(rgn);
        return;
    }
    if (lastRegion_) DeleteObject(lastRegion_);
    lastRegion_ = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(lastRegion_, rgn, nullptr, RGN_COPY);
    SetWindowRgn(hwnd_, rgn, TRUE);  // 系统接管 rgn
}

bool Overlay::Open(const std::vector<WindowEntry> &windows, const ls_rect *initial) {
    const RECT &r = shot_->rc;
    for (auto &w : windows) {
        RECT in;
        if (!IntersectRect(&in, &w.rc, &r)) continue;
        ls_window lw;
        lw.id = (int64_t)(intptr_t)w.hwnd;
        lw.rect = ls_rect{(w.rc.left - r.left) / shot_->scale, (w.rc.top - r.top) / shot_->scale,
                          (w.rc.right - w.rc.left) / shot_->scale, (w.rc.bottom - w.rc.top) / shot_->scale};
        windows_.push_back(lw);
    }
    BuildSession(initial);
    if (!CreateWin(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, WS_POPUP | WS_CLIPCHILDREN, r.left, r.top, r.right - r.left,
                   r.bottom - r.top, nullptr, L"LemonShot"))
        return false;
    // 截图层本身不接输入法（快捷键是单个字母）；文字输入框单独关联默认输入法
    ImmAssociateContext(hwnd_, nullptr);
    rt_ = gfx::CreateHwndTarget(hwnd_, shot_->dpi);
    if (rt_) frozen_.Create(rt_.Get(), shot_->img);
    cursor_ = SysCursor(LS_CURSOR_CROSSHAIR);
    if (opt_.recordMode) CreateRecordPanel();
    // 贴图标注：显示之前先把窗口区域裁好，避免透明区闪一下黑
    if (opt_.pinMode) {
        Layout();
        UpdateRegion();
    }
    return true;
}

void Overlay::BuildSession(const ls_rect *initial) {
    if (session_) ls_session_destroy(session_);
    const auto &cap = theme::Tok().capture;
    ls_session_config cfg = ls_session_default_config();
    cfg.width = shot_->lw;
    cfg.height = shot_->lh;
    cfg.scale = shot_->scale;
    cfg.base = shot_->img;
    cfg.detect_windows = (opt_.colorPicker || opt_.pinMode) ? 0 : (settings::Bool("capture.detectWindows") ? 1 : 0);
    cfg.right_click_exits = settings::Bool("capture.rightClickExits") ? 1 : 0;
    cfg.lock_selection = opt_.pinMode ? 1 : 0;
    cfg.handle_hit = cap.handleHitSize;
    cfg.edge_hit = cap.edgeHitSize;
    cfg.min_selection = cap.minSelection;
    if (initial) {
        cfg.has_initial_selection = 1;
        cfg.initial_selection = *initial;
    }
    session_ = ls_session_create(&cfg);
    if (!windows_.empty()) ls_session_set_windows(session_, windows_.data(), (int32_t)windows_.size());
    // 上次区域（按屏）
    const Json *la = opt_.pinMode ? nullptr : settings::State().Find("lastArea");
    const Json *mine = la ? la->Find(Narrow(shot_->device)) : nullptr;
    if (mine && mine->IsArray() && mine->Size() == 4) {
        ls_rect r = {(float)mine->At(0).AsNumber(), (float)mine->At(1).AsNumber(), (float)mine->At(2).AsNumber(),
                     (float)mine->At(3).AsNumber()};
        if (r.w > 0 && r.h > 0) ls_session_set_last_area(session_, r);
    }
    hotkeys::BindSession(session_);
    if (opt_.recordMode) {
        // 录屏模式：单键只保留 F 全屏、D 上次区域、V 开始录制、Tab 显隐面板
        const ls_binding off[] = {LS_BIND_LONGSHOT, LS_BIND_OCR, LS_BIND_TRANSLATE, LS_BIND_PIN,
                                  LS_BIND_COPY_RGB, LS_BIND_COPY_HEX, LS_BIND_COPY_CUSTOM};
        for (int t = LS_TOOL_RECT; t <= LS_TOOL_MAGNIFY; ++t) ls_session_bind_key(session_, (ls_binding)t, 0);
        for (ls_binding b : off) ls_session_bind_key(session_, b, 0);
    }
    style::ApplyAll(session_);
    ls_session_set_text_measurer(session_, &gfx::SessionMeasure, nullptr);
}

void Overlay::CreateRecordPanel() {
    Overlay *self = this;
    recordPanel_ = std::make_shared<rec::SetupPanel>(
        hwnd_, shot_->scale, shot_->dpi, [self]() { self->Redraw(); },
        [self]() { self->host_->HandleAction(self, LS_ACTION_RECORD); });
    ui_.recordPanel = recordPanel_.get();
}

void Overlay::EnterRecordMode() {
    if (opt_.recordMode || !session_) return;
    ls_session_state st;
    ls_session_get_state(session_, &st);
    ls_rect sel = st.selection;
    bool has = st.has_selection && st.window_count == 0;
    DestroyEditors();
    KillTimer(hwnd_, TIMER_HOVER);
    opt_.recordMode = true;
    ui_ = CaptureUi();
    patches_.Clear();
    BuildSession(has ? &sel : nullptr);
    CreateRecordPanel();
    if (mouseInside)
        Dispatch(ls_session_mouse_move(session_, lastX_, lastY_, 0) | LS_EV_CURSOR | LS_EV_REDRAW);
    else
        ls_session_mouse_exit(session_);
    Redraw();
}

void Overlay::ResetSession() {
    DestroyEditors();
    ui_ = CaptureUi();
    ui_.recordPanel = recordPanel_.get();
    patches_.Clear();
    BuildSession(nullptr);
    if (!mouseInside) ls_session_mouse_exit(session_);
    Redraw();
}

void Overlay::Paint() {
    if (!rt_) {
        rt_ = gfx::CreateHwndTarget(hwnd_, shot_->dpi);
        if (!rt_) return;
        frozen_.Create(rt_.Get(), shot_->img);
        patches_.Clear();
    }
    rt_->BeginDraw();
    {
        Canvas c(rt_.Get());
        c.SetTransform(MatIdentity());
        CaptureFrame f = Frame();
        RenderCapture(c, f, ui_);
    }
    HRESULT hr = rt_->EndDraw(nullptr, nullptr);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        frozen_.Reset();
        patches_.Clear();
        rt_.Reset();
        Redraw();
    }
    UpdateRegion();
}

bool Overlay::InUiZone(float x, float y) {
    if (ui_.hasToolbar && ui_.toolbar.rect.Inflate(0, theme::Tok().capture.toolbarGap).Contains(x, y)) return true;
    if (ui_.hasPopover && ui_.popoverRect.Inflate(theme::Tok().popover.gap).Contains(x, y)) return true;
    if (ui_.hasRecordPanel && recordPanel_ && recordPanel_->Rect().Inflate(0, theme::Tok().capture.toolbarGap).Contains(x, y))
        return true;
    return false;
}

void Overlay::SetHoverButton(int id) {
    if (ui_.hoverBtn == id) return;
    ui_.hoverBtn = id;
    ui_.tooltipBtn = 0;
    KillTimer(hwnd_, TIMER_HOVER);
    if (id) {
        int delay = settings::Int("capture.toolbarPopupDelayMs");
        SetTimer(hwnd_, TIMER_HOVER, (UINT)(delay < 1 ? 1 : delay), nullptr);
    }
    Redraw();
}

void Overlay::OnMouseMove(LPARAM lp) {
    float x = LX(lp), y = LY(lp);
    lastX_ = x;
    lastY_ = y;
    if (!tracking_) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        tracking_ = true;
    }
    if (!mouseInside) {
        mouseInside = true;
        host_->SetMouseOverlay(this);
        // 光标所在屏拿键盘焦点（任何屏在输入文字时都不抢）
        HWND fg = GetForegroundWindow();
        if (fg != hwnd_) {
            bool ours = false, editing = false;
            for (Overlay *o : host_->Overlays()) {
                if (o->Hwnd() == fg) ours = true;
                if (o->Editing()) editing = true;
            }
            if (ours && !editing) SetForegroundWindow(hwnd_);
        }
    }
    if (sliderDrag_) {
        ApplyPopover(ui_.popPressed, x - ui_.popoverRect.x, y - ui_.popoverRect.y, true);
        return;
    }
    if (ui_.pressedBtn) {
        int hit = HitToolbar(ui_.toolbar, x, y);
        ui_.hoverBtn = hit == ui_.pressedBtn ? hit : 0;
        Redraw();
        return;
    }
    Layout();
    if (recordPanel_ && !sessionDown_) {
        bool on = ui_.hasRecordPanel && recordPanel_->MouseMove(x, y);
        if (on || recordPanel_->Pressed()) {
            SetCursorShape(SysCursor(LS_CURSOR_ARROW));
            return;
        }
    }
    if (!sessionDown_ && InUiZone(x, y)) {
        int btn = ui_.hasToolbar ? HitToolbar(ui_.toolbar, x, y) : 0;
        int pop = ui_.hasPopover ? HitPopover(ui_.popover, x - ui_.popoverRect.x, y - ui_.popoverRect.y) : -1;
        SetHoverButton(btn);
        if (pop != ui_.popHover) {
            ui_.popHover = pop;
            Redraw();
        }
        SetCursorShape(SysCursor(LS_CURSOR_ARROW));
        return;
    }
    // 离开工具栏与样式面板：悬停弹出的样式面板收起
    if (ui_.hoverBtn || ui_.popHover >= 0 || ui_.hoverPopoverTool || ui_.tooltipBtn) {
        ui_.hoverPopoverTool = 0;
        ui_.popHover = -1;
        SetHoverButton(0);
        Redraw();
    }
    int ev = ls_session_mouse_move(session_, x, y, CurrentMods());
    Dispatch(ev | LS_EV_CURSOR);
}

void Overlay::OnButtonDown(int button, LPARAM lp, int clicks) {
    float x = LX(lp), y = LY(lp);
    if (!session_) return;
    if (opt_.colorPicker) {
        if (button == LS_BUTTON_LEFT) {
            host_->CopyColor(this, LS_ACTION_COPY_HEX);
            host_->Close(true);
        } else if (button == LS_BUTTON_RIGHT) {
            host_->Close(true);
        }
        return;
    }
    Layout();
    // 编辑文字时可以在样式面板里改样式（内核会再发一次 TEXT_BEGIN 更新输入框）；点别处只结束编辑
    bool inPopover = ui_.hasPopover && ui_.popoverRect.Contains(x, y);
    if (edit_ && !inPopover) {
        CommitText();
        return;
    }
    if (sizeW_) {
        EndSizeEdit(true);
        return;
    }
    if (recordPanel_ && ui_.hasRecordPanel && recordPanel_->Rect().Contains(x, y)) {
        if (button == LS_BUTTON_LEFT && recordPanel_->MouseDown(x, y) && recordPanel_->Pressed()) SetCapture(hwnd_);
        return;
    }
    if (button == LS_BUTTON_LEFT && inPopover) {
        int item = HitPopover(ui_.popover, x - ui_.popoverRect.x, y - ui_.popoverRect.y);
        if (item >= 0) {
            ui_.popPressed = item;
            if (ui_.popover.items[(size_t)item].kind == PK_SLIDER) {
                sliderDrag_ = true;
                SetCapture(hwnd_);
            }
            ApplyPopover(item, x - ui_.popoverRect.x, y - ui_.popoverRect.y, false);
        }
        return;
    }
    if (ui_.hasToolbar && ui_.toolbar.rect.Contains(x, y)) {
        if (button == LS_BUTTON_LEFT) {
            int id = HitToolbar(ui_.toolbar, x, y);
            ls_session_state st;
            ls_session_get_state(session_, &st);
            if (id && ToolbarEnabled(id, st)) {
                ui_.pressedBtn = id;
                ui_.hoverBtn = id;
                SetCapture(hwnd_);
                Redraw();
            }
        }
        return;
    }
    ls_session_state st;
    ls_session_get_state(session_, &st);
    if (button == LS_BUTTON_LEFT && clicks == 1 && ui_.hasSizeTip && st.phase == LS_PHASE_SELECTED &&
        !st.dragging_selection && ui_.sizeTip.Contains(x, y)) {
        BeginSizeEdit();
        return;
    }
    sessionDown_ = true;
    downButton_ = button;
    SetCapture(hwnd_);
    int ev = ls_session_mouse_down(session_, x, y, button, CurrentMods(), clicks);
    Dispatch(ev);
}

void Overlay::OnButtonUp(int button, LPARAM lp) {
    float x = LX(lp), y = LY(lp);
    if (recordPanel_ && recordPanel_->Pressed() && button == LS_BUTTON_LEFT) {
        std::shared_ptr<rec::SetupPanel> keep = recordPanel_;
        keep->MouseUp(x, y);  // 可能开始录制并关闭截图层
        if (GetCapture() == hwnd_) ReleaseCapture();
        return;
    }
    if (sliderDrag_ && button == LS_BUTTON_LEFT) {
        sliderDrag_ = false;
        ui_.popPressed = -1;
        ReleaseCapture();
        if (ui_.popoverTool) {
            ls_style st;
            ls_session_get_style(session_, ui_.popoverTool, &st);
            style::ToSettings(ui_.popoverTool, st);
        }
        return;
    }
    if (ui_.pressedBtn && button == LS_BUTTON_LEFT) {
        int id = ui_.pressedBtn;
        ui_.pressedBtn = 0;
        ReleaseCapture();
        bool inside = HitToolbar(ui_.toolbar, x, y) == id;
        Redraw();
        if (inside) OnToolbarButton(id);  // 可能关闭截图层，之后不能再访问成员
        return;
    }
    if (sessionDown_ && button == downButton_) {
        sessionDown_ = false;
        downButton_ = -1;
        if (GetCapture() == hwnd_) ReleaseCapture();
        int ev = ls_session_mouse_up(session_, x, y, button, CurrentMods());
        Dispatch(ev);
    }
}

void Overlay::OnToolbarButton(int id) {
    if (id >= 1 && id <= 10) {
        ui_.hoverPopoverTool = 0;
        Dispatch(ls_session_set_tool(session_, id) | LS_EV_REDRAW);
        return;
    }
    switch (id) {
    case TB_UNDO: Dispatch(ls_session_undo(session_) | LS_EV_REDRAW); break;
    case TB_REDO: Dispatch(ls_session_redo(session_) | LS_EV_REDRAW); break;
    case TB_LONGSHOT: host_->HandleAction(this, LS_ACTION_LONGSHOT); break;
    case TB_RECORD: host_->HandleAction(this, LS_ACTION_RECORD); break;
    case TB_OCR: host_->HandleAction(this, LS_ACTION_OCR); break;
    case TB_TRANSLATE: host_->HandleAction(this, LS_ACTION_TRANSLATE); break;
    case TB_PIN: host_->HandleAction(this, LS_ACTION_PIN); break;
    case TB_SAVE: host_->HandleAction(this, LS_ACTION_SAVE); break;
    case TB_CANCEL: host_->HandleAction(this, LS_ACTION_CANCEL); break;
    case TB_DONE: host_->HandleAction(this, LS_ACTION_DONE); break;
    }
}

void Overlay::ApplyPopover(int item, float x, float y, bool dragging) {
    int tool = ui_.popoverTool;
    if (!tool || item < 0 || item >= (int)ui_.popover.items.size()) return;
    const PopItem &it = ui_.popover.items[(size_t)item];
    if (it.kind == PK_CUSTOM) {
        if (!dragging) PickCustomColor(tool);
        return;
    }
    ls_style st;
    ls_session_get_style(session_, tool, &st);
    if (!PopoverApply(ui_.popover, item, x, y, st, dragging)) return;
    int ev = ls_session_set_style(session_, tool, &st);
    if (!dragging || it.kind != PK_SLIDER) style::ToSettings(tool, st);
    Dispatch(ev | LS_EV_REDRAW);
}

void Overlay::PickCustomColor(int tool) {
    ls_style st;
    ls_session_get_style(session_, tool, &st);
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc;
    std::memset(&cc, 0, sizeof cc);
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = hwnd_;
    cc.rgbResult = ToColorRef(st.color);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    st.color = ls_color{GetRValue(cc.rgbResult), GetGValue(cc.rgbResult), GetBValue(cc.rgbResult), 255};
    int ev = ls_session_set_style(session_, tool, &st);
    style::ToSettings(tool, st);
    Dispatch(ev | LS_EV_REDRAW);
}

void Overlay::Dispatch(int ev) {
    if (!session_) return;
    if (ev & LS_EV_TEXT_END) {
        if (edit_) {
            HWND e = edit_;
            edit_ = nullptr;
            DestroyWindow(e);
        }
    }
    if (ev & LS_EV_TEXT_BEGIN) BeginTextEditor();
    ls_session_state st;
    ls_session_get_state(session_, &st);
    bool selectionFinished = st.phase == LS_PHASE_SELECTED && st.has_selection && !st.dragging_selection &&
                             (prevPhase_ != LS_PHASE_SELECTED || prevDragging_);
    prevPhase_ = st.phase;
    prevDragging_ = st.dragging_selection;
    if ((ev & LS_EV_SELECTION) || st.dragging_selection) {
        if (sizeW_) EndSizeEdit(false);
        if (st.has_selection) host_->OnSelectionActivity(this);
    }
    if (ev & LS_EV_TOOL) {
        // 滚轮等改了样式：写回设置（「下一个」序号不写回）
        if (st.tool >= 1 && st.tool <= 10) {
            ls_style s;
            ls_session_get_style(session_, st.tool, &s);
            style::ToSettings(st.tool, s);
        }
    }
    if (ev & LS_EV_CURSOR) {
        float x = st.mouse_x, y = st.mouse_y;
        if (!(ui_.pressedBtn || sliderDrag_) && !(InUiZone(x, y) && !sessionDown_)) SetCursorShape(SysCursor(st.cursor));
    }
    if (ev & (LS_EV_REDRAW | LS_EV_SELECTION | LS_EV_TOOL | LS_EV_HISTORY | LS_EV_TEXT_BEGIN | LS_EV_TEXT_END)) Redraw();
    if (edit_ && !(ev & LS_EV_TEXT_BEGIN)) LayoutTextEditor();
    // 动作最后处理：可能关闭全部截图层
    if (ev & LS_EV_ACTION) host_->ProcessActions(this);
    if (selectionFinished) host_->OnSelectionFinished(this);
}

void Overlay::HandleKey(bool down, WPARAM vk, LPARAM lp) {
    if (!session_) return;
    int mods = CurrentMods();
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) {
        if (vk == VK_MENU) host_->OnAltKey(this, down);
        Dispatch(ls_session_modifiers(session_, mods));
        return;
    }
    if (down) host_->OnOtherKey();
    int key = MapKey(vk, lp);
    if (!key) return;
    if (opt_.colorPicker) {
        // 取色：Esc 退出，只转发复制颜色的三个键
        if (down && key == LS_KEY_ESCAPE) {
            host_->Close(true);
            return;
        }
        if (key != hotkeys::KeyCode(hotkeys::CaptureKey("copyRGB")) && key != hotkeys::KeyCode(hotkeys::CaptureKey("copyHEX")) &&
            key != hotkeys::KeyCode(hotkeys::CaptureKey("copyCustom")))
            return;
    }
    int ev = down ? ls_session_key_down(session_, key, mods) : ls_session_key_up(session_, key, mods);
    Dispatch(ev);
}

// ---------------------------------------------------------------- 文字输入（spec 2.6）

LRESULT CALLBACK Overlay::EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    HWND parent = GetParent(h);
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (m) {
    case WM_KEYDOWN:
        if (id == ID_TEXT_EDIT) {
            if (w == VK_ESCAPE || (w == VK_RETURN && ctrl)) {
                PostMessageW(parent, WM_LS_END_TEXT, END_TEXT, (LPARAM)h);
                return 0;
            }
        } else {
            if (w == VK_RETURN) {
                PostMessageW(parent, WM_LS_END_TEXT, APPLY_SIZE, (LPARAM)h);
                return 0;
            }
            if (w == VK_ESCAPE) {
                PostMessageW(parent, WM_LS_END_TEXT, END_SIZE, (LPARAM)h);
                return 0;
            }
            if (w == VK_TAB) {
                PostMessageW(parent, WM_LS_END_TEXT, SWITCH_SIZE, (LPARAM)h);
                return 0;
            }
        }
        break;
    case WM_CHAR:
        if (w == VK_ESCAPE) return 0;
        if (id == ID_TEXT_EDIT && ctrl && (w == 10 || w == 13)) return 0;
        if (id != ID_TEXT_EDIT && (w == 13 || w == 9)) return 0;
        break;
    case WM_KILLFOCUS:
        PostMessageW(parent, WM_LS_END_TEXT, id == ID_TEXT_EDIT ? END_TEXT : END_SIZE, (LPARAM)h);
        break;
    case WM_NCDESTROY: RemoveWindowSubclass(h, &Overlay::EditProc, id); break;
    }
    (void)ref;
    return DefSubclassProc(h, m, w, l);
}

void Overlay::BeginTextEditor() {
    ls_text_edit te;
    std::memset(&te, 0, sizeof te);
    if (!ls_session_text_edit(session_, &te)) return;
    float s = shot_->scale;
    if (edit_) {
        // 编辑中改了样式：更新已打开的输入框（字体、颜色、底色），不重开
        HFONT oldFont = editFont_;
        editFont_ = CreateFontW(-(int)std::lround(te.font_size * s), 0, 0, 0, te.bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE,
                                FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, gfx::FontFamily().c_str());
        SendMessageW(edit_, WM_SETFONT, (WPARAM)editFont_, FALSE);
        SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        if (oldFont) DeleteObject(oldFont);
        editColor_ = te.color;
        HBRUSH oldBrush = editBrush_;
        if (te.background.a > 0) {
            editBrush_ = CreateSolidBrush(ToColorRef(te.background));
            editPattern_ = false;
        } else {
            editBrush_ = CreatePatternBrush(shot_->dib);
            editPattern_ = true;
        }
        if (oldBrush) DeleteObject(oldBrush);
        LayoutTextEditor();
        InvalidateRect(edit_, nullptr, TRUE);
        SetFocus(edit_);
        return;
    }
    if (editFont_) DeleteObject(editFont_);
    editFont_ = CreateFontW(-(int)std::lround(te.font_size * s), 0, 0, 0, te.bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, gfx::FontFamily().c_str());
    editColor_ = te.color;
    if (editBrush_) DeleteObject(editBrush_);
    // 透明背景：用冻结画面做图案画刷并对齐画刷原点；底色样式用底色
    if (te.background.a > 0) {
        editBrush_ = CreateSolidBrush(ToColorRef(te.background));
        editPattern_ = false;
    } else {
        editBrush_ = CreatePatternBrush(shot_->dib);
        editPattern_ = true;
    }
    edit_ = CreateWindowExW(0, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOHSCROLL | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10,
                            10, hwnd_, (HMENU)(INT_PTR)ID_TEXT_EDIT, app::Instance(), nullptr);
    if (!edit_) return;
    SetWindowSubclass(edit_, &Overlay::EditProc, ID_TEXT_EDIT, (DWORD_PTR)this);
    SendMessageW(edit_, WM_SETFONT, (WPARAM)editFont_, FALSE);
    SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    ImmAssociateContextEx(edit_, nullptr, IACE_DEFAULT);
    std::wstring text = ReplaceAll(Widen(std::string(te.text ? te.text : "")), L"\n", L"\r\n");
    editUpdating_ = true;
    SetWindowTextW(edit_, text.c_str());
    editUpdating_ = false;
    LayoutTextEditor();
    SetFocus(edit_);
    int len = GetWindowTextLengthW(edit_);
    SendMessageW(edit_, EM_SETSEL, (WPARAM)len, (LPARAM)len);
}

void Overlay::LayoutTextEditor() {
    if (!edit_) return;
    ls_text_edit te;
    std::memset(&te, 0, sizeof te);
    if (!ls_session_text_edit(session_, &te)) return;
    float s = shot_->scale;
    int x = (int)std::floor(te.rect.x * s), y = (int)std::floor(te.rect.y * s);
    // 留出一个字宽，输入下一个字之前不会横向滚动
    int w = (int)std::ceil((te.rect.w + te.font_size * 1.2f) * s) + 2;
    int h = (int)std::ceil(te.rect.h * s) + 4;
    RECT cur;
    GetWindowRect(edit_, &cur);
    MapWindowPoints(nullptr, hwnd_, (POINT *)&cur, 2);
    if (cur.left != x || cur.top != y || cur.right - cur.left != w || cur.bottom - cur.top != h) {
        MoveWindow(edit_, x, y, w, h, TRUE);
        SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    }
}

void Overlay::OnTextChanged() {
    if (!edit_ || editUpdating_) return;
    int len = GetWindowTextLengthW(edit_);
    std::wstring w((size_t)len + 1, L'\0');
    GetWindowTextW(edit_, &w[0], len + 1);
    w.resize((size_t)len);
    std::string utf8 = Narrow(ReplaceAll(w, L"\r\n", L"\n"));
    int ev = ls_session_set_text(session_, utf8.c_str());
    Dispatch(ev | LS_EV_REDRAW);
    LayoutTextEditor();
    InvalidateRect(edit_, nullptr, TRUE);
}

void Overlay::CommitText() {
    if (!edit_) return;
    OnTextChanged();
    HWND e = edit_;
    edit_ = nullptr;
    DestroyWindow(e);
    SetFocus(hwnd_);
    Dispatch(ls_session_end_text(session_) | LS_EV_REDRAW);
}

// ---------------------------------------------------------------- 尺寸提示编辑

void Overlay::BeginSizeEdit() {
    ui_.sizeEditing = true;
    Layout();
    if (!ui_.hasSizeTip) {
        ui_.sizeEditing = false;
        return;
    }
    float s = shot_->scale;
    if (!sizeFont_)
        sizeFont_ = CreateFontW(-(int)std::lround(12 * s), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, gfx::FontFamily().c_str());
    if (!sizeBrush_) sizeBrush_ = CreateSolidBrush(RGB(58, 58, 60));
    int x, y, w, h;
    ls_session_selection_pixels(session_, &x, &y, &w, &h);
    auto make = [&](RectF r, int id, int value) {
        HWND e = CreateWindowExW(0, L"EDIT", FormatW(L"%d", value).c_str(), WS_CHILD | WS_VISIBLE | ES_CENTER | ES_NUMBER |
                                 ES_AUTOHSCROLL, (int)std::floor(r.x * s), (int)std::floor((r.y + 2) * s),
                                 (int)std::ceil(r.w * s), (int)std::ceil((r.h - 4) * s), hwnd_, (HMENU)(INT_PTR)id,
                                 app::Instance(), nullptr);
        if (!e) return e;
        SetWindowSubclass(e, &Overlay::EditProc, (UINT_PTR)id, (DWORD_PTR)this);
        SendMessageW(e, WM_SETFONT, (WPARAM)sizeFont_, FALSE);
        SendMessageW(e, EM_LIMITTEXT, 5, 0);
        return e;
    };
    sizeW_ = make(ui_.sizeEditW, ID_SIZE_W, w);
    sizeH_ = make(ui_.sizeEditH, ID_SIZE_H, h);
    if (sizeW_) {
        SetFocus(sizeW_);
        SendMessageW(sizeW_, EM_SETSEL, 0, -1);
    }
    Redraw();
}

void Overlay::EndSizeEdit(bool apply) {
    if (!sizeW_ && !sizeH_) {
        ui_.sizeEditing = false;
        return;
    }
    int w = 0, h = 0;
    if (apply && sizeW_ && sizeH_) {
        wchar_t buf[16];
        GetWindowTextW(sizeW_, buf, 16);
        w = _wtoi(buf);
        GetWindowTextW(sizeH_, buf, 16);
        h = _wtoi(buf);
    }
    HWND a = sizeW_, b = sizeH_;
    sizeW_ = sizeH_ = nullptr;
    if (a) DestroyWindow(a);
    if (b) DestroyWindow(b);
    ui_.sizeEditing = false;
    SetFocus(hwnd_);
    if (apply && w > 0 && h > 0) {
        ls_session_state st;
        ls_session_get_state(session_, &st);
        float s = shot_->scale;
        ls_rect r = {st.selection.x, st.selection.y, w / s, h / s};
        Dispatch(ls_session_set_selection(session_, r) | LS_EV_REDRAW);
    }
    Redraw();
}

void Overlay::DestroyEditors() {
    if (edit_) {
        HWND e = edit_;
        edit_ = nullptr;
        DestroyWindow(e);
    }
    if (sizeW_ || sizeH_) {
        HWND a = sizeW_, b = sizeH_;
        sizeW_ = sizeH_ = nullptr;
        if (a) DestroyWindow(a);
        if (b) DestroyWindow(b);
        ui_.sizeEditing = false;
    }
}

LRESULT Overlay::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED: return 0;  // 位置大小固定为该屏
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(cursor_ ? cursor_ : SysCursor(LS_CURSOR_CROSSHAIR));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: OnMouseMove(lp); return 0;
    case WM_MOUSELEAVE:
        tracking_ = false;
        mouseInside = false;
        if (recordPanel_) recordPanel_->MouseLeave();
        if (host_->MouseOverlay() == this) host_->SetMouseOverlay(nullptr);
        if (session_ && !sessionDown_) {
            ui_.hoverBtn = 0;
            ui_.popHover = -1;
            ui_.tooltipBtn = 0;
            Dispatch(ls_session_mouse_exit(session_) | LS_EV_REDRAW);
        }
        return 0;
    case WM_LBUTTONDOWN: OnButtonDown(LS_BUTTON_LEFT, lp, 1); return 0;
    case WM_LBUTTONDBLCLK: OnButtonDown(LS_BUTTON_LEFT, lp, 2); return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK: OnButtonDown(LS_BUTTON_RIGHT, lp, 1); return 0;
    case WM_MBUTTONDOWN: return 0;
    case WM_LBUTTONUP: OnButtonUp(LS_BUTTON_LEFT, lp); return 0;
    case WM_RBUTTONUP: OnButtonUp(LS_BUTTON_RIGHT, lp); return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd_) {
            if (sessionDown_ && session_) {
                sessionDown_ = false;
                Dispatch(ls_session_mouse_up(session_, lastX_, lastY_, downButton_, CurrentMods()));
                downButton_ = -1;
            }
            sliderDrag_ = false;
            ui_.pressedBtn = 0;
            if (recordPanel_ && recordPanel_->Pressed()) recordPanel_->MouseUp(-1e6f, -1e6f);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        // 滚轮消息发给有焦点的窗口：转给光标所在屏
        POINT pt = {MouseX(lp), MouseY(lp)};
        float dy = (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        for (Overlay *o : host_->Overlays())
            if (PtInRect(&o->Shot()->rc, pt)) o->Wheel(pt, dy);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) {
            host_->Close(true);
            return 0;
        }
        if (Overlay *t = host_->KeyTarget()) t->HandleKey(true, wp, lp);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (Overlay *t = host_->KeyTarget()) t->HandleKey(false, wp, lp);
        return 0;
    case WM_SYSCHAR:
    case WM_CHAR: return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_TIMER:
        if (wp == TIMER_HOVER) {
            KillTimer(hwnd_, TIMER_HOVER);
            ls_session_state st;
            ls_session_get_state(session_, &st);
            int id = ui_.hoverBtn;
            if (id >= 1 && id <= 10) {
                if (ToolbarEnabled(id, st)) ui_.hoverPopoverTool = id;
            } else if (id) {
                ui_.tooltipBtn = id;
            }
            Redraw();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_TEXT_EDIT && HIWORD(wp) == EN_CHANGE) OnTextChanged();
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        HWND e = (HWND)lp;
        if (e == edit_) {
            SetTextColor(dc, ToColorRef(editColor_));
            SetBkMode(dc, TRANSPARENT);
            if (editPattern_) {
                RECT r;
                GetWindowRect(e, &r);
                MapWindowPoints(nullptr, hwnd_, (POINT *)&r, 2);
                SetBrushOrgEx(dc, -r.left, -r.top, nullptr);
            }
            return (LRESULT)editBrush_;
        }
        if (e == sizeW_ || e == sizeH_) {
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkColor(dc, RGB(58, 58, 60));
            return (LRESULT)sizeBrush_;
        }
        break;
    }
    case WM_LS_END_TEXT: {
        HWND from = (HWND)lp;
        if (wp == END_TEXT) {
            if (edit_ && from == edit_) CommitText();
        } else if (wp == APPLY_SIZE) {
            EndSizeEdit(true);
        } else if (wp == SWITCH_SIZE) {
            HWND next = from == sizeW_ ? sizeH_ : sizeW_;
            if (next) {
                SetFocus(next);
                SendMessageW(next, EM_SETSEL, 0, -1);
            }
        } else if (wp == END_SIZE) {
            HWND f = GetFocus();
            if ((from == sizeW_ || from == sizeH_) && f != sizeW_ && f != sizeH_) EndSizeEdit(false);
        }
        return 0;
    }
    case WM_CLOSE: host_->Close(true); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !edit_ && !sizeW_) SetFocus(hwnd_);
        break;
    }
    return Window::Handle(msg, wp, lp);
}

// ---------------------------------------------------------------- Controller 实现

namespace {

bool Controller::Open(Mode m) {
    mode = m;
    toast::HideNow();
    bool withCursor = settings::Bool("capture.captureCursor") && m != Mode::ColorPicker;
    if (!TakeSnapshot(snap, withCursor)) {
        LS_LOG("capture: 冻结画面失败");
        return false;
    }
    int cursorIdx = 0;
    for (size_t i = 0; i < snap.monitors.size(); ++i)
        if (PtInRect(&snap.monitors[i]->rc, snap.cursor)) cursorIdx = (int)i;
    for (size_t i = 0; i < snap.monitors.size(); ++i) {
        MonitorShot *shot = snap.monitors[i].get();
        ls_rect initial;
        bool hasInitial = false;
        if (m == Mode::LastArea) {
            const Json *la = settings::State().Find("lastArea");
            const Json *mine = la ? la->Find(Narrow(shot->device)) : nullptr;
            if (mine && mine->IsArray() && mine->Size() == 4) {
                initial = ls_rect{(float)mine->At(0).AsNumber(), (float)mine->At(1).AsNumber(),
                                  (float)mine->At(2).AsNumber(), (float)mine->At(3).AsNumber()};
                hasInitial = initial.w > 0 && initial.h > 0;
            }
        } else if (m == Mode::Preset && (int)i == cursorIdx) {
            float w = settings::Int("capture.presetWidth") / shot->scale;
            float h = settings::Int("capture.presetHeight") / shot->scale;
            if (w > shot->lw) w = shot->lw;
            if (h > shot->lh) h = shot->lh;
            float cx = (snap.cursor.x - shot->rc.left) / shot->scale, cy = (snap.cursor.y - shot->rc.top) / shot->scale;
            float x = Clamp(cx - w / 2, 0.f, shot->lw - w), y = Clamp(cy - h / 2, 0.f, shot->lh - h);
            // 对齐到像素
            float px = 1.f / shot->scale;
            x = std::floor(x / px + 0.5f) * px;
            y = std::floor(y / px + 0.5f) * px;
            initial = ls_rect{x, y, w, h};
            hasInitial = true;
        }
        std::unique_ptr<Overlay> o(new Overlay(this, shot, m == Mode::ColorPicker ? 1 : (m == Mode::Record ? 3 : 0)));
        if (!o->Open(snap.windows, hasInitial ? &initial : nullptr)) continue;
        overlays.push_back(std::move(o));
    }
    if (overlays.empty()) return false;
    // 显示：先显示其他屏，最后显示光标所在屏并让它拿到前台与键盘焦点
    Overlay *focus = nullptr;
    for (auto &o : overlays)
        if (PtInRect(&o->Shot()->rc, snap.cursor)) focus = o.get();
    if (!focus) focus = overlays[0].get();
    for (auto &o : overlays) {
        if (o.get() == focus) continue;
        ShowWindow(o->Hwnd(), SW_SHOWNOACTIVATE);
        UpdateWindow(o->Hwnd());
    }
    ShowWindow(focus->Hwnd(), SW_SHOW);
    UpdateWindow(focus->Hwnd());
    // 截图层拿前台：挂到当前前台线程的输入队列上再 SetForegroundWindow
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD me = GetCurrentThreadId();
    bool attached = fgThread && fgThread != me && AttachThreadInput(fgThread, me, TRUE);
    SetForegroundWindow(focus->Hwnd());
    SetActiveWindow(focus->Hwnd());
    SetFocus(focus->Hwnd());
    if (attached) AttachThreadInput(fgThread, me, FALSE);
    mouseOverlay = focus;
    // 把当前光标位置喂给内核，立刻显示悬停与放大镜
    for (auto &o : overlays) {
        POINT pt = snap.cursor;
        ScreenToClient(o->Hwnd(), &pt);
        if (o.get() == focus) {
            o->mouseInside = true;
            o->Dispatch(ls_session_mouse_move(o->Session(), pt.x / o->Shot()->scale, pt.y / o->Shot()->scale, 0) |
                        LS_EV_CURSOR | LS_EV_REDRAW);
        }
    }
    return true;
}

void Controller::RestoreFocus() {
    if (settings::Bool("capture.returnToPreviousApp") && snap.foreground && IsWindow(snap.foreground))
        SetForegroundWindow(snap.foreground);
}

void Controller::Close(bool restoreFocus) {
    if (closed) return;
    closed = true;
    for (auto &o : overlays) o->Hide();
    if (restoreFocus) RestoreFocus();
    app::Defer(&DeleteController, this);
}

Overlay *Controller::KeyTarget() {
    for (auto &o : overlays) {
        ls_session_state st;
        ls_session_get_state(o->Session(), &st);
        if (st.has_selection) return o.get();
    }
    if (mouseOverlay) return mouseOverlay;
    return overlays.empty() ? nullptr : overlays[0].get();
}

void Controller::OnSelectionActivity(Overlay *active) {
    // 只有一块屏可以有选区：其他屏重置为无悬停、整屏遮罩
    for (auto &o : overlays) {
        if (o.get() == active) continue;
        ls_session_state st;
        ls_session_get_state(o->Session(), &st);
        if (st.has_selection || st.has_hover) {
            o->ResetSession();
            ls_session_mouse_exit(o->Session());
            o->Redraw();
        }
    }
}

void Controller::ProcessActions(Overlay *o) {
    for (int guard = 0; guard < 16 && !closed; ++guard) {
        ls_action a = ls_session_take_action(o->Session());
        if (a == LS_ACTION_NONE) break;
        HandleAction(o, a);
    }
}

void Controller::SaveLastArea(Overlay *o) {
    ls_session_state st;
    ls_session_get_state(o->Session(), &st);
    if (!st.has_selection || st.window_count > 0) return;
    Json &state = settings::State();
    Json *la = state.Find("lastArea");
    if (!la || !la->IsObject()) la = &state.Set("lastArea", Json::MakeObject());
    Json arr = Json::MakeArray();
    arr.Push(Json::MakeNumber(st.selection.x));
    arr.Push(Json::MakeNumber(st.selection.y));
    arr.Push(Json::MakeNumber(st.selection.w));
    arr.Push(Json::MakeNumber(st.selection.h));
    la->Set(Narrow(o->Shot()->device), arr);
    settings::SaveState();
}

bool Controller::RenderImage(Overlay *o, ls_image &img, POINT &origin) {
    ls_session_state st;
    ls_session_get_state(o->Session(), &st);
    if (st.window_count > 0) return ComposeWindows(o, img, origin);
    int x, y, w, h;
    ls_session_selection_pixels(o->Session(), &x, &y, &w, &h);
    origin.x = o->Shot()->rc.left + x;
    origin.y = o->Shot()->rc.top + y;
    return exporter::RenderSelection(o->Session(), o->Shot()->img, o->Shot()->scale, img);
}

// 多窗口：各窗口单独截图（同单窗口方式），按原位置合成在外接矩形内，透明背景
bool Controller::ComposeWindows(Overlay *o, ls_image &img, POINT &origin) {
    MonitorShot *shot = o->Shot();
    int64_t ids[64];
    int n = ls_session_selected_windows(o->Session(), ids, 64);
    if (n > 64) n = 64;
    int sx, sy, sw, sh;
    ls_session_selection_pixels(o->Session(), &sx, &sy, &sw, &sh);
    float scale = shot->scale;
    bool corners = settings::Bool("capture.multiWindowCorners");
    bool shadow = settings::Bool("capture.multiWindowShadow");
    float radius = corners ? settings::Num("capture.windowCornerRadius") * scale : 0;
    float sr = shadow ? settings::Num("capture.windowShadowRadius") * scale : 0;
    float soff = shadow ? (float)std::lround(settings::Num("capture.windowShadowRadius") / 3) * scale : 0;
    ls_color sc = style::ToLs(ParseColor(settings::Str("capture.windowShadowColor"), Color{0, 0, 0, 0.4f}));
    int pad = sr > 0 ? (int)std::ceil(sr + std::fabs(soff)) : 0;
    img = ls_image_alloc(sw + 2 * pad, sh + 2 * pad);
    if (!img.data) return false;
    origin.x = shot->rc.left + sx - pad;
    origin.y = shot->rc.top + sy - pad;
    // z 序：从最下层画到最上层
    struct Item {
        HWND hwnd;
        int z;
    };
    std::vector<Item> items;
    for (int i = 0; i < n; ++i) {
        HWND h = (HWND)(intptr_t)ids[i];
        int z = 0;
        for (size_t k = 0; k < snap.windows.size(); ++k)
            if (snap.windows[k].hwnd == h) z = (int)k;
        items.push_back({h, z});
    }
    for (size_t a = 0; a < items.size(); ++a)
        for (size_t b = a + 1; b < items.size(); ++b)
            if (items[b].z > items[a].z) std::swap(items[a], items[b]);
    for (auto &it : items) {
        ls_image win;
        RECT frame;
        if (!CaptureWindowImage(it.hwnd, win, &frame, false)) {
            // PrintWindow 失败：从冻结画面裁
            frame = WindowFrameRect(it.hwnd);
            RECT in;
            if (!IntersectRect(&in, &frame, &shot->rc)) continue;
            frame = in;
            if (ls_image_crop(&shot->img, frame.left - shot->rc.left, frame.top - shot->rc.top, frame.right - frame.left,
                              frame.bottom - frame.top, &win) != 0)
                continue;
        }
        ls_image framed;
        int fpad = 0;
        if ((radius > 0 || sr > 0) && ls_image_frame(&win, radius, sr, soff, sc, &framed) == 0 && framed.data) {
            fpad = (framed.width - win.width) / 2;
            ls_image_free(&win);
            win = framed;
        }
        int dx = frame.left - shot->rc.left - sx + pad - fpad;
        int dy = frame.top - shot->rc.top - sy + pad - fpad;
        ls_image_draw(&img, &win, dx, dy);
        ls_image_free(&win);
    }
    return true;
}

void Controller::Output(Overlay *o, int kind) {
    if (kind == exporter::OUT_NONE) return;
    ls_image img;
    POINT origin = {0, 0};
    float scale = o->Shot()->scale;
    bool ok = RenderImage(o, img, origin);
    SaveLastArea(o);
    bool ask = settings::Bool("save.askLocation") &&
               (kind == exporter::OUT_SAVE || kind == exporter::OUT_BOTH || kind == exporter::OUT_SAVE_OPEN);
    Close(!ask);
    if (ok) exporter::Deliver(img, scale, kind, origin);
    if (ask) RestoreFocus();
}

void Controller::CopyColor(Overlay *o, ls_action a) {
    ls_session_state st;
    ls_session_get_state(o->Session(), &st);
    ls_color c = ls_session_pixel_at(o->Session(), st.mouse_x, st.mouse_y);
    // 色彩空间：Windows 视屏幕为 sRGB
    std::string space = settings::Str("color.space");
    uint8_t r = c.r, g = c.g, b = c.b;
    int target = -1;
    if (space == "p3") target = LS_SPACE_P3;
    else if (space == "adobe") target = LS_SPACE_ADOBE;
    else if (space == "generic") target = LS_SPACE_GENERIC;
    if (target >= 0) {
        float in[3] = {r / 255.f, g / 255.f, b / 255.f}, out[3];
        ls_color_convert(LS_SPACE_SRGB, target, in, out);
        r = (uint8_t)Clamp((int)std::lround(out[0] * 255), 0, 255);
        g = (uint8_t)Clamp((int)std::lround(out[1] * 255), 0, 255);
        b = (uint8_t)Clamp((int)std::lround(out[2] * 255), 0, 255);
    }
    bool upper = settings::Bool("color.hexUppercase");
    char buf[256] = {0};
    if (a == LS_ACTION_COPY_RGB) {
        snprintf(buf, sizeof buf, "%d, %d, %d", r, g, b);
    } else if (a == LS_ACTION_COPY_HEX) {
        if (ls_color_hex(r, g, b, settings::Bool("color.hexPrefix") ? 1 : 0, upper ? 1 : 0, buf, sizeof buf) <= 0)
            snprintf(buf, sizeof buf, "#%02X%02X%02X", r, g, b);
    } else {
        ls_color_format(settings::Str("color.customFormat").c_str(), r, g, b, upper ? 1 : 0, buf, sizeof buf);
    }
    std::wstring value = Widen(std::string(buf));
    CopyTextToClipboard(app::MainHwnd(), value);
    toast::Show(i18n::T("capture.copiedColor", {{"value", value}}), "pipette");
}

void Controller::OnAltKey(Overlay *o, bool down) {
    double now = NowSeconds();
    if (down) {
        if (altClean) return;  // 按住不放的重复
        if (lastAltUp > 0 && now - lastAltUp < 0.35) {
            lastAltUp = 0;
            altClean = false;
            ls_session_state st;
            ls_session_get_state(o->Session(), &st);
            if (st.has_selection && st.phase == LS_PHASE_SELECTED)
                Output(o, exporter::OutputFromSetting(settings::Str("capture.doubleModifierAction")));
            return;
        }
        altClean = true;
    } else {
        if (altClean) lastAltUp = now;
        altClean = false;
    }
}

void Controller::HandleAction(Overlay *o, ls_action a) {
    if (closed) return;
    // 取色模式只认退出与复制颜色
    if (mode == Mode::ColorPicker && a != LS_ACTION_CANCEL && a != LS_ACTION_COPY_RGB && a != LS_ACTION_COPY_HEX &&
        a != LS_ACTION_COPY_CUSTOM)
        return;
    // 录屏模式（spec 8.1）：回车、V、双击选区或「开始录制」开始录制，Esc 取消，其余动作无效
    if (mode == Mode::Record) {
        if (a == LS_ACTION_CANCEL) {
            Close(true);
        } else if (a == LS_ACTION_DONE || a == LS_ACTION_RECORD || a == LS_ACTION_DOUBLE_CLICK) {
            ls_session_state st;
            ls_session_get_state(o->Session(), &st);
            if (!st.has_selection || st.window_count > 0) return;
            if (!record::CanStart()) return;  // 缺编码组件等：截图层保持打开，可以改选 GIF
            int x, y, w, h;
            ls_session_selection_pixels(o->Session(), &x, &y, &w, &h);
            RECT r = {o->Shot()->rc.left + x, o->Shot()->rc.top + y, o->Shot()->rc.left + x + w, o->Shot()->rc.top + y + h};
            HMONITOR mon = o->Shot()->mon;
            SaveLastArea(o);
            Close(true);
            record::StartWithRegion(mon, r);
        }
        return;
    }
    switch (a) {
    case LS_ACTION_CANCEL: Close(true); break;
    case LS_ACTION_DONE:
        if (PurposeAction() != LS_ACTION_NONE) {
            HandleAction(o, PurposeAction());
            break;
        }
        Output(o, exporter::OutputFromSetting(settings::Str("save.enterAction")));
        break;
    case LS_ACTION_DOUBLE_CLICK: Output(o, exporter::OutputFromSetting(settings::Str("capture.doubleClickAction"))); break;
    case LS_ACTION_COPY: Output(o, exporter::OUT_COPY); break;
    case LS_ACTION_SAVE: Output(o, exporter::OUT_SAVE); break;
    case LS_ACTION_PIN: Output(o, exporter::OUT_PIN); break;
    case LS_ACTION_OCR:
    case LS_ACTION_TRANSLATE: {
        ls_image img;
        POINT origin;
        float scale = o->Shot()->scale;
        bool ok = RenderImage(o, img, origin);
        int rx, ry, rw, rh;
        ls_session_selection_pixels(o->Session(), &rx, &ry, &rw, &rh);
        RECT region = {o->Shot()->rc.left + rx, o->Shot()->rc.top + ry, o->Shot()->rc.left + rx + rw, o->Shot()->rc.top + ry + rh};
        SaveLastArea(o);
        Close(true);
        if (!ok) break;
        if (a == LS_ACTION_OCR)
            ocr::Recognize(img, scale, &region);
        else
            translate::FromImage(img, scale);
        break;
    }
    case LS_ACTION_RECORD: {
        // 截图工具栏「录屏」/ V：原地换成录屏模式，沿用当前选区
        ls_session_state st;
        ls_session_get_state(o->Session(), &st);
        if (st.window_count > 0) break;
        mode = Mode::Record;
        for (auto &ov : overlays) ov->EnterRecordMode();
        break;
    }
    case LS_ACTION_LONGSHOT: {
        ls_session_state st;
        ls_session_get_state(o->Session(), &st);
        if (a == LS_ACTION_LONGSHOT && st.window_count > 0) {
            toast::Show(T("longshot.windowMode"), "triangle-alert");
            break;
        }
        int x, y, w, h;
        ls_session_selection_pixels(o->Session(), &x, &y, &w, &h);
        RECT r = {o->Shot()->rc.left + x, o->Shot()->rc.top + y, o->Shot()->rc.left + x + w, o->Shot()->rc.top + y + h};
        HMONITOR mon = o->Shot()->mon;
        SaveLastArea(o);
        Close(false);
        if (a == LS_ACTION_LONGSHOT)
            longshot::StartWithRegion(mon, r);
        else
            record::StartWithRegion(mon, r);
        break;
    }
    case LS_ACTION_COPY_RGB:
    case LS_ACTION_COPY_HEX:
    case LS_ACTION_COPY_CUSTOM: CopyColor(o, a); break;
    case LS_ACTION_LAST_AREA:
    default: break;
    }
}

// ---------------------------------------------------------------- 延时截图倒计时

class Countdown : public Window {
public:
    int remaining = 0;
    LayeredSurface surface;
    RECT mon = {};
    UINT dpi = 96;

    void Begin(int seconds) {
        remaining = seconds;
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(m, &mi);
        mon = mi.rcMonitor;
        dpi = api::MonitorDpi(m);
        if (!hwnd_ && !CreateWin(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                 WS_POPUP, 0, 0, 1, 1, nullptr, L"LemonShot"))
            return;
        api::ExcludeFromCapture(hwnd_);
        Render();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        Tick();
        SetTimer(hwnd_, 1, 1000, nullptr);
    }
    void Tick() {
        if (settings::Bool("capture.delaySound")) MessageBeep(MB_OK);
    }
    void Render() {
        float s = dpi / 96.f;
        float size = theme::Tok().recordBar.countdownSize;
        int px = (int)std::ceil(size * s);
        ID2D1RenderTarget *rt = surface.Begin(px, px, dpi);
        if (!rt) return;
        {
            Canvas c(rt);
            c.FillEllipse(size / 2, size / 2, size / 2, size / 2, theme::Cap().tipBg);
            TextStyle ts;
            ts.size = size * 0.45f;
            ts.weight = kSemibold;
            ts.color = Color{1, 1, 1, 1};
            ts.h = HAlign::Center;
            ts.v = VAlign::Center;
            ts.tabular = true;
            c.Text(FormatW(L"%d", remaining), RectF(0, 0, size, size), ts);
        }
        surface.End(hwnd_, (mon.left + mon.right) / 2 - px / 2, (mon.top + mon.bottom) / 2 - px / 2, 255);
    }
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        if (msg == WM_TIMER && wp == 1) {
            --remaining;
            if (remaining <= 0) {
                KillTimer(hwnd_, 1);
                ShowWindow(hwnd_, SW_HIDE);
                // 等倒计时窗口从屏幕上消失再冻结画面
                SetTimer(hwnd_, 2, 120, nullptr);
            } else {
                Tick();
                Render();
            }
            return 0;
        }
        if (msg == WM_TIMER && wp == 2) {
            KillTimer(hwnd_, 2);
            Start(Mode::Normal);
            return 0;
        }
        return Window::Handle(msg, wp, lp);
    }
};

Countdown *g_countdown = nullptr;

}  // namespace

void Start(Mode mode) {
    if (g_ctl && !g_ctl->closed) return;
    Controller *c = new Controller();
    g_ctl = c;
    if (!c->Open(mode)) {
        g_ctl = nullptr;
        delete c;
    }
}

bool IsActive() { return g_ctl && !g_ctl->closed; }

void Cancel() {
    if (g_ctl && !g_ctl->closed) g_ctl->Close(true);
}

void OnSettingsChanged() {
    if (!g_ctl || g_ctl->closed) return;
    for (auto &o : g_ctl->overlays) o->ReloadOptions();
}

static void DeliverEnter(ls_image img, float scale, POINT origin) {
    exporter::Deliver(img, scale, exporter::OutputFromSetting(settings::Str("save.enterAction")), origin);
}

void CaptureWindowUnderCursor() {
    if (IsActive()) return;
    POINT pt;
    GetCursorPos(&pt);
    HWND h = WindowFromPoint(pt);
    if (h) h = GetAncestor(h, GA_ROOT);
    wchar_t cls[64] = {};
    if (h) GetClassNameW(h, cls, 63);
    if (!h || wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) {
        CaptureFullscreen();
        return;
    }
    toast::HideNow();
    ls_image img;
    RECT frame;
    if (!CaptureWindowImage(h, img, &frame)) return;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    float scale = api::MonitorDpi(mon) / 96.f;
    bool corners = settings::Bool("capture.windowCorners");
    bool shadow = settings::Bool("capture.windowShadow");
    float radius = corners ? settings::Num("capture.windowCornerRadius") * scale : 0;
    float sr = shadow ? settings::Num("capture.windowShadowRadius") * scale : 0;
    float soff = shadow ? (float)std::lround(settings::Num("capture.windowShadowRadius") / 3) * scale : 0;
    POINT origin = {frame.left, frame.top};
    if (radius > 0 || sr > 0) {
        ls_color sc = style::ToLs(ParseColor(settings::Str("capture.windowShadowColor"), Color{0, 0, 0, 0.4f}));
        ls_image framed;
        if (ls_image_frame(&img, radius, sr, soff, sc, &framed) == 0 && framed.data) {
            int pad = (framed.width - img.width) / 2;
            origin.x -= pad;
            origin.y -= pad;
            ls_image_free(&img);
            img = framed;
        }
    }
    DeliverEnter(img, scale, origin);
}

void CaptureFullscreen() {
    if (IsActive()) return;
    toast::HideNow();
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, &mi);
    ls_image img;
    if (!CaptureScreenRect(mi.rcMonitor, settings::Bool("capture.captureCursor"), img)) return;
    POINT origin = {mi.rcMonitor.left, mi.rcMonitor.top};
    DeliverEnter(img, api::MonitorDpi(mon) / 96.f, origin);
}

void StartDelayed() {
    if (IsActive()) return;
    int seconds = settings::Int("capture.delaySeconds");
    if (seconds <= 0) {
        Start(Mode::Normal);
        return;
    }
    if (!g_countdown) g_countdown = new Countdown();
    g_countdown->Begin(seconds);
}

}  // namespace capture
}  // namespace ls
