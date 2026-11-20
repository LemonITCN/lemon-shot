// 录屏（spec 8）：第二期接入（DXGI/GDI 抓帧、Media Foundation 编码、WASAPI）。本期只有入口。
#pragma once

#include <windows.h>

namespace ls {
namespace record {

void Start();
void StartWithRegion(HMONITOR monitor, RECT regionPx);
void TogglePause();
void TogglePen();
void Stop();
bool IsRecording();
bool IsPaused();

}  // namespace record
}  // namespace ls
