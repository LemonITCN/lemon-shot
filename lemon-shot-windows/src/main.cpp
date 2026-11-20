// Lemon Shot Windows 入口：解析命令行，初始化各模块，进入托盘应用或自测。
#include <windows.h>
#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>

#include <string>

#include "App/App.h"
#include "App/Hotkeys.h"
#include "SelfTest/SelfTest.h"
#include "Settings/I18n.h"
#include "Settings/Icons.h"
#include "Settings/Settings.h"
#include "Settings/Theme.h"
#include "UI/Gfx.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/Log.h"
#include "Util/Util.h"

using namespace ls;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    LogInit();
    api::Init();

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring selftestDir, theme, lang;
    float scale = 2;  // 自测默认 2 倍（与 Mac 端一致）
    bool selftest = false;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        bool hasNext = i + 1 < argc;
        if (a == L"--selftest" && hasNext) {
            selftest = true;
            selftestDir = argv[++i];
        } else if (a == L"--scale" && hasNext) {
            scale = (float)_wtof(argv[++i]);
        } else if (a == L"--theme" && hasNext) {
            theme = argv[++i];
        } else if (a == L"--lang" && hasNext) {
            lang = argv[++i];
        }
    }
    if (argv) LocalFree(argv);

    if (selftest) {
        // 自测：不创建任何窗口；ssh 会话里输出到父进程控制台
        if (AttachConsole((DWORD)-1 /* ATTACH_PARENT_PROCESS */)) LogToConsole(true);
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
        settings::Init(true);
        theme::Init();
        theme::ForceMode(theme == L"dark" ? 2 : 1);
        std::string l = Narrow(lang);
        i18n::Init(l.empty() ? "zh-Hans" : l.c_str());
        if (!gfx::Init()) return 3;
        icons::Init();
        hotkeys::Init();
        if (!(scale >= 1 && scale <= 4)) scale = 2;
        int rc = selftest::Run(selftestDir, scale);
        gfx::Shutdown();
        CoUninitialize();
        return rc;
    }

    api::SetupDpiAwareness();
    if (FAILED(OleInitialize(nullptr))) return 2;
    settings::Init(false);
    theme::Init();
    i18n::Init(nullptr);
    if (!gfx::Init()) {
        MessageBoxW(nullptr, L"Direct2D is not available on this system.", L"Lemon Shot", MB_ICONERROR);
        return 3;
    }
    icons::Init();
    hotkeys::Init();
    RegisterWindowClass(inst);
    int rc = app::Run(inst);
    gfx::Shutdown();
    OleUninitialize();
    return rc;
}
