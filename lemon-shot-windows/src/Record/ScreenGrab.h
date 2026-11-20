// 抓屏（spec 8.3）：Win8+ DXGI Desktop Duplication（按显示器，ACCESS_LOST 时重建，自己画光标），
// Win7 或 DXGI 不可用（旋转屏、远程桌面、显卡不支持等）时用 GDI BitBlt(CAPTUREBLT) 抓区域。
// 只在采集线程上使用。结果是区域大小的 BGRA（alpha 不保证为 255）。
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

namespace ls {
namespace rec {

class ScreenGrabber {
public:
    ScreenGrabber() = default;
    ~ScreenGrabber();
    ScreenGrabber(const ScreenGrabber &) = delete;
    ScreenGrabber &operator=(const ScreenGrabber &) = delete;

    // region：屏幕像素（虚拟桌面坐标），必须在 mon 内
    bool Open(HMONITOR mon, RECT region, bool cursor);
    // 抓一帧（含光标）到内部缓冲；失败返回 false（缓冲保留上一帧）
    bool Grab();
    const uint8_t *Pixels() const { return (const uint8_t *)bits_; }
    int Stride() const { return w_ * 4; }
    int Width() const { return w_; }
    int Height() const { return h_; }
    void Close();

private:
    bool OpenDxgi();
    void CloseDxgi(bool keepDevice);
    bool Duplicate();
    bool GrabDxgi(bool &changed);
    bool GrabGdi();
    void DrawCursor();

    HMONITOR mon_ = nullptr;
    RECT region_ = {};
    int w_ = 0, h_ = 0;
    bool cursor_ = true;

    // 合成缓冲：DIB 段（光标用 DrawIconEx 画上去）
    HDC dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
    void *bits_ = nullptr;
    // DXGI 路径：不含光标的干净画面
    std::vector<uint8_t> clean_;
    bool haveClean_ = false;
    RECT lastCursor_ = {};  // 上一帧光标覆盖的区域（区域坐标），恢复用

    bool dxgi_ = false;
    void *factory_ = nullptr;  // IDXGIFactory1
    void *output_ = nullptr;   // IDXGIOutput1
    void *device_ = nullptr;   // ID3D11Device
    void *context_ = nullptr;  // ID3D11DeviceContext
    void *dupl_ = nullptr;     // IDXGIOutputDuplication
    void *staging_ = nullptr;  // ID3D11Texture2D
    int stagingW_ = 0, stagingH_ = 0;
    RECT outputRect_ = {};     // 显示器桌面坐标
    int64_t failSince_ = 0;    // DXGI 连续失败开始时刻
    int64_t lastRetry_ = 0;

    // 光标缓存
    HCURSOR lastCursorHandle_ = nullptr;
    POINT hotspot_ = {};
    SIZE cursorSize_ = {};
};

}  // namespace rec
}  // namespace ls
