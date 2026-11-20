#!/bin/bash
# 交叉编译 Windows 版（macOS 或 Linux 主机）：x86（Win7+）、x64（Win7+）、ARM64（Win10+）。
# 用法：scripts/build-windows.sh [i686|x86_64|aarch64 ...] [--debug] [--tests]
#   产物：dist/windows/LemonShot-x86.exe、LemonShot-x64.exe、LemonShot-arm64.exe
#   --tests 另外编出内核单元测试 dist/windows/tests/lemonshot_core_tests-<架构>.exe（静态链接，拷到 Windows 上直接运行）
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="$("$ROOT/scripts/fetch-windows-toolchain.sh")"
export LLVM_MINGW
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
TYPE=MinSizeRel
TESTS=0
ARCHS=()
for a in "$@"; do
  case "$a" in
    --debug) TYPE=Debug ;;
    --tests) TESTS=1 ;;
    i686|x86_64|aarch64) ARCHS+=("$a") ;;
  esac
done
[ ${#ARCHS[@]} -eq 0 ] && ARCHS=(i686 x86_64 aarch64)
mkdir -p "$ROOT/dist/windows"
for ARCH in "${ARCHS[@]}"; do
  case "$ARCH" in i686) N=x86 ;; x86_64) N=x64 ;; aarch64) N=arm64 ;; esac
  B="${LEMONSHOT_BUILD_DIR:-$ROOT/build}/win-$ARCH-$TYPE"
  cmake -S "$ROOT/lemon-shot-windows" -B "$B" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/lemon-shot-windows/cmake/llvm-mingw.cmake" \
    -DLS_ARCH="$ARCH" -DLLVM_MINGW="$LLVM_MINGW" -DCMAKE_BUILD_TYPE="$TYPE" >/dev/null
  cmake --build "$B" -j "$JOBS"
  cp "$B/LemonShot.exe" "$ROOT/dist/windows/LemonShot-$N.exe"
  "$ROOT/scripts/check-win-imports.sh" "$ROOT/dist/windows/LemonShot-$N.exe" "$ARCH"
  ls -l "$ROOT/dist/windows/LemonShot-$N.exe" | awk '{printf "%s %.1f KB\n", $9, $5/1024}'
  if [ "$TESTS" = 1 ]; then
    TB="${LEMONSHOT_BUILD_DIR:-$ROOT/build}/win-core-tests-$ARCH"
    cmake -S "$ROOT/lemon-shot-core" -B "$TB" -G "Unix Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE="$ROOT/lemon-shot-windows/cmake/llvm-mingw.cmake" \
      -DLS_ARCH="$ARCH" -DLLVM_MINGW="$LLVM_MINGW" -DCMAKE_BUILD_TYPE=MinSizeRel -DLEMONSHOT_CORE_TESTS=ON >/dev/null
    cmake --build "$TB" -j "$JOBS"
    mkdir -p "$ROOT/dist/windows/tests"
    cp "$TB/lemonshot_core_tests.exe" "$ROOT/dist/windows/tests/lemonshot_core_tests-$N.exe"
    "$ROOT/scripts/check-win-imports.sh" "$ROOT/dist/windows/tests/lemonshot_core_tests-$N.exe" "$ARCH"
  fi
done
