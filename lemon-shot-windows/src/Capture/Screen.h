// 屏幕捕获（spec 2.1 Windows 部分）：按显示器 GDI BitBlt 冻结画面、窗口列表、单窗口截图。
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "lemonshot.h"

namespace ls {

// 一块显示器的冻结画面。img.data 指向 DIB 段内存（不是 ls_image_alloc 分配的，不能 ls_image_free）。
struct MonitorShot {
    HMONITOR mon = nullptr;
    RECT rc = {};    // 屏幕像素（虚拟桌面坐标，可为负）
    RECT work = {};  // 工作区
    std::wstring device;
    UINT dpi = 96;
    float scale = 1;     // 像素 / 逻辑点
    float lw = 0, lh = 0;  // 逻辑尺寸
    HBITMAP dib = nullptr;
    ls_image img = {};
    bool primary = false;

    MonitorShot() = default;
    ~MonitorShot();
    MonitorShot(const MonitorShot &) = delete;
    MonitorShot &operator=(const MonitorShot &) = delete;
};

struct WindowEntry {
    HWND hwnd;
    RECT rc;  // 屏幕像素（DWM 扩展边框，不含不可见的调整边）
};

struct MonitorInfo {
    HMONITOR mon;
    RECT rc, work;
    std::wstring device;
    UINT dpi;
    bool primary;
};

std::vector<MonitorInfo> EnumMonitors();
int MonitorAt(const std::vector<MonitorInfo> &mons, POINT pt);  // 找不到返回最近的

struct Snapshot {
    std::vector<std::unique_ptr<MonitorShot>> monitors;
    std::vector<WindowEntry> windows;  // z 序从上到下
    HWND foreground = nullptr;
    POINT cursor = {};
};

// 抓取全部显示器（withCursor：把光标画进冻结图）与窗口列表。onlyMonitor 非空时只抓这一块
bool TakeSnapshot(Snapshot &snap, bool withCursor, HMONITOR onlyMonitor = nullptr);
void CollectWindows(std::vector<WindowEntry> &out);
RECT WindowFrameRect(HWND hwnd);  // DWMWA_EXTENDED_FRAME_BOUNDS，Win7 无 DWM 时 GetWindowRect

// 截屏幕上一块像素矩形（BitBlt），结果预乘 BGRA 不透明；out 用 ls_image_free
bool CaptureScreenRect(RECT px, bool withCursor, ls_image &out);
// 单窗口：PrintWindow(PW_RENDERFULLCONTENT)，失败退回屏幕裁剪；结果裁到 DWM 边框
bool CaptureWindowImage(HWND hwnd, ls_image &out, RECT *frame, bool screenFallback = true);

// 光标所在显示器的 DPI 缩放
float ScaleForPoint(POINT pt);

}  // namespace ls
