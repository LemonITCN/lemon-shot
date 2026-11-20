// 提示（spec 13）：屏幕底部居中上方 80pt 的胶囊，淡入 120ms，停留 1.5 秒，淡出 200ms。
// 分层窗口（UpdateLayeredWindow），不抢焦点，Win10 2004+ 不进截图/录屏画面。
#pragma once

#include <string>

#include "UI/Gfx.h"

namespace ls {
namespace toast {

enum ActionKind { kNone = 0, kReveal = 1, kOpen = 2 };

struct Action {
    int kind = kNone;
    std::wstring path;
};

void Show(const std::wstring &text, const char *icon = "circle-check", const Action &action = Action());
void HideNow();  // 截图前调用，避免把提示截进去

// 绘制（屏幕与自测共用）：返回逻辑尺寸
RectF Measure(const std::wstring &text);
void Draw(Canvas &c, RectF r, const std::wstring &text, const char *icon);
// 外围留给阴影的边距（逻辑点）
float Margin();

}  // namespace toast
}  // namespace ls
