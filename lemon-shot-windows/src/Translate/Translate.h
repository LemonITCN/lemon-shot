// 截图翻译（spec 5）：内核 ls_translate_* 拼请求、签名、解析，这里只用 WinHTTP 发请求（后台线程）。
// 窗口：上原文（可编辑）下译文，顶部「自动 → 目标语言」与翻译服务，底部复制译文。
#pragma once

#include <string>

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {
namespace translate {

// 全局快捷键 / 菜单：进截图层，选完区域自动识别并翻译
void Start();
// 截图工具栏「翻译」：先识别图里的文字再翻译（接管所有权）
void FromImage(ls_image image, float scale);
// 文字识别窗口「翻译」
void FromText(const std::wstring &text);
// 偏好设置「测试」按钮：用所选服务翻译一个词，结果用 toast 提示
void TestProvider(const std::string &provider);

// 自测：翻译窗口（假原文与译文），逻辑尺寸 560×420
SizeF SelfTestSize();
ls_image RenderSelfTest(float scale, const std::wstring &source, const std::wstring &result);

}  // namespace translate
}  // namespace ls
