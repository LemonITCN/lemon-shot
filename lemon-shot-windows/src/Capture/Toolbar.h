// 截图工具栏（spec 2.3）：布局、绘制、命中。截图层与贴图标注共用。
#pragma once

#include <string>
#include <vector>

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {

enum ToolbarButton {
    TB_NONE = 0,
    // 1..10 与 ls_tool 同值
    TB_UNDO = 20,
    TB_REDO,
    TB_LONGSHOT,
    TB_RECORD,
    TB_OCR,
    TB_TRANSLATE,
    TB_PIN,
    TB_SAVE,
    TB_CANCEL,
    TB_DONE,
};

struct ToolbarLayout {
    RectF rect;
    struct Btn {
        int id;
        RectF r;
    };
    std::vector<Btn> btns;
    std::vector<RectF> seps;
    RectF ButtonRect(int id) const;
};

// pinMode：贴图标注时没有「功能」组（长截图/录屏/识别/翻译/贴图）
float ToolbarWidth(bool pinMode = false);
float ToolbarHeight();
ToolbarLayout LayoutToolbar(float x, float y, bool pinMode = false);
const char *ToolbarIcon(int id);
std::wstring ToolbarTooltip(int id);  // 「名称　快捷键」
bool ToolbarEnabled(int id, const ls_session_state &st);
void DrawToolbar(Canvas &c, const ToolbarLayout &lay, const ls_session_state &st, int hover, int pressed);
int HitToolbar(const ToolbarLayout &lay, float x, float y);

// 提示气泡：放在按钮下方（below = false 时放上方），夹在 [0, screenW]
RectF ToolbarTooltipRect(RectF btn, const std::wstring &text, bool below, float screenW, float screenH);
void DrawToolbarTooltip(Canvas &c, RectF btn, const std::wstring &text, bool below, float screenW, float screenH);

}  // namespace ls
