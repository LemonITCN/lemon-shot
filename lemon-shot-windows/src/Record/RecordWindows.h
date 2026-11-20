// 录屏时不进画面的窗口（spec 8.2）：控制条、区域边框、倒计时、GIF 进度、提示气泡。
// 全部是分层置顶工具窗口、不抢焦点；Win10 2004+ 用 WDA_EXCLUDEFROMCAPTURE 排除出画面，更早的系统由调用方把它们放在区域外。
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Record/RecordUI.h"
#include "UI/Gfx.h"
#include "UI/Window.h"

namespace ls {
namespace rec {

// 提示气泡（深色底），点击穿透
class TipWindow : public Window {
public:
    // anchor：按钮的屏幕像素矩形；below：放在按钮下方
    void Show(const std::wstring &text, RECT anchor, bool below, UINT dpi);
    void Hide();

private:
    LayeredSurface surface_;
};

// 控制条
class BarWindow : public Window {
public:
    BarView view;
    std::function<void(int)> onButton;
    int tipSide = 0;  // 提示气泡放哪边：1 下方，-1 上方，0 按屏幕位置（排除不了窗口的系统上要避开录制区域）

    bool Create(UINT dpi);
    bool Excluded() const { return excluded_; }  // 是否已排除出画面（Win10 2004+）
    SizeF BarSize() const { return view.Size(); }
    // 控制条本体左上角的屏幕像素位置（窗口四周另有阴影边距）
    void Place(int x, int y);
    POINT Position() const { return pos_; }
    void Show();
    void Hide();
    bool Visible() const { return visible_; }
    void SetAutoFade(bool on);
    void Render();
    void Close();

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    float Scale() const { return dpi_ / 96.f; }
    void SetHover(int id);
    void ScheduleFade();
    void SetAlpha(BYTE a);

    LayeredSurface surface_;
    TipWindow tip_;
    UINT dpi_ = 96;
    POINT pos_ = {};
    bool excluded_ = false;
    bool visible_ = false;
    bool autoFade_ = false;
    bool hovering_ = false;
    bool tracking_ = false;
    BYTE alpha_ = 255;
    double fadeStart_ = 0;
    int tipFor_ = BAR_NONE;
};

// 区域外侧的红色虚线边框：四条细窗口围住区域，点击穿透
class BorderWindows {
public:
    ~BorderWindows() { Close(); }
    void Show(RECT region, UINT dpi);
    void Close();

private:
    class Strip : public Window {
    public:
        LayeredSurface surface;
    };
    Strip strips_[4];
};

// 区域中央的倒计时（countdownSize 圆形 tipBg 底），Esc 取消（倒计时期间临时注册 Esc 全局热键）
class CountdownWindow : public Window {
public:
    std::function<void()> onDone, onCancel;
    bool Start(RECT region, UINT dpi, int seconds);
    void Close();

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void Render();
    LayeredSurface surface_;
    RECT region_ = {};
    UINT dpi_ = 96;
    int remaining_ = 0;
    bool hotkey_ = false;
    bool excluded_ = false;
};

// GIF 编码进度：屏幕底部居中（与 toast 同一位置）
class ProgressWindow : public Window {
public:
    bool Show(HMONITOR mon, const std::wstring &text);
    void SetProgress(double v);
    void Close();

private:
    void Render();
    LayeredSurface surface_;
    std::wstring text_;
    double progress_ = 0;
    UINT dpi_ = 96;
    POINT pos_ = {};
};

}  // namespace rec
}  // namespace ls
