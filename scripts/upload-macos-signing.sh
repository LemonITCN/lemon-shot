#!/bin/bash
# 把本机的 Mac 签名证书（scripts/.sign-identity 指定的那一张，只这一张）导出成 .p12，写进仓库的 Actions Secrets：
#   MACOS_CERTIFICATE_P12（base64）、MACOS_CERTIFICATE_PASSWORD（每次随机生成）、MACOS_SIGN_IDENTITY（证书 SHA-1）
# 这样 GitHub 打的包和本机构建是同一个签名，系统授权（屏幕录制、辅助功能等）在更新后保留。
# 上传前先在临时钥匙串里用导出的证书实际签一次，签名要求必须与 lemon-shot-macos/Packaging/designated-requirement.txt 一致。
# 导出私钥时系统会弹框要钥匙串密码（由本人在弹框里输入）。证书续签（名字不变）后重跑本脚本即可，授权不受影响。
# 用法：scripts/upload-macos-signing.sh [仓库，默认 LemonITCN/lemon-shot]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="${1:-LemonITCN/lemon-shot}"
SHA="$(head -1 "$ROOT/scripts/.sign-identity" 2>/dev/null | tr -d '[:space:]' | tr 'a-f' 'A-F')"
[[ "$SHA" =~ ^[0-9A-F]{40}$ ]] || { echo "scripts/.sign-identity 里应是证书的 SHA-1" >&2; exit 1; }
EXPECT="$(cat "$ROOT/lemon-shot-macos/Packaging/designated-requirement.txt")"
WWDR="$ROOT/lemon-shot-macos/Packaging/certs/AppleWWDRCAG3.cer"

TMP="$(mktemp -d)"
chmod 700 "$TMP"
KC="$TMP/check.keychain-db"
cleanup() { security delete-keychain "$KC" >/dev/null 2>&1 || true; rm -rf "$TMP"; }
trap cleanup EXIT

PASS="$(openssl rand -base64 33 | tr -d '\n/+=')"
P12="$TMP/sign.p12"

# 只导出 SHA-1 对应的那一个身份（security export 会把钥匙串里所有证书一起导出，不能用）
cat > "$TMP/export.swift" <<'SWIFT'
import CryptoKit
import Foundation
import Security
let args = CommandLine.arguments
let want = args[1], pass = args[2], out = args[3]
var found: CFTypeRef?
let query: [String: Any] = [kSecClass as String: kSecClassIdentity, kSecMatchLimit as String: kSecMatchLimitAll, kSecReturnRef as String: true]
guard SecItemCopyMatching(query as CFDictionary, &found) == errSecSuccess, let ids = found as? [SecIdentity] else {
    FileHandle.standardError.write("钥匙串里没有可用的签名身份\n".data(using: .utf8)!); exit(1)
}
for id in ids {
    var cert: SecCertificate?
    guard SecIdentityCopyCertificate(id, &cert) == errSecSuccess, let c = cert else { continue }
    let sha = Insecure.SHA1.hash(data: SecCertificateCopyData(c) as Data).map { String(format: "%02X", $0) }.joined()
    guard sha == want else { continue }
    var params = SecItemImportExportKeyParameters()
    params.version = UInt32(SEC_KEY_IMPORT_EXPORT_PARAMS_VERSION)
    params.passphrase = Unmanaged.passUnretained(pass as CFString as AnyObject)
    var data: CFData?
    let st = SecItemExport(id, .formatPKCS12, [], &params, &data)
    guard st == errSecSuccess, let d = data as Data? else {
        FileHandle.standardError.write("导出失败：\(st)\n".data(using: .utf8)!); exit(1)
    }
    FileManager.default.createFile(atPath: out, contents: d, attributes: [.posixPermissions: 0o600])
    exit(0)
}
FileHandle.standardError.write("钥匙串里找不到 SHA-1 为 \(want) 的签名身份\n".data(using: .utf8)!)
exit(1)
SWIFT
echo "导出证书（系统弹框时输入钥匙串密码并允许）…"
swiftc -O -o "$TMP/export" "$TMP/export.swift" 2>/dev/null
"$TMP/export" "$SHA" "$PASS" "$P12"

# 在临时钥匙串里用导出的证书签一次，核对签名要求
KCPASS="$(openssl rand -hex 16)"
security create-keychain -p "$KCPASS" "$KC"
security unlock-keychain -p "$KCPASS" "$KC"
security import "$P12" -k "$KC" -P "$PASS" -f pkcs12 -A >/dev/null
security import "$WWDR" -k "$KC" >/dev/null 2>&1 || true
N="$(security find-identity -v -p codesigning "$KC" | grep -c "$SHA" || true)"
[ "$N" = 1 ] || { echo "导出的 .p12 里没有可签名的身份" >&2; exit 1; }
mkdir -p "$TMP/Probe.app/Contents/MacOS"
cp /usr/bin/true "$TMP/Probe.app/Contents/MacOS/Probe"
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>cn.lemonit.shot</string><key>CFBundleExecutable</key><string>Probe</string></dict></plist>\n' > "$TMP/Probe.app/Contents/Info.plist"
codesign --force --keychain "$KC" --sign "$SHA" "$TMP/Probe.app" 2>/dev/null
GOT="$(codesign -d -r- "$TMP/Probe.app" 2>&1 | sed -n 's/^designated => //p')"
[ "$GOT" = "$EXPECT" ] || { echo "签名要求不一致：$GOT" >&2; exit 1; }
echo "核对通过：$GOT"

base64 -i "$P12" | gh secret set MACOS_CERTIFICATE_P12 --repo "$REPO"
printf '%s' "$PASS" | gh secret set MACOS_CERTIFICATE_PASSWORD --repo "$REPO"
gh secret set MACOS_SIGN_IDENTITY --repo "$REPO" --body "$SHA"
echo "已写入 $REPO 的 Secrets：MACOS_CERTIFICATE_P12、MACOS_CERTIFICATE_PASSWORD、MACOS_SIGN_IDENTITY"
