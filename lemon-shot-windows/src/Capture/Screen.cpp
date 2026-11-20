#include "Capture/Screen.h"

#include <dwmapi.h>

#include <cstring>

#include "Util/Api.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {

MonitorShot::~MonitorShot() {
    if (dib) DeleteObject(dib);
}

static BOOL CALLBACK MonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto *out = reinterpret_cast<std::vector<MonitorInfo> *>(lp);
    MONITORINFOEXW mi;
    std::memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    MonitorInfo m;
    m.mon = mon;
    m.rc = mi.rcMonitor;
    m.work = mi.rcWork;
    m.device = mi.szDevice;
    m.dpi = api::MonitorDpi(mon);
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    out->push_back(m);
    return TRUE;
}

std::vector<MonitorInfo> EnumMonitors() {
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(nullptr, nullptr, &MonitorProc, (LPARAM)&out);
    return out;
}

int MonitorAt(const std::vector<MonitorInfo> &mons, POINT pt) {
    for (size_t i = 0; i < mons.size(); ++i)
        if (PtInRect(&mons[i].rc, pt)) return (int)i;
    HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    for (size_t i = 0; i < mons.size(); ++i)
        if (mons[i].mon == m) return (int)i;
    return mons.empty() ? -1 : 0;
}

float ScaleForPoint(POINT pt) {
    HMONITOR m = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    return api::MonitorDpi(m) / 96.f;
}

// 32 位自顶向下 DIB 段
static HBITMAP CreateDib32(int w, int h, void **bits) {
    BITMAPINFO bi;
    std::memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static void ForceOpaque(uint8_t *data, int w, int h, int stride) {
    for (int y = 0; y < h; ++y) {
        uint8_t *p = data + (size_t)y * stride + 3;
        for (int x = 0; x < w; ++x, p += 4) *p = 255;
    }
}

static void DrawCursorInto(HDC dc, int originX, int originY) {
    CURSORINFO ci;
    std::memset(&ci, 0, sizeof ci);
    ci.cbSize = sizeof ci;
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    ICONINFO ii;
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    int x = ci.ptScreenPos.x - (int)ii.xHotspot - originX;
    int y = ci.ptScreenPos.y - (int)ii.yHotspot - originY;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DrawIconEx(dc, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
}

// 从屏幕 DC 拷贝一块到新 DIB
static HBITMAP GrabRect(HDC screen, RECT px, bool withCursor, void **bits) {
    int w = px.right - px.left, h = px.bottom - px.top;
    if (w <= 0 || h <= 0) return nullptr;
    HBITMAP dib = CreateDib32(w, h, bits);
    if (!dib || !*bits) {
        if (dib) DeleteObject(dib);
        return nullptr;
    }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, screen, px.left, px.top, SRCCOPY | CAPTUREBLT);
    if (withCursor) DrawCursorInto(mem, px.left, px.top);
    GdiFlush();
    SelectObject(mem, old);
    DeleteDC(mem);
    // GDI 不写 alpha：一律置为不透明（预乘与非预乘此时相同）
    ForceOpaque((uint8_t *)*bits, w, h, w * 4);
    return dib;
}

RECT WindowFrameRect(HWND hwnd) {
    RECT r = {};
    BOOL composition = FALSE;
    if (SUCCEEDED(DwmIsCompositionEnabled(&composition)) && composition &&
        SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)) && r.right > r.left)
        return r;
    GetWindowRect(hwnd, &r);
    return r;
}

static bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    // DWMWA_CLOAKED = 14：Win8 起才有，Win7 调用失败视为未隐藏
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, 14, &cloaked, sizeof cloaked))) return cloaked != 0;
    return false;
}

struct EnumCtx {
    std::vector<WindowEntry> *out;
    DWORD pid;
};

static BOOL CALLBACK WindowProc(HWND hwnd, LPARAM lp) {
    EnumCtx *ctx = reinterpret_cast<EnumCtx *>(lp);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid) return TRUE;
    if (IsCloaked(hwnd)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 63);
    // 桌面本身不当作窗口（都不包含时内核自动高亮整屏）
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return TRUE;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) return TRUE;
    }
    if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED)) return TRUE;  // 点击穿透的浮层
    RECT r = WindowFrameRect(hwnd);
    if (r.right - r.left < 4 || r.bottom - r.top < 4) return TRUE;
    ctx->out->push_back({hwnd, r});
    return TRUE;
}

void CollectWindows(std::vector<WindowEntry> &out) {
    out.clear();
    EnumCtx ctx{&out, GetCurrentProcessId()};
    EnumWindows(&WindowProc, (LPARAM)&ctx);
}

