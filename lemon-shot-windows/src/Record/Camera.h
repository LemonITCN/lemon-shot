// 摄像头画中画（spec 8.2）：区域右下角，圆形或圆角矩形（正方形、圆角 18%），直径 = 区域短边 22%，外圈 2pt 白边，
// 可拖动，进画面。取帧：Media Foundation IMFSourceReader（单独线程，输出 RGB32），没有 MF 时调用方不打开。
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "UI/Gfx.h"
#include "UI/Window.h"

namespace ls {
namespace rec {

class CameraWindow : public Window {
public:
    struct Shared;  // 与取帧线程共享的数据（见 Camera.cpp）
    std::function<void()> onFailed;  // 打不开摄像头（界面线程回调）
    ~CameraWindow() override;
    bool Open(const std::string &deviceId, RECT region, UINT dpi, bool rounded);
    void Close();

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void Render();
    Shared *shared_ = nullptr;
    HANDLE thread_ = nullptr;
    LayeredSurface surface_;
    std::vector<uint8_t> frame_;
    int fw_ = 0, fh_ = 0;
    int size_ = 0;  // 像素边长
    UINT dpi_ = 96;
    bool rounded_ = false;
    POINT pos_ = {};
    bool shown_ = false;
    bool dragging_ = false;
    POINT dragOffset_ = {};
};

}  // namespace rec
}  // namespace ls
