#include "Record/ScreenGrab.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>

#include "Record/MF.h"
#include "Record/RecordUtil.h"
#include "Util/Api.h"
#include "Util/Log.h"

namespace ls {
namespace rec {

namespace {

typedef HRESULT(WINAPI *CreateDXGIFactory1Fn)(REFIID, void **);
typedef HRESULT(WINAPI *D3D11CreateDeviceFn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                             UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

CreateDXGIFactory1Fn g_createFactory = nullptr;
D3D11CreateDeviceFn g_createDevice = nullptr;
bool g_dxLoaded = false;

bool LoadDx() {
    if (g_dxLoaded) return g_createFactory && g_createDevice;
    g_dxLoaded = true;
    HMODULE dxgi = mf::LoadSystemDll(L"dxgi.dll");
    HMODULE d3d = mf::LoadSystemDll(L"d3d11.dll");
    if (dxgi)
        g_createFactory = reinterpret_cast<CreateDXGIFactory1Fn>(reinterpret_cast<void *>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
    if (d3d)
        g_createDevice = reinterpret_cast<D3D11CreateDeviceFn>(reinterpret_cast<void *>(GetProcAddress(d3d, "D3D11CreateDevice")));
    return g_createFactory && g_createDevice;
}

template <class T>
void SafeRelease(void *&p) {
    if (p) static_cast<T *>(p)->Release();
    p = nullptr;
}

}  // namespace

ScreenGrabber::~ScreenGrabber() { Close(); }

bool ScreenGrabber::Open(HMONITOR mon, RECT region, bool cursor) {
    Close();
    mon_ = mon;
    region_ = region;
    w_ = region.right - region.left;
    h_ = region.bottom - region.top;
    cursor_ = cursor;
    if (w_ <= 0 || h_ <= 0) return false;
    BITMAPINFO bi;
    std::memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w_;
    bi.bmiHeader.biHeight = -h_;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc_ = CreateCompatibleDC(nullptr);
    if (!dc_) return false;
    dib_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!dib_ || !bits_) {
        Close();
        return false;
    }
    oldBmp_ = SelectObject(dc_, dib_);
    // Win8+ 先试 DXGI；第一帧总是用 GDI 抓，保证一开始就有画面
    dxgi_ = api::IsWin8OrLater() && OpenDxgi();
    GrabGdi();
    if (dxgi_) {
        clean_.assign((size_t)w_ * h_ * 4, 0);
        std::memcpy(clean_.data(), bits_, clean_.size());
        haveClean_ = true;
    }
    LS_LOG("record: 抓屏方式 %s，区域 %ldx%ld+%ld+%ld", dxgi_ ? "DXGI" : "GDI", (long)w_, (long)h_, (long)region.left,
           (long)region.top);
    return true;
}

void ScreenGrabber::Close() {
    CloseDxgi(false);
    dxgi_ = false;
    if (dc_) {
        if (oldBmp_) SelectObject(dc_, oldBmp_);
        DeleteDC(dc_);
    }
    if (dib_) DeleteObject(dib_);
    dc_ = nullptr;
    dib_ = nullptr;
    oldBmp_ = nullptr;
    bits_ = nullptr;
    clean_.clear();
    haveClean_ = false;
    lastCursorHandle_ = nullptr;
}

bool ScreenGrabber::OpenDxgi() {
    if (!LoadDx()) return false;
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(g_createFactory(__uuidof(IDXGIFactory1), (void **)&factory)) || !factory) return false;
    IDXGIAdapter1 *found = nullptr;
    IDXGIOutput *foundOut = nullptr;
    DXGI_OUTPUT_DESC od;
    std::memset(&od, 0, sizeof od);
    for (UINT i = 0; !found; ++i) {
        IDXGIAdapter1 *ad = nullptr;
        if (factory->EnumAdapters1(i, &ad) == DXGI_ERROR_NOT_FOUND || !ad) break;
        for (UINT j = 0;; ++j) {
            IDXGIOutput *out = nullptr;
            if (ad->EnumOutputs(j, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
            DXGI_OUTPUT_DESC d;
            if (SUCCEEDED(out->GetDesc(&d)) && d.Monitor == mon_) {
                od = d;
                foundOut = out;
                found = ad;
                break;
            }
            out->Release();
        }
        if (!found) ad->Release();
    }
    factory_ = factory;
    if (!found) {
        LS_LOG("record: DXGI 找不到该显示器");
        CloseDxgi(false);
        return false;
    }
    IDXGIOutput1 *out1 = nullptr;
    HRESULT hr = foundOut->QueryInterface(__uuidof(IDXGIOutput1), (void **)&out1);
    foundOut->Release();
    if (FAILED(hr) || !out1) {
        found->Release();
        CloseDxgi(false);
        return false;
    }
    output_ = out1;
    outputRect_ = od.DesktopCoordinates;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    hr = g_createDevice(found, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    found->Release();
    if (FAILED(hr) || !dev || !ctx) {
        LS_LOG("record: D3D11CreateDevice 失败 0x%08lx", (unsigned long)hr);
        if (dev) dev->Release();
        if (ctx) ctx->Release();
        CloseDxgi(false);
        return false;
    }
    device_ = dev;
    context_ = ctx;
    if (!Duplicate()) {
        CloseDxgi(false);
        return false;
    }
    return true;
}

void ScreenGrabber::CloseDxgi(bool keepDevice) {
    SafeRelease<ID3D11Texture2D>(staging_);
    stagingW_ = stagingH_ = 0;
    if (dupl_) static_cast<IDXGIOutputDuplication *>(dupl_)->Release();
    dupl_ = nullptr;
    if (keepDevice) return;
    SafeRelease<ID3D11DeviceContext>(context_);
    SafeRelease<ID3D11Device>(device_);
    SafeRelease<IDXGIOutput1>(output_);
    SafeRelease<IDXGIFactory1>(factory_);
}

bool ScreenGrabber::Duplicate() {
    if (!output_ || !device_) return false;
    IDXGIOutputDuplication *d = nullptr;
    HRESULT hr = static_cast<IDXGIOutput1 *>(output_)->DuplicateOutput(static_cast<ID3D11Device *>(device_), &d);
    if (FAILED(hr) || !d) {
        LS_LOG("record: DuplicateOutput 失败 0x%08lx", (unsigned long)hr);
        return false;
    }
    DXGI_OUTDUPL_DESC dd;
    d->GetDesc(&dd);
    if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY && dd.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        // 旋转屏的画面是未旋转的方向：交给 GDI
        LS_LOG("record: 显示器已旋转，改用 GDI");
        d->Release();
        return false;
    }
    dupl_ = d;
    if (!staging_ || stagingW_ != w_ || stagingH_ != h_) {
        SafeRelease<ID3D11Texture2D>(staging_);
        D3D11_TEXTURE2D_DESC td;
        std::memset(&td, 0, sizeof td);
        td.Width = (UINT)w_;
        td.Height = (UINT)h_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D *tex = nullptr;
        hr = static_cast<ID3D11Device *>(device_)->CreateTexture2D(&td, nullptr, &tex);
        if (FAILED(hr) || !tex) {
            LS_LOG("record: 创建暂存纹理失败 0x%08lx", (unsigned long)hr);
            d->Release();
            dupl_ = nullptr;
            return false;
        }
        staging_ = tex;
        stagingW_ = w_;
        stagingH_ = h_;
    }
    return true;
}

bool ScreenGrabber::GrabDxgi(bool &changed) {
    changed = false;
    int64_t now = Now100ns();
    if (!dupl_) {
        // 失去访问（UAC 安全桌面、切换用户、全屏独占、分辨率变化）后每 250 ms 重建一次
        if (now - lastRetry_ < 2500000LL) return false;
        lastRetry_ = now;
        if (!device_ || !output_) {
            CloseDxgi(false);
            if (!OpenDxgi()) return false;
        } else if (!Duplicate()) {
            return false;
        }
    }
    IDXGIOutputDuplication *d = static_cast<IDXGIOutputDuplication *>(dupl_);
    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource *res = nullptr;
    HRESULT hr = d->AcquireNextFrame(0, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return true;
    if (FAILED(hr)) {
        LS_LOG("record: AcquireNextFrame 失败 0x%08lx，重建", (unsigned long)hr);
        if (res) res->Release();
        bool deviceGone = hr != DXGI_ERROR_ACCESS_LOST;
        CloseDxgi(!deviceGone);
        lastRetry_ = now;
        return false;
    }
    bool ok = true;
    if (fi.LastPresentTime.QuadPart != 0 || !haveClean_) {
        ID3D11Texture2D *tex = nullptr;
        ok = res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) && tex;
        if (ok) {
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            // 区域相对显示器的位置；分辨率变化后只拷重叠部分
            LONG rx = region_.left - outputRect_.left, ry = region_.top - outputRect_.top;
            LONG x0 = rx < 0 ? 0 : rx, y0 = ry < 0 ? 0 : ry;
            LONG x1 = rx + w_, y1 = ry + h_;
            if (x1 > (LONG)td.Width) x1 = (LONG)td.Width;
            if (y1 > (LONG)td.Height) y1 = (LONG)td.Height;
            ID3D11DeviceContext *ctx = static_cast<ID3D11DeviceContext *>(context_);
            ID3D11Texture2D *st = static_cast<ID3D11Texture2D *>(staging_);
            if (x1 > x0 && y1 > y0) {
                D3D11_BOX box = {(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
                ctx->CopySubresourceRegion(st, 0, (UINT)(x0 - rx), (UINT)(y0 - ry), 0, tex, 0, &box);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
                    size_t row = (size_t)w_ * 4;
                    for (int y = 0; y < h_; ++y)
                        std::memcpy(clean_.data() + (size_t)y * row, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, row);
                    ctx->Unmap(st, 0);
                    haveClean_ = true;
                    changed = true;
                } else {
                    ok = false;
                }
            }
            tex->Release();
        }
    }
    if (res) res->Release();
    d->ReleaseFrame();
    return ok;
}

bool ScreenGrabber::GrabGdi() {
    if (!dc_) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    BOOL ok = BitBlt(dc_, 0, 0, w_, h_, screen, region_.left, region_.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    GdiFlush();
    return ok != 0;
}

void ScreenGrabber::DrawCursor() {
    lastCursor_ = RECT{0, 0, 0, 0};
    if (!cursor_) return;
    CURSORINFO ci;
    std::memset(&ci, 0, sizeof ci);
    ci.cbSize = sizeof ci;
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    if (ci.hCursor != lastCursorHandle_) {
        ICONINFO ii;
        if (!GetIconInfo(ci.hCursor, &ii)) return;
        hotspot_ = POINT{(LONG)ii.xHotspot, (LONG)ii.yHotspot};
        BITMAP bm;
        cursorSize_ = SIZE{32, 32};
        if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof bm, &bm)) {
            cursorSize_ = SIZE{bm.bmWidth, bm.bmHeight};
        } else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof bm, &bm)) {
            cursorSize_ = SIZE{bm.bmWidth, bm.bmHeight / 2};  // 单色光标：上半 AND 掩码、下半 XOR 掩码
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        lastCursorHandle_ = ci.hCursor;
    }
    int x = ci.ptScreenPos.x - hotspot_.x - region_.left;
    int y = ci.ptScreenPos.y - hotspot_.y - region_.top;
    RECT r = {x, y, x + cursorSize_.cx, y + cursorSize_.cy};
    RECT all = {0, 0, w_, h_};
    RECT in;
    if (!IntersectRect(&in, &r, &all)) return;
    DrawIconEx(dc_, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    GdiFlush();
    lastCursor_ = in;
}

bool ScreenGrabber::Grab() {
    if (!dc_) return false;
    if (dxgi_) {
        bool changed = false;
        bool ok = GrabDxgi(changed);
        int64_t now = Now100ns();
        if (ok) {
            failSince_ = 0;
        } else {
            if (!failSince_) failSince_ = now;
            // 连续 10 秒拿不到画面（显卡驱动问题、远程桌面等）：彻底改用 GDI
            if (now - failSince_ > 100000000LL) {
                LS_LOG("record: DXGI 持续失败，改用 GDI");
                CloseDxgi(false);
                dxgi_ = false;
                clean_.clear();
                haveClean_ = false;
                if (!GrabGdi()) return false;
                DrawCursor();
                return true;
            }
        }
        size_t row = (size_t)w_ * 4;
        uint8_t *dst = (uint8_t *)bits_;
        if (changed) {
            std::memcpy(dst, clean_.data(), clean_.size());
        } else if (haveClean_ && lastCursor_.right > lastCursor_.left) {
            // 画面没变：只把上一帧光标盖住的地方还原
            size_t x0 = (size_t)lastCursor_.left * 4, n = (size_t)(lastCursor_.right - lastCursor_.left) * 4;
            for (LONG y = lastCursor_.top; y < lastCursor_.bottom; ++y)
                std::memcpy(dst + (size_t)y * row + x0, clean_.data() + (size_t)y * row + x0, n);
        }
        DrawCursor();
        return true;
    }
    bool ok = GrabGdi();
    DrawCursor();
    return ok;
}

}  // namespace rec
}  // namespace ls
