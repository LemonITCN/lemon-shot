// 样式面板（spec 2.4）：第 1 行工具相关控件，第 2 行调色板（局部高亮、马赛克除外；马赛克第 2 行放滑块），右上角 info。
// 偏好设置「标注」页复用第 1、2 行的工具控件（不带调色板、info 与「下一个」）。
#pragma once

#include <vector>

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {

enum PopKind {
    PK_DOT,      // 粗细三档
    PK_TOGGLE,   // 图标开关（实心、粗体）
    PK_DASH,     // 虚线开关（自绘预览）
    PK_CHIP,     // 文字按钮（圆角开关、放大倍数）
    PK_VARIANT,  // 子样式
    PK_LABEL,
    PK_SLIDER,
    PK_STEPPER,
    PK_SWATCH,
    PK_CUSTOM,
    PK_INFO,
    PK_SEP,
};

enum PopField {
    PF_NONE,
    PF_LINEW,
    PF_FILLED,
    PF_CORNER,
    PF_DASHED,
    PF_VARIANT,
    PF_BLOCK,
    PF_BRUSH,
    PF_FONT,
    PF_BOLD,
    PF_STEPSIZE,
    PF_NEXT,
    PF_OPACITY,
    PF_ZOOM,
    PF_COLOR,
};

struct PopItem {
    PopKind kind;
    PopField field;
    RectF r;      // 面板内坐标
    RectF track;  // 滑块轨道 / 步进框
    int index;
    float min, max;
    const char *icon;
    const char *label;  // i18n 键（标签、文字按钮）
};

struct PopLayout {
    int tool = 0;
    float w = 0, h = 0;
    std::vector<PopItem> items;
};

// full = true：截图里的样式面板（底板、调色板、info、「下一个」）；false：偏好设置用（从 (0,0) 起排，不带这些）
PopLayout LayoutPopover(int tool, const ls_style &st, bool full);
void DrawPopover(Canvas &c, const PopLayout &lay, float ox, float oy, const ls_style &st, int hover, bool panel);
int HitPopover(const PopLayout &lay, float x, float y);  // 面板内坐标；没有返回 -1
// 点击/拖动第 item 项（面板内坐标）。改了返回 true；自定义颜色项、info 返回 false
bool PopoverApply(const PopLayout &lay, int item, float x, float y, ls_style &st, bool dragging);
// info 气泡（style.hint），放在面板下方或上方
void DrawStyleHint(Canvas &c, RectF popover, float screenW, float screenH);

}  // namespace ls
