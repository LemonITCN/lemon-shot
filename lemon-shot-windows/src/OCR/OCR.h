// 文字识别（spec 4）：结果窗口（开关、缩略图、可编辑文本、复制/复制并关闭/翻译）、二维码优先、连续识别、固定区域。
#pragma once

#include <windows.h>

#include <string>

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {
namespace ocr {

// 全局快捷键 / 菜单：开了「固定区域」且记得上次区域时直接识别该区域，否则进截图层（选完区域自动识别）
void Start();
// 截图工具栏「文字识别」、贴图右键：识别给定图片（接管所有权）。
// regionPx 为图片在屏幕上的像素区域（记作「固定区域」），贴图等没有区域时为空
void Recognize(ls_image image, float scale, const RECT *regionPx = nullptr);

// 自测：结果窗口（假缩略图 + 假结果），逻辑尺寸 520×420
SizeF SelfTestSize();
ls_image RenderSelfTest(float scale, const ls_image &thumb, const std::wstring &text);

}  // namespace ocr
}  // namespace ls
