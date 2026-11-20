// 图片编码/解码（WIC）、PDF（嵌入 PNG 的压缩数据，无损）、剪贴板图片读写。
#pragma once

#include <windows.h>

#include <string>

#include "lemonshot.h"

namespace ls {

// format：png jpg tiff bmp pdf（webp/heic 由调用方先换成 png）。scale 只用于 PDF 页面尺寸（逻辑点）
bool EncodeImage(const ls_image &img, const std::string &format, int jpegQuality, float scale, std::string &out);
bool SaveImageFile(const ls_image &img, const std::wstring &path, const std::string &format, int jpegQuality, float scale);
// 解码成预乘 BGRA（out 用 ls_image_free）
bool DecodeImage(const void *data, size_t size, ls_image &out);
bool LoadImageFile(const std::wstring &path, ls_image &out);

// 剪贴板：CF_DIBV5 + 注册格式 "PNG"
bool CopyImageToClipboard(HWND owner, const ls_image &img);
// 读剪贴板里的图片（PNG → DIBV5/DIB → BITMAP）
bool ReadClipboardImage(HWND owner, ls_image &out);
bool ReadClipboardText(HWND owner, std::wstring &out);

bool ImageHasAlpha(const ls_image &img);
// 复制一份（ls_image_alloc）
ls_image CloneImage(const ls_image &img);

}  // namespace ls
