// 偏好设置（spec 11）：固定 760×520 窗口，左侧边栏 12 页，右侧内容区；控件全部自绘。
#pragma once

#include <string>
#include <vector>

#include "UI/Gfx.h"

namespace ls {
namespace prefs {

void Show(const char *page);  // page 为页 id（general、capture … about），空则保持当前页
void Close();
void Refresh();               // 语言、主题变化后重画
bool IsOpen();
bool IsRecordingShortcut();   // 正在录入快捷键（此时全局快捷键已暂停）
void OnSettingChanged(const std::string &path);

// 自测：整窗画进一张图（侧边栏 + 该页）；内容超出 520 时加高，便于对照全部控件
std::vector<std::string> PageIds();
SizeF PageSize(const std::string &page);
void RenderPage(Canvas &c, const std::string &page);

}  // namespace prefs
}  // namespace ls
