// 贴图（spec 3）：贴图窗、贴图库、全局显隐。
#pragma once

#include <windows.h>

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {
namespace pins {

// 新建贴图：接管 img 的所有权；originPx 为图片左上角的屏幕像素位置；source："capture" / "clipboard" / "library"
void Create(ls_image img, float scale, POINT originPx, const char *source);
void PinClipboard();
void ToggleAll();
void CloseAll();
void HideAll();
int Count();
void ShowLibrary();
void OnSettingsChanged();
// 偏好设置里改了「在所有桌面显示」：作用到已打开的贴图
void ApplyAllDesktops(bool on);

// 自测：画一个贴图窗（含按钮、圆角、阴影），返回逻辑尺寸
RectF PinPreviewSize(float imgW, float imgH);
void DrawPinPreview(Canvas &c, ID2D1Bitmap *img, float imgW, float imgH, bool active, bool buttons, int hoverBtn);

}  // namespace pins
}  // namespace ls
