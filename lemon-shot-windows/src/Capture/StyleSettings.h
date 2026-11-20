// 标注样式与设置 annotate.<tool> 之间的换算（截图样式面板、偏好设置「标注」页共用）。
#pragma once

#include <string>
#include <vector>

#include "Settings/Theme.h"
#include "lemonshot.h"

namespace ls {
namespace style {

const char *ToolKey(int tool);       // "rect" "ellipse" …
const char *ToolNameKey(int tool);   // i18n 键 "tool.rect" …
const char *ToolIcon(int tool);      // 工具栏图标名
std::vector<Color> Palette();        // annotate.palette
ls_color ToLs(Color c);
Color FromLs(ls_color c);

ls_style FromSettings(int tool);
// 写回设置（不写「下一个」序号：每次截图都从设置值开始）
void ToSettings(int tool, const ls_style &st);
// 给会话设置全部工具的默认样式
void ApplyAll(ls_session *s);

// 粗细三档（线宽 2/3/6）；放大镜边框同样三档
extern const float kLineWidths[3];
int NearestLevel(float w);

}  // namespace style
}  // namespace ls
