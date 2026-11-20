// 截图层绘制（spec 2.2）：冻结画面、遮罩、标注、选区、十字线、尺寸提示、放大镜、左下角提示、工具栏、样式面板、气泡。
// 屏幕窗口与自测共用：只依赖会话状态与 CaptureUi。
#pragma once

#include <string>

#include "Capture/Annotations.h"
#include "Capture/StylePopover.h"
#include "Capture/Toolbar.h"
#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {

namespace rec {
class SetupPanel;  // 录屏设置面板（Record/SetupPanel.h）
}

struct CaptureOptions {
    bool showMagnifier = true;
    bool showCrosshair = false;
    bool showTips = true;
    Color tipsColor{0.92f, 0.35f, 0.32f, 1};
    bool hexPrefix = true;
    bool hexUpper = true;
    bool colorPicker = false;  // 取色模式：只有放大镜
    bool pinMode = false;      // 贴图标注：没有遮罩、选区框、尺寸提示、放大镜与提示，工具栏用贴图版
    bool recordMode = false;   // 录屏模式（spec 8.1）：没有放大镜，工具栏换成录屏设置面板
    static CaptureOptions FromSettings();
};

// 界面状态（悬停、样式面板等），以及上一次绘制时算出的布局（命中测试用）
struct CaptureUi {
    int hoverBtn = 0;
    int pressedBtn = 0;
    int tooltipBtn = 0;        // 显示提示气泡的按钮
    int hoverPopoverTool = 0;  // 悬停工具按钮弹出的样式面板（不切换工具）
    int popHover = -1;
    int popPressed = -1;
    bool sizeEditing = false;
    float sizeEditFieldW = 56;

    // 布局结果
    bool hasToolbar = false;
    bool toolbarBelow = true;
    ToolbarLayout toolbar;
    bool hasPopover = false;
    int popoverTool = 0;
    RectF popoverRect;
    PopLayout popover;
    bool hasSizeTip = false;
    RectF sizeTip;
    RectF sizeEditW, sizeEditH;  // 编辑时两个输入框的位置
    // 录屏模式：设置面板（由截图层窗口持有）与它是否显示
    rec::SetupPanel *recordPanel = nullptr;
    bool hasRecordPanel = false;
};

struct CaptureFrame {
    const ls_session *session = nullptr;
    const TiledBitmap *frozen = nullptr;
    float scale = 1;
    float width = 0, height = 0;  // 逻辑尺寸
    PatchCache *patches = nullptr;
    CaptureOptions opt;
};

// 计算工具栏/样式面板/尺寸提示的位置（绘制前、命中测试前调用）
void LayoutCaptureUi(const CaptureFrame &f, CaptureUi &ui);
void RenderCapture(Canvas &c, const CaptureFrame &f, CaptureUi &ui);
std::wstring SizeTipText(const ls_session *s, float scale);
std::wstring RatioName(int index);

}  // namespace ls
