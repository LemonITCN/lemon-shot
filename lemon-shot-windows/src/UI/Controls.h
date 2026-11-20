// 自绘控件的绘制函数（尺寸、颜色、圆角全部来自 tokens.json）。
// 只负责「画」与「量」，交互状态由使用方（偏好设置、样式面板）维护。
#pragma once

#include <string>
#include <vector>

#include "UI/Gfx.h"

namespace ls {
namespace ui {

struct CtrlState {
    bool hover = false;
    bool pressed = false;
    bool focused = false;
    bool disabled = false;
};

TextStyle BodyText(Color c, HAlign h = HAlign::Left);

void DrawSwitch(Canvas &c, RectF r, bool on, CtrlState st);

float SegmentedWidth(const std::vector<std::wstring> &labels);
void DrawSegmented(Canvas &c, RectF r, const std::vector<std::wstring> &labels, int selected, int hover, CtrlState st);
int SegmentedHit(RectF r, const std::vector<std::wstring> &labels, float x, float y);

void DrawDropdown(Canvas &c, RectF r, const std::wstring &text, CtrlState st);
float DropdownWidth(const std::vector<std::wstring> &options);

// 滑块：r 为整个可点击区域（含圆钮），t 为 0..1
void DrawSlider(Canvas &c, RectF r, float t, CtrlState st);
float SliderValueAt(RectF r, float x);  // 返回 0..1

// 步进：[-] 数值 [+]；hoverPart 1 = 减，2 = 加
float StepperWidth();
void DrawStepper(Canvas &c, RectF r, const std::wstring &value, int hoverPart, CtrlState st);
int StepperHit(RectF r, float x, float y);

float ButtonWidth(const std::wstring &text);
void DrawButton(Canvas &c, RectF r, const std::wstring &text, bool primary, CtrlState st);
void DrawIconButton(Canvas &c, RectF r, const char *icon, float iconSize, bool selected, CtrlState st);

void DrawSwatch(Canvas &c, float cx, float cy, float d, Color color, bool selected, bool hover);
void DrawInputFrame(Canvas &c, RectF r, CtrlState st);
void DrawInfoIcon(Canvas &c, float cx, float cy, float size, CtrlState st);

// 气泡（深色提示底）：宽度自适应，maxWidth 时换行；返回绘制尺寸
RectF MeasureBubble(const std::wstring &text, float maxWidth);
void DrawBubble(Canvas &c, RectF r, const std::wstring &text);

// 弹出面板底板：阴影 + popoverBg + 细边
void DrawPanel(Canvas &c, RectF r, float radius);

}  // namespace ui
}  // namespace ls
