// 带壳截图（spec 7）：延时（shell.delay）后截光标所在屏的全屏，套上设备外壳，打开带壳预览窗口
// （左侧预览，右侧机型列表 + 背景 + 复制、保存）。Windows 自动匹配：被截的屏是内置屏 → 通用笔记本，否则 → 通用显示器。
#pragma once

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {
namespace shell {

void Capture();

// 自测：带壳预览窗口（自动匹配 + 假截图），逻辑尺寸 920×600
SizeF SelfTestSize();
ls_image RenderSelfTest(float scale, const ls_image &shot);

}  // namespace shell
}  // namespace ls
