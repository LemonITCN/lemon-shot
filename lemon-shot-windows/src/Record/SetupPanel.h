// 录屏设置面板（spec 8.1）：录屏模式的截图层里代替工具栏，放在选区下方（摆放同工具栏）。
// 麦克风 · 系统声音 · 摄像头 · 光标 · 点击 · 按键 | 格式 | 清晰度 | 帧率 | 开始录制。
// 改动立即写回 record.*；按住麦克风/摄像头开关 0.5 秒弹出设备选择。悬停 0.5 秒显示提示气泡。
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "UI/Gfx.h"

namespace ls {
namespace rec {

class SetupPanel {
public:
    // owner：截图层窗口（重画、弹出菜单定位用），自测时为空。scale：该屏像素/逻辑点
    SetupPanel(HWND owner, float scale, UINT dpi, std::function<void()> redraw, std::function<void()> start);
    ~SetupPanel();
    SetupPanel(const SetupPanel &) = delete;
    SetupPanel &operator=(const SetupPanel &) = delete;

    SizeF Size() const;
    // 截图层布局后给出面板位置（本屏逻辑坐标）；below：面板在选区下方（提示气泡放面板下方）
    void SetRect(RectF r, bool below);
    RectF Rect() const { return rect_; }
    void SetStartEnabled(bool on) { startEnabled_ = on; }
    void Draw(Canvas &c, float screenW, float screenH);

    // 鼠标（本屏逻辑坐标）。返回 true 表示事件在面板上
    bool MouseMove(float x, float y);
    void MouseLeave();
    bool MouseDown(float x, float y);
    void MouseUp(float x, float y);
    bool Pressed() const { return pressed_ >= 0; }
    bool MenuOpen() const { return menuOpen_; }

    // 自测：指定悬停项并立即显示提示
    void SetHoverForTest(int id) {
        hover_ = id;
        tooltip_ = id;
    }

private:
    struct Item {
        int id;
        RectF r;
    };
    enum { kMic = 0, kSystem, kCamera, kCursor, kClicks, kKeys, kFormat = 10, kQuality, kFps, kStart = 20 };
    void Layout();
    int Hit(float x, float y) const;
    bool Enabled(int id) const;
    bool ToggleOn(int id) const;
    const char *ToggleKey(int id) const;
    std::vector<std::wstring> ChipLabels(int id) const;
    std::vector<std::string> ChipValues(int id) const;
    int ChipIndex(int id) const;
    std::wstring Tooltip(int id) const;
    void Click(int id);
    void ShowChipMenu(int id);
    void ShowDeviceMenu(int id);
    void StartTimer(int which, UINT ms);
    void StopTimer(int which);
    static void CALLBACK TimerProc(HWND, UINT, UINT_PTR id, DWORD);
    void OnTimer(int which);
    void Redraw() {
        if (redraw_) redraw_();
    }

    HWND owner_;
    float scale_;
    UINT dpi_;
    std::function<void()> redraw_, start_;
    RectF rect_;
    bool below_ = true;
    std::vector<Item> items_;
    std::vector<RectF> seps_;
    float chipW_[3] = {0, 0, 0};
    float startW_ = 0;
    bool startEnabled_ = true;
    int hover_ = -1, pressed_ = -1, tooltip_ = -1, menuFor_ = -1;
    bool menuOpen_ = false;
    bool longFired_ = false;
    UINT_PTR timers_[2] = {0, 0};  // 0 提示延迟，1 长按
    bool mfAvailable_ = false;
};

}  // namespace rec
}  // namespace ls
