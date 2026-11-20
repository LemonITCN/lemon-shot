// 长截图（spec 6）：区域外红色细边框 + 侧面板（预览、高度、自动滚动/完成/取消，首次先显示注意事项），
// 每 100ms 用 GDI BitBlt 抓一次区域交给内核 ls_stitch_push（拼接在后台线程），可自动滚动（SendInput 滚轮），完成后打开结果窗口。
#pragma once

#include <windows.h>

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {
namespace longshot {

// 菜单 / 全局快捷键：进截图层，选好区域后开始
void Start();
// 截图工具栏「长截图」：给定屏幕像素区域
void StartWithRegion(HMONITOR monitor, RECT regionPx);
bool IsActive();

// 结果窗口（复制、保存、贴图、关闭）：接管 img
void ShowResult(ls_image img, float scale, HMONITOR monitor);

// 自测：假屏幕（逻辑 screenW×screenH，像素图 screen）上画区域边框与面板（自动滚动中，假预览与高度），返回整屏图
ls_image RenderSelfTest(float scale, const ls_image &screen, float screenW, float screenH, RectF region, const ls_image &preview,
                        int heightPx);

}  // namespace longshot
}  // namespace ls
