// 录屏（spec 8）入口：录屏模式截图层 → 倒计时 → 录制（控制条、边框、画笔、点击、按键、摄像头）→ 编码保存。
// 画面采集、编码、声音各自在后台线程（见 Pipeline.h），这里的函数都在界面线程调用。
#pragma once

#include <windows.h>

namespace ls {
namespace record {

// 全局 record / 菜单「录屏」：录制中 = 停止（倒计时中 = 取消）；否则进入录屏模式截图层
void Start();
// 录屏模式截图层确认区域前的检查：缺编码组件（录 MP4 但没有 Media Foundation）、保存位置建不了时 toast 并返回 false，
// 截图层保持打开（可以改选 GIF）
bool CanStart();
// 开始录制（regionPx 为屏幕像素，必须在 monitor 内）
bool StartWithRegion(HMONITOR monitor, RECT regionPx);
void TogglePause();
void TogglePen();
void Stop();
bool IsRecording();  // 倒计时与录制中
bool IsPaused();
// 退出程序前：录制中则停止并保存（同步等写完），倒计时中取消
void Shutdown();
// 托盘图标：录制中为红点（暂停时灰点），没在录制返回 nullptr。size 为像素
HICON TrayIcon(int size);

}  // namespace record
}  // namespace ls
