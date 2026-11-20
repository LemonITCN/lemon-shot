#!/bin/bash
# 编译并运行内核单元测试（macOS / Linux 主机）。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="${LEMONSHOT_BUILD_DIR:-$ROOT/build}/core-tests"
cmake -S "$ROOT/lemon-shot-core" -B "$B" -DLEMONSHOT_CORE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$B" -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
"$B/lemonshot_core_tests" "$ROOT/lemon-shot-core/tests/fixtures"
