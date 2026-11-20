// 截图层窗口（每块屏一个；贴图标注也复用它）：转发事件给内核会话，按内核数据绘制，处理文字输入与工具栏。
#pragma once

#include <windows.h>

#include <memory>
#include <vector>

#include "Capture/CaptureView.h"
#include "Capture/Screen.h"
#include "UI/Window.h"
#include "lemonshot.h"

namespace ls {
namespace capture {

class Overlay;

// 截图层的宿主：截图控制器、贴图标注各实现一份
class OverlayHost {
public:
    virtual ~OverlayHost() = default;
    virtual std::vector<Overlay *> Overlays() = 0;
    virtual Overlay *MouseOverlay() = 0;
    virtual void SetMouseOverlay(Overlay *o) = 0;
    virtual Overlay *KeyTarget() = 0;
    virtual void OnSelectionActivity(Overlay *) {}
    virtual void ProcessActions(Overlay *o) = 0;
    virtual void HandleAction(Overlay *o, ls_action a) = 0;
    virtual void CopyColor(Overlay *, ls_action) {}
    virtual void Close(bool restoreFocus) = 0;
    virtual void OnAltKey(Overlay *, bool) {}
    virtual void OnOtherKey() {}
    // 选区刚确定（拖完、单击窗口、F 全屏等）
    virtual void OnSelectionFinished(Overlay *) {}
};


class Overlay : public Window {
public:
    // mode：0 截图，1 取色，2 贴图标注（lock_selection，工具栏贴在图下方，窗口区域裁到图片与工具栏），3 录屏
    Overlay(OverlayHost *host, MonitorShot *shot, int mode) : host_(host), shot_(shot) {
        opt_ = CaptureOptions::FromSettings();
        opt_.colorPicker = mode == 1;
        opt_.pinMode = mode == 2;
        opt_.recordMode = mode == 3;
    }
    ~Overlay() override;

    bool Open(const std::vector<WindowEntry> &windows, const ls_rect *initial);
    void ResetSession();
    ls_session *Session() const { return session_; }
    MonitorShot *Shot() const { return shot_; }
    void Redraw() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }
    void Hide() {
        DestroyEditors();
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    }
    void HandleKey(bool down, WPARAM vk, LPARAM lp);
    void Dispatch(int ev);
    void ReloadOptions() {
        bool cp = opt_.colorPicker, pm = opt_.pinMode, rm = opt_.recordMode;
        opt_ = CaptureOptions::FromSettings();
        opt_.colorPicker = cp;
        opt_.pinMode = pm;
        opt_.recordMode = rm;
        Redraw();
    }
    bool ColorPicker() const { return opt_.colorPicker; }
    // 截图工具栏「录屏」：原地换成录屏模式（保留选区，清掉标注，工具栏换成录屏设置面板）
    void EnterRecordMode();
    bool Editing() const { return edit_ || sizeW_; }
    void Wheel(POINT screen, float dy);
    // 贴图标注：把当前会话（图片 + 标注）画成像素图
    bool RenderResult(ls_image &out);
    bool mouseInside = false;

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void BuildSession(const ls_rect *initial);
    void Paint();
    void UpdateRegion();
    CaptureFrame Frame() {
        CaptureFrame f;
        f.session = session_;
        f.frozen = &frozen_;
        f.scale = shot_->scale;
        f.width = shot_->lw;
        f.height = shot_->lh;
        f.patches = &patches_;
        f.opt = opt_;
        return f;
    }
    void Layout() {
        CaptureFrame f = Frame();
        LayoutCaptureUi(f, ui_);
    }
    float LX(LPARAM lp) const { return MouseX(lp) / shot_->scale; }
    float LY(LPARAM lp) const { return MouseY(lp) / shot_->scale; }
    void SetCursorShape(HCURSOR c) {
        cursor_ = c;
        if (mouseInside) SetCursor(c);
    }
    bool InUiZone(float x, float y);
    void OnMouseMove(LPARAM lp);
    void OnButtonDown(int button, LPARAM lp, int clicks);
    void OnButtonUp(int button, LPARAM lp);
    void OnToolbarButton(int id);
    void ApplyPopover(int item, float x, float y, bool dragging);
    void PickCustomColor(int tool);
    void SetHoverButton(int id);
    // 文字输入
    void BeginTextEditor();
    void LayoutTextEditor();
    void CommitText();
    void OnTextChanged();
    // 尺寸提示编辑
    void BeginSizeEdit();
    void EndSizeEdit(bool apply);
    void DestroyEditors();
    static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref);

    OverlayHost *host_;
    MonitorShot *shot_;
    ls_session *session_ = nullptr;
    std::vector<ls_window> windows_;
    CaptureOptions opt_;
    CaptureUi ui_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    TiledBitmap frozen_;
    PatchCache patches_;
    HRGN lastRegion_ = nullptr;
    HCURSOR cursor_ = nullptr;
    bool tracking_ = false;
    bool sessionDown_ = false;
    int downButton_ = -1;
    bool sliderDrag_ = false;
    float lastX_ = 0, lastY_ = 0;
    // 文字输入框
    HWND edit_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    bool editPattern_ = false;
    ls_color editColor_ = {0, 0, 0, 255};
    bool editUpdating_ = false;
    // 尺寸输入框
    HWND sizeW_ = nullptr, sizeH_ = nullptr;
    HFONT sizeFont_ = nullptr;
    HBRUSH sizeBrush_ = nullptr;
    // 上一次分发时的状态（判断「选区刚确定」）
    int prevPhase_ = -1;
    int prevDragging_ = 0;
    // 录屏模式的设置面板
    std::shared_ptr<rec::SetupPanel> recordPanel_;
    void CreateRecordPanel();
};


}  // namespace capture
}  // namespace ls
