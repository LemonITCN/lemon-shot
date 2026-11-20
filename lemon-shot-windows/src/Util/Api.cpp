#include "Util/Api.h"

#include <dwmapi.h>

namespace ls {
namespace api {

namespace {

typedef LONG(WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
typedef BOOL(WINAPI *SetProcessDpiAwarenessContextFn)(HANDLE);
typedef HRESULT(WINAPI *SetProcessDpiAwarenessFn)(int);
typedef BOOL(WINAPI *SetProcessDPIAwareFn)(void);
typedef UINT(WINAPI *GetDpiForWindowFn)(HWND);
typedef UINT(WINAPI *GetDpiForSystemFn)(void);
typedef HRESULT(WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);
typedef BOOL(WINAPI *AdjustWindowRectExForDpiFn)(RECT *, DWORD, BOOL, DWORD, UINT);
typedef BOOL(WINAPI *EnableNonClientDpiScalingFn)(HWND);
typedef HRESULT(WINAPI *GetProcessDpiAwarenessFn)(HANDLE, int *);

RtlGetVersionFn pRtlGetVersion;
SetProcessDpiAwarenessContextFn pSetProcessDpiAwarenessContext;
SetProcessDpiAwarenessFn pSetProcessDpiAwareness;
SetProcessDPIAwareFn pSetProcessDPIAware;
GetDpiForWindowFn pGetDpiForWindow;
GetDpiForSystemFn pGetDpiForSystem;
GetDpiForMonitorFn pGetDpiForMonitor;
AdjustWindowRectExForDpiFn pAdjustWindowRectExForDpi;
EnableNonClientDpiScalingFn pEnableNonClientDpiScaling;
GetProcessDpiAwarenessFn pGetProcessDpiAwareness;

OSVERSIONINFOW g_ver;
bool g_inited = false;
bool g_perMonitor = false;
UINT g_systemDpi = 0;

template <class T>
void Load(HMODULE m, const char *name, T &out) {
    out = m ? reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(m, name))) : nullptr;
}

}  // namespace

void Init() {
    if (g_inited) return;
    g_inited = true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    // shcore.dll 只在 Win8.1+ 存在；LoadLibraryEx 只在 System32 里找，避免 DLL 劫持
    HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    if (!shcore && GetLastError() == ERROR_INVALID_PARAMETER) shcore = nullptr;  // Win7 无 KB2533623 时不支持该标志
    Load(ntdll, "RtlGetVersion", pRtlGetVersion);
    Load(user32, "SetProcessDpiAwarenessContext", pSetProcessDpiAwarenessContext);
    Load(user32, "SetProcessDPIAware", pSetProcessDPIAware);
    Load(user32, "GetDpiForWindow", pGetDpiForWindow);
    Load(user32, "GetDpiForSystem", pGetDpiForSystem);
    Load(user32, "AdjustWindowRectExForDpi", pAdjustWindowRectExForDpi);
    Load(user32, "EnableNonClientDpiScaling", pEnableNonClientDpiScaling);
    Load(shcore, "SetProcessDpiAwareness", pSetProcessDpiAwareness);
    Load(shcore, "GetDpiForMonitor", pGetDpiForMonitor);
    Load(shcore, "GetProcessDpiAwareness", pGetProcessDpiAwareness);
    g_ver.dwOSVersionInfoSize = sizeof g_ver;
    if (!pRtlGetVersion || pRtlGetVersion(&g_ver) != 0) {
        g_ver.dwMajorVersion = 6;
        g_ver.dwMinorVersion = 1;
        g_ver.dwBuildNumber = 7601;
    }
}

DWORD OsMajor() { return g_ver.dwMajorVersion; }
DWORD OsMinor() { return g_ver.dwMinorVersion; }
DWORD OsBuild() { return g_ver.dwBuildNumber; }
bool IsWin8OrLater() { return g_ver.dwMajorVersion > 6 || (g_ver.dwMajorVersion == 6 && g_ver.dwMinorVersion >= 2); }
bool IsWin81OrLater() { return g_ver.dwMajorVersion > 6 || (g_ver.dwMajorVersion == 6 && g_ver.dwMinorVersion >= 3); }
bool IsWin10OrLater() { return g_ver.dwMajorVersion >= 10; }

void SetupDpiAwareness() {
    Init();
    // 清单已声明 PerMonitorV2/PerMonitor，这里是兜底（例如被外部改了兼容性设置）。
    // 清单生效时这些调用会因为「已设置」而失败，不影响结果。
    if (pSetProcessDpiAwarenessContext) {
        pSetProcessDpiAwarenessContext((HANDLE)(INT_PTR)-4 /* PER_MONITOR_AWARE_V2 */);
    } else if (pSetProcessDpiAwareness) {
        pSetProcessDpiAwareness(2 /* PROCESS_PER_MONITOR_DPI_AWARE */);
    } else if (pSetProcessDPIAware) {
        pSetProcessDPIAware();
    }
    int awareness = 0;
    if (pGetProcessDpiAwareness && SUCCEEDED(pGetProcessDpiAwareness(nullptr, &awareness)))
        g_perMonitor = awareness == 2;
    else
        g_perMonitor = false;
    g_systemDpi = 0;
}

bool PerMonitorDpi() { return g_perMonitor; }

UINT SystemDpi() {
    if (g_systemDpi) return g_systemDpi;
    if (pGetDpiForSystem) {
        g_systemDpi = pGetDpiForSystem();
    } else {
        HDC dc = GetDC(nullptr);
        g_systemDpi = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
    }
    if (g_systemDpi == 0) g_systemDpi = 96;
    return g_systemDpi;
}

UINT MonitorDpi(HMONITOR mon) {
    if (g_perMonitor && pGetDpiForMonitor && mon) {
        UINT x = 0, y = 0;
        if (SUCCEEDED(pGetDpiForMonitor(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x > 0) return x;
    }
    return SystemDpi();
}

UINT WindowDpi(HWND hwnd) {
    if (pGetDpiForWindow && hwnd) {
        UINT d = pGetDpiForWindow(hwnd);
        if (d) return d;
    }
    if (g_perMonitor && hwnd) return MonitorDpi(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
    return SystemDpi();
}

void AdjustRectForDpi(RECT *rc, DWORD style, DWORD exStyle, UINT dpi) {
    if (pAdjustWindowRectExForDpi && pAdjustWindowRectExForDpi(rc, style, FALSE, exStyle, dpi)) return;
    // 老系统：非客户区按系统 DPI 绘制，AdjustWindowRectEx 的结果就是实际大小
    AdjustWindowRectEx(rc, style, FALSE, exStyle);
}

void EnableNonClientScaling(HWND hwnd) {
    if (pEnableNonClientDpiScaling) pEnableNonClientDpiScaling(hwnd);
}

bool ExcludeFromCapture(HWND hwnd) {
    // WDA_EXCLUDEFROMCAPTURE = 0x11，Win10 2004（19041）起支持；更早的系统调用会失败
    if (!(g_ver.dwMajorVersion >= 10 && g_ver.dwBuildNumber >= 19041)) return false;
    return SetWindowDisplayAffinity(hwnd, 0x00000011) != 0;
}

void SetDarkTitleBar(HWND hwnd, bool dark) {
    if (!(g_ver.dwMajorVersion >= 10 && g_ver.dwBuildNumber >= 17763)) return;
    BOOL v = dark ? TRUE : FALSE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE（20H1+），19 为 1809–1909 的旧值
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &v, sizeof v))) DwmSetWindowAttribute(hwnd, 19, &v, sizeof v);
}

}  // namespace api
}  // namespace ls
