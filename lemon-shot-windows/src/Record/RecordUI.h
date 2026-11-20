// 录屏/录音的自绘界面（spec 8.1、8.2、10）：控制条、按键浮层、GIF 进度、通用小控件。
// 只负责「量」与「画」（逻辑点），屏幕窗口与自测共用；交互状态由窗口维护。尺寸颜色全部来自 tokens.json。
#pragma once

#include <string>
#include <vector>

#include "UI/Gfx.h"

namespace ls {
namespace rec {

// tokens.json 里 Theme.h 没有收录的两段：keysOverlay、clickOverlay
struct OverlayTokens {
    struct KeySize {
        float fontSize, padding, radius;
    };
    KeySize small{16, 8, 8}, medium{22, 10, 10}, large{30, 14, 12};
    Color lightBg, lightText, darkBg, darkText;
    float margin = 48, gap = 8;
    float clickRadius = 18;
    Color clickColor, clickRightColor;
    float clickMs = 450;
};
const OverlayTokens &OTok();

// 图标开关（设置面板、录音面板）：开 = surfaceSelected 底 + iconActive 图标
void DrawToggle(Canvas &c, RectF r, const char *icon, bool on, bool hover, bool pressed, bool enabled);
// 紧凑下拉：当前值 + chevron
float ChipWidth(const std::vector<std::wstring> &labels);
void DrawChip(Canvas &c, RectF r, const std::wstring &label, bool hover, bool pressed, bool enabled);
// 主按钮（accent 底，中等字重），高 window.controlHeight
float PrimaryButtonWidth(const std::wstring &text);
void DrawPrimaryButton(Canvas &c, RectF r, const std::wstring &text, bool hover, bool pressed, bool enabled, bool danger = false);
// 工具栏样式的底板（阴影 + toolbarBg + 细边）
void DrawBarPanel(Canvas &c, RectF r, float radius);

// ---------------------------------------------------------------- 控制条

enum BarButton { BAR_NONE = 0, BAR_PAUSE, BAR_PEN, BAR_STOP, BAR_CANCEL };

struct BarView {
    std::wstring clock = L"00:00";
    bool paused = false;
    bool penOn = false;
    int hover = BAR_NONE;
    int pressed = BAR_NONE;

    SizeF Size() const;
    RectF ButtonRect(RectF bar, int id) const;
    int Hit(RectF bar, float x, float y) const;
    void Draw(Canvas &c, RectF bar) const;
    static std::wstring Tooltip(int id, bool paused);
};

// ---------------------------------------------------------------- 按键浮层

struct KeysView {
    struct Item {
        std::wstring text;
        float alpha = 1;
    };
    std::vector<Item> items;
    std::string position = "bottomCenter";
    std::string size = "medium";
    bool dark = true;

    const OverlayTokens::KeySize &Metrics() const;
    SizeF PillSize(const std::wstring &text) const;
    // 各组合在 r（区域坐标）里的位置；放不下时只保留最近的。返回 (下标, 矩形)
    std::vector<std::pair<int, RectF>> Layout(RectF r) const;
    void Draw(Canvas &c, RectF r) const;
};

// ---------------------------------------------------------------- GIF 进度

SizeF ProgressSize();
void DrawProgress(Canvas &c, RectF r, const std::wstring &text, double progress);

}  // namespace rec
}  // namespace ls
