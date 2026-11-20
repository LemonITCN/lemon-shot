#include "Util/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "Util/Util.h"

namespace ls {

static bool g_enabled = true;
static bool g_console = false;
static std::wstring g_path;

void LogInit() {
    wchar_t env[8] = {};
    if (GetEnvironmentVariableW(L"LEMONSHOT_LOG", env, 8) > 0 && env[0] == L'0') g_enabled = false;
    g_path = PathJoin(TempDir(), L"LemonShot.log");
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fad) && (fad.nFileSizeHigh || fad.nFileSizeLow > 1024 * 1024))
        DeleteFileW(g_path.c_str());
}

void LogEnable(bool on) { g_enabled = on; }
void LogToConsole(bool on) { g_console = on; }

void Log(const char *fmt, ...) {
    if (!g_enabled && !g_console) return;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof msg - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof msg - 3) n = (int)sizeof msg - 3;
    msg[n++] = '\r';
    msg[n++] = '\n';
    msg[n] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[48];
    int hn = snprintf(head, sizeof head, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (g_console) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out && out != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(out, msg, (DWORD)n, &w, nullptr);
        }
    }
    if (!g_enabled || g_path.empty()) return;
    HANDLE h = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, head, (DWORD)hn, &w, nullptr);
    WriteFile(h, msg, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

}  // namespace ls
