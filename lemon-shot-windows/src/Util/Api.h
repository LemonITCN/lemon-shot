// 运行时动态获取 Win7 SP1 之后才有的 API，没有时降级。任何新 API 都只能经过这里调用。
#pragma once

#include <windows.h>

namespace ls {
namespace api {

void Init();  // 进程启动时调用一次

// 系统版本（RtlGetVersion，不受兼容性清单影响）
DWORD OsMajor();
DWORD OsMinor();
DWORD OsBuild();
bool IsWin8OrLater();
bool IsWin81OrLater();  // WinHTTP 自动代理、默认开 TLS 1.2 从 8.1 起
bool IsWin10OrLater();

// DPI 感知：PerMonitorV2（Win10 1703+）→ PerMonitor（Win8.1）→ 系统 DPI（Win7）
void SetupDpiAwareness();
bool PerMonitorDpi();  // 进程是否按显示器感知 DPI
UINT SystemDpi();
UINT MonitorDpi(HMONITOR mon);
UINT WindowDpi(HWND hwnd);
// 按 DPI 调整窗口外框（Win10 1607+ 用 AdjustWindowRectExForDpi）
void AdjustRectForDpi(RECT *rc, DWORD style, DWORD exStyle, UINT dpi);
// 非客户区随 DPI 缩放（Win10 1607 之前的 PerMonitor v1 需要手动开启）
void EnableNonClientScaling(HWND hwnd);

// 截图/录屏时排除窗口：Win10 2004+ WDA_EXCLUDEFROMCAPTURE，失败返回 false
bool ExcludeFromCapture(HWND hwnd);

// 深色标题栏（Win10 1809+），其他系统无效果
void SetDarkTitleBar(HWND hwnd, bool dark);

}  // namespace api
}  // namespace ls
