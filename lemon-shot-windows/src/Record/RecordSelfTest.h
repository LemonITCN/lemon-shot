// 自测（spec 1.1）：录屏设置面板 record-setup、控制条 record-bar、按键浮层 record-keys、录音面板 audio。
// 场景与 Mac 端 RecordSelfTest.swift 一致。由 SelfTest.cpp 调用，传入它的离屏渲染与写文件函数。
#pragma once

#include <functional>

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {
namespace rec {

typedef std::function<void(Canvas &, ID2D1RenderTarget *)> DrawFn;
typedef std::function<ls_image(float w, float h, const DrawFn &draw)> RenderFn;
typedef std::function<bool(const char *name, ls_image &img)> WriteFn;

// screen：1280×800 假屏幕（像素 = 逻辑 × scale）；selected：新建一个选区为 (200,150,640,400) 的会话（调用方负责假窗口等）
void SelfTest(const ls_image &screen, float scale, float screenW, float screenH, const std::function<ls_session *()> &selected,
              const RenderFn &render, const WriteFn &write);

}  // namespace rec
}  // namespace ls
