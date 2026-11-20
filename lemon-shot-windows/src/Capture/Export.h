// 导出（spec 2.8）：离屏按像素尺寸画「冻结画面裁剪 + 标注」，复制 / 保存 / 贴图。
#pragma once

#include <windows.h>

#include <string>

#include "lemonshot.h"

namespace ls {
namespace exporter {

enum Output { OUT_NONE, OUT_COPY, OUT_SAVE, OUT_BOTH, OUT_PIN, OUT_SAVE_OPEN };

// 设置值（copy/save/both/pin/none/saveAndOpen）→ Output
int OutputFromSetting(const std::string &v);

// 选区像素矩形内的冻结画面 + 标注（与屏幕同一套绘制代码）；out 用 ls_image_free
bool RenderSelection(const ls_session *s, const ls_image &base, float scale, ls_image &out);

// 交付：接管 img 的所有权。originPx 为图片左上角在屏幕上的像素位置（贴图用）
void Deliver(ls_image img, float scale, int output, POINT originPx);

// 保存：按设置的位置/文件名/格式；ask 时弹系统保存框。成功返回 true 并给出路径
bool SaveImage(const ls_image &img, float scale, bool ask, std::wstring &path, std::wstring &error);
std::wstring ExpandFileName(const std::string &tpl);
std::wstring SaveFolder();
std::string SaveFormat();  // 实际使用的格式（webp/heic 退回 png）

}  // namespace exporter
}  // namespace ls
