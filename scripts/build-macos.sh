#!/bin/bash
# 构建 Mac 版：arm64 与 x86_64 分别编译后 lipo 合成通用二进制，组装 .app。
# 用法：scripts/build-macos.sh [--debug] [--install] [--dmg]
#   产物：dist/macos/Lemon Shot.app；--dmg 另出 dist/macos/LemonShot-<版本>.dmg（拖到「应用程序」安装）
#   签名：设置 LEMONSHOT_SIGN_IDENTITY="Developer ID Application: ..." 用正式证书（带安全时间戳，可公证），否则临时签名。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG=release
INSTALL=0
DMG=0
for a in "$@"; do
  case "$a" in
    --debug) CONFIG=debug ;;
    --install) INSTALL=1 ;;
    --dmg) DMG=1 ;;
  esac
done
VERSION="$(sed -n 's/#define LEMONSHOT_VERSION "\(.*\)"/\1/p' "$ROOT/lemon-shot-core/src/internal.h")"
BUILD="$(date +%Y%m%d%H%M)"
SCRATCH="${LEMONSHOT_BUILD_DIR:-$ROOT/build/macos}"
OUT="$ROOT/dist/macos"
APP="$OUT/Lemon Shot.app"
cd "$ROOT/lemon-shot-macos"
# 不用 swift build：SwiftPM 会把部署目标抬到 macOS 12，这里直接调编译器以支持 macOS 11。
MINOS=11.0
SDK="$(xcrun --sdk macosx --show-sdk-path)"
CORE="$ROOT/lemon-shot-core"
if [ "$CONFIG" = release ]; then OPT_C=-Os; OPT_S=-Osize; else OPT_C="-O0 -g"; OPT_S="-Onone -g"; fi
BINS=()
for ARCH in arm64 x86_64; do
  W="$SCRATCH/$ARCH-$CONFIG"
  rm -rf "$W" && mkdir -p "$W/obj" "$W/module"
  printf 'module LemonShotCore {\n  header "%s"\n  header "%s"\n  export *\n}\n' "$CORE/include/lemonshot.h" "$CORE/include/lemonshot_extras.h" > "$W/module/module.modulemap"
  OBJS=()
  # 内核（启用 WebP：去掉 extras_optional.cpp 里的空实现）
  for f in "$CORE"/src/*.cpp; do
    o="$W/obj/$(basename "${f%.cpp}").o"
    xcrun clang++ -std=c++17 $OPT_C -target "$ARCH-apple-macos$MINOS" -isysroot "$SDK" -fno-exceptions -fno-rtti \
      -DLEMONSHOT_WITH_WEBP -I "$CORE/include" -I "$CORE/src" -c "$f" -o "$o" &
    OBJS+=("$o")
  done
  # WebP：libwebp 编码部分 + extras/webp.cpp（与 Package.swift 的 LemonShotWebP 目标一致）
  WEBP="$CORE/third_party/libwebp"
  mkdir -p "$W/obj/webp"
  xcrun clang++ -std=c++17 $OPT_C -target "$ARCH-apple-macos$MINOS" -isysroot "$SDK" -fno-exceptions -fno-rtti \
    -I "$CORE/include" -I "$WEBP" -I "$WEBP/src" -c "$CORE/extras/webp.cpp" -o "$W/obj/webp/ls_webp.o" &
  OBJS+=("$W/obj/webp/ls_webp.o")
  for f in "$WEBP"/src/enc/*.c "$WEBP"/src/dsp/*.c "$WEBP"/src/utils/*.c "$WEBP"/sharpyuv/*.c; do
    o="$W/obj/webp/$(basename "$(dirname "$f")")_$(basename "${f%.c}").o"
    xcrun clang $OPT_C -target "$ARCH-apple-macos$MINOS" -isysroot "$SDK" -w -I "$WEBP" -I "$WEBP/src" -c "$f" -o "$o" &
    OBJS+=("$o")
  done
  wait
  SWIFT_SOURCES=()
  while IFS= read -r -d '' f; do SWIFT_SOURCES+=("$f"); done < <(find Sources/LemonShot -name '*.swift' -print0)
  xcrun swiftc $OPT_S -target "$ARCH-apple-macos$MINOS" -sdk "$SDK" -module-name LemonShot -swift-version 5 \
    -I "$W/module" "${SWIFT_SOURCES[@]}" "${OBJS[@]}" -lc++ \
    -framework AppKit -framework Carbon -Xlinker -weak_framework -Xlinker ScreenCaptureKit \
    -Xlinker -dead_strip -o "$W/LemonShot"
  BINS+=("$W/LemonShot")
done
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
lipo -create "${BINS[@]}" -output "$APP/Contents/MacOS/LemonShot"
if [ "$CONFIG" = release ]; then xcrun strip -x "$APP/Contents/MacOS/LemonShot"; fi
sed -e "s/__VERSION__/$VERSION/" -e "s/__BUILD__/$BUILD/" Packaging/Info.plist > "$APP/Contents/Info.plist"
cp -R Packaging/zh-Hans.lproj Packaging/en.lproj "$APP/Contents/Resources/"
[ -f Packaging/AppIcon.icns ] && cp Packaging/AppIcon.icns "$APP/Contents/Resources/"
mkdir -p "$APP/Contents/Resources/shared"
cp -R "$ROOT/lemon-shot-core/resources/"* "$APP/Contents/Resources/shared/"
rm -f "$APP/Contents/Resources/shared/README.md"
IDENTITY="${LEMONSHOT_SIGN_IDENTITY:--}"
if [ "$IDENTITY" = "-" ]; then TS=--timestamp=none; else TS=--timestamp; fi
codesign --force --options runtime $TS --entitlements Packaging/LemonShot.entitlements --sign "$IDENTITY" "$APP"
echo "$APP"
du -sh "$APP" | cat
if [ "$DMG" = 1 ]; then
  DMG_PATH="$OUT/LemonShot-$VERSION.dmg"
  STAGE="$SCRATCH/dmg-stage"
  rm -rf "$STAGE" "$DMG_PATH" && mkdir -p "$STAGE"
  cp -R "$APP" "$STAGE/"
  ln -s /Applications "$STAGE/Applications"
  hdiutil create -volname "Lemon Shot" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 "$DMG_PATH" >/dev/null
  rm -rf "$STAGE"
  if [ "$IDENTITY" != "-" ]; then codesign --force --timestamp --sign "$IDENTITY" "$DMG_PATH"; fi
  echo "$DMG_PATH"
fi
if [ "$INSTALL" = 1 ]; then
  rm -rf "/Applications/Lemon Shot.app"
  cp -R "$APP" "/Applications/Lemon Shot.app"
  echo "已安装到 /Applications/Lemon Shot.app"
fi
