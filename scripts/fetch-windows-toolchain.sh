#!/bin/bash
# 下载并组装 Windows 交叉编译工具链（llvm-mingw）。macOS 与 Linux（x86_64 / aarch64）主机都可用。
# x86 / x64 换成 msvcrt 运行库与头文件（Win7 原生自带 msvcrt.dll，不依赖 UCRT 更新）；
# ARM64 保留 UCRT（Windows on ARM 只有 Win10+，系统自带 UCRT）。
# 用法：scripts/fetch-windows-toolchain.sh   结果（打印到标准输出）：$LEMONSHOT_TOOLS/llvm-mingw
set -euo pipefail
VER="${LLVM_MINGW_VERSION:-20261117}"
case "$(uname -s)" in
  Darwin)
    DEFAULT_TOOLS="$HOME/Library/Caches/LemonShot/build-tools"
    HOST_NAME="ucrt-macos-universal" ;;
  Linux)
    DEFAULT_TOOLS="${XDG_CACHE_HOME:-$HOME/.cache}/LemonShot/build-tools"
    case "$(uname -m)" in
      aarch64|arm64) HOST_NAME="ucrt-ubuntu-22.04-aarch64" ;;
      *) HOST_NAME="ucrt-ubuntu-22.04-x86_64" ;;
    esac ;;
  *) echo "不支持的主机系统：$(uname -s)" >&2; exit 1 ;;
esac
TOOLS="${LEMONSHOT_TOOLS:-$DEFAULT_TOOLS}"
DEST="$TOOLS/llvm-mingw"
STAMP="$DEST/.lemonshot-$VER"
BASE="https://github.com/mstorsjo/llvm-mingw/releases/download/$VER"
HOST_PKG="llvm-mingw-$VER-$HOST_NAME.tar.xz"
CRT_PKG="llvm-mingw-$VER-msvcrt-ubuntu-22.04-x86_64.tar.xz"   # 只取其中与主机无关的目标库和头文件

# GNU tar 解压时要显式打开通配符匹配，bsdtar 默认就支持（空数组展开写法兼容 macOS 自带的 bash 3.2）
TAR_WILD=()
TAR_VERSION="$(tar --version 2>/dev/null || true)"
case "$TAR_VERSION" in *GNU*) TAR_WILD=(--wildcards) ;; esac

fetch() {
  local f="$1"
  if [ ! -s "$TOOLS/downloads/$f" ]; then
    echo "下载 $f" >&2
    local n=0
    # 网络不稳时断点续传，最多重试 60 次
    until curl -fL --http1.1 --retry 5 --retry-all-errors --retry-delay 3 -C - -o "$TOOLS/downloads/$f.part" "$BASE/$f" >&2; do
      n=$((n + 1))
      if [ "$n" -ge 60 ]; then echo "下载失败：$f" >&2; exit 1; fi
      sleep 3
    done
    mv "$TOOLS/downloads/$f.part" "$TOOLS/downloads/$f"
  fi
}

# x86/x64 必须用 msvcrt 包自己的头文件（_mingw.h 里默认运行库版本 0x600，UCRT 包是 0xE00）
fix_msvcrt_headers() {
  local tmp="$TOOLS/work-hdr-$$"
  rm -rf "$tmp" && mkdir -p "$tmp"
  tar -xJf "$TOOLS/downloads/$CRT_PKG" -C "$tmp" ${TAR_WILD[@]+"${TAR_WILD[@]}"} '*/generic-w64-mingw32/include'
  rm -rf "$DEST/generic-msvcrt-w64-mingw32"
  mkdir -p "$DEST/generic-msvcrt-w64-mingw32"
  mv "$tmp"/*/generic-w64-mingw32/include "$DEST/generic-msvcrt-w64-mingw32/include"
  rm -rf "$tmp"
  for t in i686-w64-mingw32 x86_64-w64-mingw32; do
    rm -f "$DEST/$t/include"
    ln -s ../generic-msvcrt-w64-mingw32/include "$DEST/$t/include"
  done
}

if [ -f "$STAMP" ]; then
  if [ ! -d "$DEST/generic-msvcrt-w64-mingw32" ]; then fix_msvcrt_headers; fi
  echo "$DEST"
  exit 0
fi

mkdir -p "$TOOLS/downloads"
fetch "$HOST_PKG"
fetch "$CRT_PKG"
WORK="$TOOLS/work-$$"
rm -rf "$WORK" && mkdir -p "$WORK"
tar -xJf "$TOOLS/downloads/$HOST_PKG" -C "$WORK"
tar -xJf "$TOOLS/downloads/$CRT_PKG" -C "$WORK" ${TAR_WILD[@]+"${TAR_WILD[@]}"} '*/i686-w64-mingw32' '*/x86_64-w64-mingw32'
HOST_DIR="$WORK/llvm-mingw-$VER-$HOST_NAME"
CRT_DIR="$WORK/llvm-mingw-$VER-msvcrt-ubuntu-22.04-x86_64"
for t in i686-w64-mingw32 x86_64-w64-mingw32; do
  rm -rf "$HOST_DIR/$t"
  mv "$CRT_DIR/$t" "$HOST_DIR/$t"
done
rm -rf "$DEST"
mv "$HOST_DIR" "$DEST"
rm -rf "$WORK"
fix_msvcrt_headers
if [ "$(uname -s)" = Darwin ]; then xattr -dr com.apple.quarantine "$DEST" 2>/dev/null || true; fi
touch "$STAMP"
echo "$DEST"
