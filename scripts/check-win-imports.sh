#!/bin/bash
# 检查 exe 的导入表：x86/x64 版必须能在 Windows 7 SP1 上加载。
# 规则：只允许 Win7 自带的 DLL；已知 Win8+ 才有的函数一律报错（这些必须运行时 GetProcAddress）。
set -euo pipefail
EXE="$1"; ARCH="${2:-x86_64}"
if [ -z "${LLVM_MINGW:-}" ]; then
  case "$(uname -s)" in
    Darwin) LLVM_MINGW="$HOME/Library/Caches/LemonShot/build-tools/llvm-mingw" ;;
    *) LLVM_MINGW="${XDG_CACHE_HOME:-$HOME/.cache}/LemonShot/build-tools/llvm-mingw" ;;
  esac
fi
DUMP="$("$LLVM_MINGW/bin/llvm-objdump" -p "$EXE")"
DLLS="$(printf '%s\n' "$DUMP" | sed -n 's/.*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u)"
ALLOWED="kernel32.dll user32.dll gdi32.dll advapi32.dll shell32.dll shlwapi.dll ole32.dll oleaut32.dll comctl32.dll comdlg32.dll msvcrt.dll winmm.dll winhttp.dll dwmapi.dll uxtheme.dll d2d1.dll dwrite.dll windowscodecs.dll imm32.dll version.dll d3d11.dll dxgi.dll crypt32.dll bcrypt.dll psapi.dll setupapi.dll ws2_32.dll"
FAIL=0
for d in $DLLS; do
  case "$d" in api-ms-win-crt-*|ucrtbase.dll) [ "$ARCH" = aarch64 ] && continue ;; esac
  if ! printf ' %s ' "$ALLOWED" | grep -q " $d "; then echo "不允许直接导入 $d（Win7 没有或不保证有，改为运行时加载）"; FAIL=1; fi
done
DENY="GetSystemTimePreciseAsFileTime SetProcessDpiAwarenessContext GetDpiForWindow GetDpiForSystem GetDpiForMonitor SetProcessDpiAwareness GetSystemMetricsForDpi AdjustWindowRectExForDpi EnableNonClientDpiScaling SetThreadDpiAwarenessContext GetThreadDpiAwarenessContext SetThreadDescription CreateFile2 WaitOnAddress WakeByAddressSingle WakeByAddressAll PrefetchVirtualMemory GetCurrentThreadStackLimits CreateDXGIFactory2 D2D1CreateDevice RoInitialize RoGetActivationFactory WindowsCreateString GetPointerType GetPointerInfo RegisterPointerDeviceNotifications EnableMouseInPointer SetWindowCompositionAttribute DwmSetIconicThumbnail AddDllDirectory SetDefaultDllDirectories GetPackageFamilyName"
for f in $DENY; do
  if printf '%s\n' "$DUMP" | grep -E "^\s+[0-9]+\s+$f$|\s$f$" >/dev/null; then echo "直接导入了 Win8+ 函数 $f（改为运行时加载）"; FAIL=1; fi
done
if [ "$FAIL" = 1 ] && [ "$ARCH" != aarch64 ]; then exit 1; fi
echo "导入检查通过：$(basename "$EXE")"
