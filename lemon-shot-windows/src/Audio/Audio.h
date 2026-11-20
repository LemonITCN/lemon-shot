// 录音（spec 10）：屏幕右上角浮动面板（300×120，圆角 12，popoverBg，可拖动，不抢焦点，不进截图/录屏画面）：
// 标题 + 计时 + 关闭；来源开关（麦克风 / 系统声音）+ 格式；电平条 + 开始/停止。
// 采集与写文件复用录屏的 WASAPI 采集、混音与写入线程（Record/Pipeline.h）：m4a（MF AAC）、wav、mp3（Win8+ MF MP3 编码器）。
#pragma once

#include <string>

#include "UI/Gfx.h"

namespace ls {
namespace audio {

// 全局 recordAudio / 菜单「录音」：面板没开 = 打开；录音中 = 停止；否则关闭面板
void Toggle();
// 退出程序前：录音中则停止并保存（同步）
void Shutdown();
bool IsRecording();

// 面板的绘制数据（屏幕与自测共用）
struct PanelView {
    bool recording = false;
    std::wstring clock = L"00:00";
    float level = 0;  // 0..1（已换算成表盘刻度）
    bool mic = true, system = false;
    std::wstring format = L"M4A";
    int hover = 0, pressed = 0;  // 见 Audio.cpp 的按钮编号
};
SizeF PanelSize();
void DrawPanel(Canvas &c, RectF r, const PanelView &v);
// 峰值 → 表盘刻度（-50 dB 到 0 dB）
float Meter(float peak);

}  // namespace audio
}  // namespace ls
