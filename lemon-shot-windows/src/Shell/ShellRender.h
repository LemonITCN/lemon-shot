// 带壳截图的外壳（spec 7）：矢量自绘（圆角机身、屏幕黑边、刘海/摄像头、底座），不用任何图片素材。
// 几何与配色与 Mac 端 Shell/ShellRenderer.swift 逐项一致：所有尺寸按屏幕宽 W（像素）的比例给出，输出与截图同一像素密度。
// 渲染自己建 D2D/WIC 工厂，可以在后台线程调用。
#pragma once

#include <string>

#include "lemonshot.h"

namespace ls {
namespace shell {

enum Model { kMacbookAir, kMacbookPro14, kMacbookPro16, kImac24, kStudioDisplay, kLaptop, kMonitor, kIphone, kIpad, kModelCount };

const char *ModelKey(int model);            // "macbookAir" 等（设置 shell.model 的值）
int ModelFromKey(const std::string &key);   // 未知返回 -1
const char *ModelIcon(int model);           // 机型列表里的图标

// 套壳：截图等比缩放、居中裁切进外壳屏幕区域；background：transparent / white / black。out 新分配
bool Render(const ls_image &shot, int model, const std::string &background, ls_image &out);

}  // namespace shell
}  // namespace ls
