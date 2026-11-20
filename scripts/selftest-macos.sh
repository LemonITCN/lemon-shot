#!/bin/bash
# 在本机跑 Mac 版离屏自测：scripts/selftest-macos.sh <输出目录> [--scale 2] [--theme dark] [--lang en]
# 直接运行构建目录里的应用会被系统顺手登记，跑完立即注销，本机始终只登记 /Applications 里那一份。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/build/LemonShot.noindex/LemonShot.app"
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
[ $# -ge 1 ] || { echo "用法：$0 <输出目录> [自测参数]" >&2; exit 2; }
[ -x "$APP/Contents/MacOS/LemonShot" ] || { echo "先运行 scripts/build-macos.sh" >&2; exit 1; }
OUTDIR="$1"; shift
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"
STATUS=0
"$APP/Contents/MacOS/LemonShot" --selftest "$OUTDIR" "$@" || STATUS=$?
"$LSREGISTER" -u "$APP" >/dev/null 2>&1 || true
ls "$OUTDIR"/*.png 2>/dev/null | wc -l | xargs echo "截图："
exit $STATUS
