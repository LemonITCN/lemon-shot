// 第二期自测状态（spec 1.1）：ocr、translate、longshot、shell；另写 ocr-check.txt（离屏跑一遍真实识别流程）。
// 假数据与 Mac 端 OCR/Phase2SelfTest.swift 一致。
#pragma once

#include <functional>
#include <string>

#include "lemonshot.h"

namespace ls {
namespace selftest {

// screen：1280×800 逻辑点的假屏幕（scale 倍像素）；write 写一张图（接管并释放 img），返回是否成功
void RunPhase2(const ls_image &screen, float scale, const std::wstring &dir, const std::function<bool(const char *, ls_image &)> &write);

}  // namespace selftest
}  // namespace ls
