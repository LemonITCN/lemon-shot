#!/bin/bash
# 构建 Mac 版：arm64 与 x86_64 分别编译后 lipo 合成通用二进制，组装 .app。
# 用法：scripts/build-macos.sh [--debug] [--install] [--dmg]
#   产物：build/LemonShot.noindex/LemonShot.app（自测、打包用）；--dmg 另出 dist/macos/LemonShot-<版本>.dmg 与 .zip
#   .app 放在 .noindex 目录里：聚焦不索引这个目录，系统就不会把它当成第二个 LemonShot 登记进启动台和应用库
#   签名（决定系统权限能否在重新编译后保留）：
#     1. 环境变量 LEMONSHOT_SIGN_IDENTITY（证书名或 SHA-1；"-" 表示临时签名）
#     2. 本机文件 scripts/.sign-identity（不进仓库，内容同上）
#     3. 钥匙串里第一张 Developer ID Application，没有就第一张 Apple Development
#     4. 都没有才临时签名。临时签名的应用每次重新编译，屏幕录制等权限都会失效，所以装机（--install）和
#        LEMONSHOT_REQUIRE_SIGNATURE=1（GitHub 推送 main / 打标签）时不许临时签名。
#   签名身份锁定在 lemon-shot-macos/Packaging/designated-requirement.txt（当前是 LemonIT.CN 的 Apple Development 证书），
#   签出来不一致直接失败；GitHub 用的同一张证书由 scripts/upload-macos-signing.sh 写进仓库 Secrets。
#   --install：装到 /Applications/LemonShot.app 作为本机唯一一份（注销其他副本的登记）
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
APPDIR="$ROOT/build/LemonShot.noindex"
APP="$APPDIR/LemonShot.app"
LSREGISTER=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
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
# 旧版脚本把 .app 放在 dist/macos，被聚焦登记成第二个 LemonShot：注销并删掉
if [ -d "$OUT/LemonShot.app" ]; then "$LSREGISTER" -u "$OUT/LemonShot.app" >/dev/null 2>&1 || true; rm -rf "$OUT/LemonShot.app"; fi
rm -f "$ROOT/dist/.metadata_never_index"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$OUT"
lipo -create "${BINS[@]}" -output "$APP/Contents/MacOS/LemonShot"
if [ "$CONFIG" = release ]; then xcrun strip -x "$APP/Contents/MacOS/LemonShot"; fi
sed -e "s/__VERSION__/$VERSION/" -e "s/__BUILD__/$BUILD/" Packaging/Info.plist > "$APP/Contents/Info.plist"
cp -R Packaging/zh-Hans.lproj Packaging/en.lproj "$APP/Contents/Resources/"
[ -f Packaging/AppIcon.icns ] && cp Packaging/AppIcon.icns "$APP/Contents/Resources/"
mkdir -p "$APP/Contents/Resources/shared"
cp -R "$ROOT/lemon-shot-core/resources/"* "$APP/Contents/Resources/shared/"
rm -f "$APP/Contents/Resources/shared/README.md"
IDENTITY="${LEMONSHOT_SIGN_IDENTITY:-}"
if [ -z "$IDENTITY" ] && [ -f "$ROOT/scripts/.sign-identity" ]; then IDENTITY="$(head -1 "$ROOT/scripts/.sign-identity" | tr -d '[:space:]')"; fi
if [ -z "$IDENTITY" ]; then
  IDS="$(security find-identity -v -p codesigning 2>/dev/null || true)"
  IDENTITY="$(printf '%s\n' "$IDS" | sed -n 's/^ *[0-9]*) \([0-9A-F]\{40\}\) "Developer ID Application:.*/\1/p' | head -1)"
  if [ -z "$IDENTITY" ]; then
    # 多张开发证书时优先选组织为 LemonIT.CN 的那张，否则取第一张
    DEV="$(printf '%s\n' "$IDS" | sed -n 's/^ *[0-9]*) \([0-9A-F]\{40\}\) "\(Apple Development:.*\)"$/\1 \2/p')"
    while read -r HASH NAME; do
      [ -z "$HASH" ] && continue
      if security find-certificate -c "$NAME" -p 2>/dev/null | openssl x509 -noout -subject 2>/dev/null | grep -q "LemonIT"; then IDENTITY="$HASH"; break; fi
    done <<EOF_IDS
$DEV
EOF_IDS
    [ -z "$IDENTITY" ] && IDENTITY="$(printf '%s\n' "$DEV" | head -1 | cut -d' ' -f1)"
  fi
