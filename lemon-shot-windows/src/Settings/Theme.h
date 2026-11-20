// 界面令牌（tokens.json）与浅色/深色主题。
#pragma once

#include <string>
#include <vector>

namespace ls {

// 与 D2D1_COLOR_F 内存布局一致（非预乘，0..1）
struct Color {
    float r = 0, g = 0, b = 0, a = 0;
};

Color ParseColor(const std::string &hex, Color def = Color{0, 0, 0, 1});
std::string ColorToHex(Color c, bool withAlpha);  // "#RRGGBB" 或 "#RRGGBBAA"
inline Color WithAlpha(Color c, float a) { return Color{c.r, c.g, c.b, c.a * a}; }

struct UiColors {
    Color accent, accentHover, accentText, danger, success, warning, windowBg, surface, surfaceHover, surfacePressed,
        surfaceSelected, border, divider, text, textSecondary, textTertiary, icon, iconActive, toolbarBg, toolbarBorder,
        popoverBg, shadow, controlTrack, controlKnob, inputBg, inputBorder, focusRing;
};

struct CaptureColors {
    Color mask, selectionBorder, windowHover, windowHoverFill, handleFill, handleBorder, tipBg, tipText, crosshair,
        magnifierBorder, magnifierGrid, magnifierCenter, shapeHandleFill, shapeHandleBorder, shapeSelectionDash;
};

struct Tokens {
    std::vector<std::wstring> fontFamilies;  // Microsoft YaHei UI → Microsoft YaHei → Segoe UI
    std::vector<std::wstring> monoFamilies;
    struct {
        float caption, body, title, header;
    } fontSize;
    struct {
        float selectionBorderWidth, handleSize, handleHitSize, edgeHitSize, minSelection, windowHoverBorderWidth,
            tipHeight, tipPaddingX, tipRadius, tipGap, toolbarGap, toolbarScreenMargin;
        struct {
            float zoom, pixels, size, infoHeight, radius, offset;
        } magnifier;
    } capture;
    struct {
        float height, paddingX, radius, button, buttonRadius, iconSize, iconStroke, spacing, separatorWidth,
            separatorHeight, separatorMargin, shadowBlur, shadowOffsetY;
    } toolbar;
    struct {
        float radius, padding, gap, arrow, swatch, swatchGap, swatchRadius, sliderWidth, rowHeight;
    } popover;
    struct {
        float radius, titleBarHeight, sidebarWidth, contentPadding, sectionGap, rowHeight, rowGap, labelWidth,
            controlHeight, controlRadius, switchWidth, switchHeight, checkbox, buttonPaddingX, sidebarItemHeight,
            sidebarItemRadius, sidebarIcon;
    } window;
    struct {
        float minSize, borderWidth;
        Color activeBorder, inactiveBorder;
        float shadowBlur, shadowOffsetY, zoomStep, zoomMin, zoomMax, opacityStep;
    } pin;
    struct {
        float height, radius, countdownSize;
        Color regionBorder;
        float regionBorderWidth;
        Color dot;
    } recordBar;
    struct {
        float fastMs, normalMs;
    } animation;
    UiColors light, dark;
    CaptureColors cap;
};

namespace theme {

void Init();
const Tokens &Tok();
const UiColors &Ui();        // 当前主题
const CaptureColors &Cap();  // 截图层颜色（不随主题变）
bool IsDark();
// 根据设置 general.theme（system/light/dark）与系统 AppsUseLightTheme 重新计算；返回是否变化
bool Refresh();
// 自测：强制浅色或深色（0 不强制，1 浅色，2 深色）
void ForceMode(int mode);
// 系统任务栏是否为深色（托盘图标选色用；Win7 视为深色任务栏）
bool SystemTaskbarDark();

}  // namespace theme
}  // namespace ls