bool TakeSnapshot(Snapshot &snap, bool withCursor, HMONITOR onlyMonitor) {
    double t0 = NowSeconds();
    snap.monitors.clear();
    snap.windows.clear();
    snap.foreground = GetForegroundWindow();
    GetCursorPos(&snap.cursor);
    std::vector<MonitorInfo> mons = EnumMonitors();
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    for (auto &m : mons) {
        if (onlyMonitor && m.mon != onlyMonitor) continue;
        std::unique_ptr<MonitorShot> s(new MonitorShot());
        s->mon = m.mon;
        s->rc = m.rc;
        s->work = m.work;
        s->device = m.device;
        s->dpi = m.dpi;
        s->scale = m.dpi / 96.f;
        s->primary = m.primary;
        int w = m.rc.right - m.rc.left, h = m.rc.bottom - m.rc.top;
        void *bits = nullptr;
        s->dib = GrabRect(screen, m.rc, withCursor, &bits);
        if (!s->dib) {
            LS_LOG("capture: 显示器 %ls 截图失败", m.device.c_str());
            continue;
        }
        s->img.data = (uint8_t *)bits;
        s->img.width = w;
        s->img.height = h;
        s->img.stride = w * 4;
        s->lw = w / s->scale;
        s->lh = h / s->scale;
        snap.monitors.push_back(std::move(s));
    }
    ReleaseDC(nullptr, screen);
    CollectWindows(snap.windows);
    LS_LOG("capture: 冻结 %d 块屏 %d 个窗口 用时 %.1f ms", (int)snap.monitors.size(), (int)snap.windows.size(),
           (NowSeconds() - t0) * 1000.0);
    return !snap.monitors.empty();
}

bool CaptureScreenRect(RECT px, bool withCursor, ls_image &out) {
    out = ls_image_alloc(px.right - px.left, px.bottom - px.top);
    if (!out.data) return false;
    HDC screen = GetDC(nullptr);
    void *bits = nullptr;
    HBITMAP dib = GrabRect(screen, px, withCursor, &bits);
    ReleaseDC(nullptr, screen);
    if (!dib) {
        ls_image_free(&out);
        return false;
    }
    std::memcpy(out.data, bits, (size_t)out.stride * out.height);
    DeleteObject(dib);
    return true;
}

// 整张图是否几乎全黑（PrintWindow 对某些硬件加速窗口会返回黑图）
static bool MostlyBlack(const uint8_t *p, int w, int h, int stride) {
    int samples = 0, black = 0;
    for (int y = 0; y < h; y += (h / 16 > 0 ? h / 16 : 1)) {
        for (int x = 0; x < w; x += (w / 16 > 0 ? w / 16 : 1)) {
            const uint8_t *q = p + (size_t)y * stride + (size_t)x * 4;
            ++samples;
            if (q[0] < 8 && q[1] < 8 && q[2] < 8) ++black;
        }
    }
    return samples > 0 && black * 10 >= samples * 9;
}

bool CaptureWindowImage(HWND hwnd, ls_image &out, RECT *frameOut, bool screenFallback) {
    out = ls_image{nullptr, 0, 0, 0};
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return false;
    RECT frame = WindowFrameRect(hwnd);
    if (frameOut) *frameOut = frame;
    int w = wr.right - wr.left, h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return false;
    void *bits = nullptr;
    HBITMAP dib = CreateDib32(w, h, &bits);
    bool ok = false;
    if (dib && bits) {
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        HGDIOBJ old = SelectObject(mem, dib);
        // PW_RENDERFULLCONTENT = 2：Win8.1 起支持；老系统不认这个标志时再用 0 试一次
        ok = PrintWindow(hwnd, mem, 2) != 0;
        if (!ok) ok = PrintWindow(hwnd, mem, 0) != 0;
        GdiFlush();
        SelectObject(mem, old);
        DeleteDC(mem);
        if (ok) ForceOpaque((uint8_t *)bits, w, h, w * 4);
        // 裁到 DWM 边框
        int fx = frame.left - wr.left, fy = frame.top - wr.top;
        int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
        if (fx < 0) fx = 0;
        if (fy < 0) fy = 0;
        if (fx + fw > w) fw = w - fx;
        if (fy + fh > h) fh = h - fy;
        if (ok && fw > 0 && fh > 0 && !MostlyBlack((uint8_t *)bits + (size_t)fy * w * 4 + (size_t)fx * 4, fw, fh, w * 4)) {
            out = ls_image_alloc(fw, fh);
            if (out.data) {
                for (int y = 0; y < fh; ++y)
                    std::memcpy(out.data + (size_t)y * out.stride, (uint8_t *)bits + (size_t)(fy + y) * w * 4 + (size_t)fx * 4,
                                (size_t)fw * 4);
            } else {
                ok = false;
            }
        } else {
            ok = false;
        }
    }
    if (dib) DeleteObject(dib);
    if (ok) return true;
    if (!screenFallback) return false;
    // 退回屏幕裁剪（窗口被遮挡的部分会截到上层窗口）
    return CaptureScreenRect(frame, false, out);
}

}  // namespace ls