fi
[ -z "$IDENTITY" ] && IDENTITY="-"
# 签名身份锁定在 Packaging/designated-requirement.txt：系统按它记屏幕录制、辅助功能等授权，变了授权就全部失效。
# 本机构建与 GitHub 打包（证书在仓库 Secrets，scripts/upload-macos-signing.sh 写入）必须签成同一个，不一致直接报错。
EXPECT_DR="$(cat "$ROOT/lemon-shot-macos/Packaging/designated-requirement.txt")"
if [ "$IDENTITY" = "-" ]; then
  if [ "${LEMONSHOT_REQUIRE_SIGNATURE:-0}" = 1 ] || [ "$INSTALL" = 1 ]; then
    echo "没有签名证书：要装到本机或正式打包必须用锁定的证书签名（scripts/.sign-identity 或 LEMONSHOT_SIGN_IDENTITY）" >&2
    exit 1
  fi
  echo "警告：临时签名，系统授权每次重新编译都会失效，只能用来自测" >&2
fi
# 正式分发（Developer ID）才需要安全时间戳；临时签名和本地开发证书不联网取时间戳
TS=--timestamp=none
if [ "$IDENTITY" != "-" ]; then
  case "$(security find-identity -v -p codesigning 2>/dev/null | grep -F "$IDENTITY" || true)" in *"Developer ID Application"*) TS=--timestamp ;; esac
fi
echo "签名：$( [ "$IDENTITY" = "-" ] && echo 临时签名 || security find-identity -v -p codesigning | grep -F "$IDENTITY" | sed 's/.*"\(.*\)"/\1/' | head -1)"
codesign --force --options runtime $TS --entitlements Packaging/LemonShot.entitlements --sign "$IDENTITY" "$APP"
if [ "$IDENTITY" != "-" ]; then
  GOT_DR="$(codesign -d -r- "$APP" 2>&1 | sed -n 's/^designated => //p')"
  if [ "$GOT_DR" != "$EXPECT_DR" ]; then
    echo "签名身份和锁定的不一致，装上后系统授权会全部失效：" >&2
    echo "  实际：$GOT_DR" >&2
    echo "  锁定：$EXPECT_DR" >&2
    exit 1
  fi
  echo "签名要求与锁定的一致"
fi
echo "$APP"
du -sh "$APP" | cat
if [ "$DMG" = 1 ]; then
  DMG_PATH="$OUT/LemonShot-$VERSION.dmg"
  ZIP_PATH="$OUT/LemonShot-$VERSION.zip"
  STAGE="$APPDIR/dmg-stage"
  rm -rf "$STAGE" "$DMG_PATH" && mkdir -p "$STAGE"
  cp -R "$APP" "$STAGE/"
  ln -s /Applications "$STAGE/Applications"
  hdiutil create -volname "LemonShot" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 "$DMG_PATH" >/dev/null
  rm -rf "$STAGE"
  if [ "$IDENTITY" != "-" ]; then codesign --force $TS --sign "$IDENTITY" "$DMG_PATH"; fi
  rm -f "$ZIP_PATH"
  ditto -c -k --keepParent "$APP" "$ZIP_PATH"
  echo "$DMG_PATH"
  echo "$ZIP_PATH"
fi
# 构建目录里的副本只给打包和自测用，不登记，免得和装好的那份混在一起
"$LSREGISTER" -u "$APP" >/dev/null 2>&1 || true
if [ "$INSTALL" = 1 ]; then
  # 已装的那份签名不同（例如以前的临时签名包）：覆盖后授权会全部失效，先停下让人确认
  if [ -d "/Applications/LemonShot.app" ] && [ "${LEMONSHOT_FORCE_INSTALL:-0}" != 1 ]; then
    OLD_DR="$(codesign -d -r- "/Applications/LemonShot.app" 2>&1 | sed -n 's/^designated => //p')"
    if [ "$OLD_DR" != "$EXPECT_DR" ]; then
      echo "/Applications/LemonShot.app 的签名和这次不同，覆盖后屏幕录制等授权要全部重新给。确认要装：LEMONSHOT_FORCE_INSTALL=1 再运行" >&2
      exit 1
    fi
  fi
  rm -rf "/Applications/LemonShot.app" "/Applications/Lemon Shot.app"
  ditto "$APP" "/Applications/LemonShot.app"
  "$LSREGISTER" -f "/Applications/LemonShot.app" >/dev/null 2>&1 || true
  echo "已安装到 /Applications/LemonShot.app"
fi
