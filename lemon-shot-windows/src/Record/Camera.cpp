#include "Record/Camera.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Record/Devices.h"
#include "Record/MF.h"
#include "Record/RecordUtil.h"
#include "Util/Api.h"
#include "Util/ComPtr.h"
#include "Util/Log.h"

namespace ls {
namespace rec {

namespace {

enum : UINT { WM_REC_CAMFRAME = WM_APP + 0x320, WM_REC_CAMFAIL };
const DWORD kVideoStream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;

}  // namespace

// 界面线程与取帧线程共享；引用计数 2，两边各释放一次
struct CameraWindow::Shared {
    volatile LONG refs = 2;
    volatile LONG stop = 0;
    Lock lock;
    HWND hwnd = nullptr;
    std::vector<uint8_t> frame;
    int w = 0, h = 0;
    bool posted = false;
    std::string id;
    void Release() {
        if (InterlockedDecrement(&refs) == 0) delete this;
    }
};

namespace {

struct Candidate {
    int index;
    int score;
};

bool SetupReader(IMFSourceReader *reader) {
    std::vector<Candidate> cands;
    for (DWORD i = 0; i < 400; ++i) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = reader->GetNativeMediaType(kVideoStream, i, t.Put());
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
        GUID sub = GUID_NULL;
        UINT64 size = 0;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        t->GetUINT64(MF_MT_FRAME_SIZE, &size);
        int w = (int)(size >> 32);
        bool known = sub == MFVideoFormat_NV12 || sub == MFVideoFormat_YUY2 || sub == MFVideoFormat_RGB32 ||
                     sub == MFVideoFormat_RGB24 || sub == MFVideoFormat_I420 || sub == MFVideoFormat_IYUV ||
                     sub == MFVideoFormat_MJPG;
        if (!known || w <= 0) continue;
        // 画中画最大也就几百像素：优先宽 640–960 的非压缩格式
        int score = w > 1280 ? -10000 - w : -std::abs(w - 800);
        if (sub != MFVideoFormat_MJPG) score += 50;
        cands.push_back({(int)i, score});
    }
    std::sort(cands.begin(), cands.end(), [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    for (size_t k = 0; k < cands.size() && k < 6; ++k) {
        ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(kVideoStream, (DWORD)cands[k].index, native.Put()))) continue;
        if (FAILED(reader->SetCurrentMediaType(kVideoStream, nullptr, native.Get()))) continue;
        ComPtr<IMFMediaType> out;
        if (FAILED(mf::CreateMediaType(out.Put()))) return false;
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(reader->SetCurrentMediaType(kVideoStream, nullptr, out.Get()))) return true;
    }
    return false;
}

void ReadFormat(IMFSourceReader *reader, int &w, int &h, LONG &stride) {
    ComPtr<IMFMediaType> cur;
    w = h = 0;
    stride = 0;
    if (FAILED(reader->GetCurrentMediaType(kVideoStream, cur.Put()))) return;
    UINT64 size = 0;
    cur->GetUINT64(MF_MT_FRAME_SIZE, &size);
    w = (int)(size >> 32);
    h = (int)(size & 0xFFFFFFFF);
    UINT32 s = 0;
    if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)))
        stride = (LONG)(INT32)s;
    else
        stride = w * 4;
}

DWORD WINAPI CameraProc(void *p) {
    CameraWindow::Shared *sh = (CameraWindow::Shared *)p;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool mfok = mf::Startup();
    IMFMediaSource *src = nullptr;
    IMFActivate *act = nullptr;
    IMFSourceReader *reader = nullptr;
    bool ok = mfok && OpenCamera(sh->id, &src, &act);
    if (ok) {
        ComPtr<IMFAttributes> attr;
        ok = SUCCEEDED(mf::CreateAttributes(attr.Put(), 2));
        if (ok) {
            attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            ok = SUCCEEDED(mf::CreateSourceReaderFromMediaSource(src, attr.Get(), &reader)) && reader;
        }
    }
    if (ok) ok = SetupReader(reader);
    int w = 0, h = 0;
    LONG stride = 0;
    if (ok) ReadFormat(reader, w, h, stride);
    if (ok && (w <= 0 || h <= 0)) ok = false;
    bool gotFrame = false;
    if (!ok) LS_LOG("record: 摄像头打开失败");
    while (ok && !sh->stop) {
        DWORD idx = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *s = nullptr;
        HRESULT hr = reader->ReadSample(kVideoStream, 0, &idx, &flags, &ts, &s);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            LS_LOG("record: 摄像头取帧结束 0x%08lx", (unsigned long)hr);
            if (s) s->Release();
            if (!gotFrame) ok = false;
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) ReadFormat(reader, w, h, stride);
        if (!s) continue;
        IMFMediaBuffer *buf = nullptr;
        if (SUCCEEDED(s->GetBufferByIndex(0, &buf)) && buf && w > 0 && h > 0) {
            BYTE *scan0 = nullptr;
            LONG pitch = 0;
            IMF2DBuffer *b2 = nullptr;
            BYTE *base = nullptr;
            bool locked2d = false;
            if (SUCCEEDED(buf->QueryInterface(__uuidof(IMF2DBuffer), (void **)&b2)) && b2 &&
                SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
                locked2d = true;
            } else {
                DWORD max = 0, len = 0;
                if (SUCCEEDED(buf->Lock(&base, &max, &len)) && base) {
                    pitch = stride ? stride : w * 4;
                    scan0 = pitch < 0 ? base + (size_t)(-pitch) * (h - 1) : base;
                    if ((size_t)(pitch < 0 ? -pitch : pitch) * h > len) scan0 = nullptr;
                }
            }
            if (scan0) {
                Guard g(sh->lock);
                sh->frame.resize((size_t)w * h * 4);
                for (int y = 0; y < h; ++y) {
                    const BYTE *row = scan0 + (ptrdiff_t)pitch * y;
                    uint8_t *d = sh->frame.data() + (size_t)y * w * 4;
                    std::memcpy(d, row, (size_t)w * 4);
                    for (int x = 0; x < w; ++x) d[x * 4 + 3] = 255;
                }
                sh->w = w;
                sh->h = h;
                if (!sh->posted && sh->hwnd) {
                    sh->posted = true;
                    PostMessageW(sh->hwnd, WM_REC_CAMFRAME, 0, 0);
                }
                gotFrame = true;
            }
            if (locked2d) b2->Unlock2D();
            else if (base) buf->Unlock();
            if (b2) b2->Release();
        }
        if (buf) buf->Release();
        s->Release();
    }
    if (!ok) {
        Guard g(sh->lock);
        if (sh->hwnd) PostMessageW(sh->hwnd, WM_REC_CAMFAIL, 0, 0);
    }
    if (reader) reader->Release();
    if (src) {
        src->Shutdown();
        src->Release();
    }
    if (act) {
        act->ShutdownObject();
        act->Release();
    }
    if (mfok) mf::Shutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    sh->Release();
    return 0;
}

}  // namespace

CameraWindow::~CameraWindow() { Close(); }

bool CameraWindow::Open(const std::string &deviceId, RECT region, UINT dpi, bool rounded) {
    Close();
    dpi_ = dpi;
    rounded_ = rounded;
    float s = dpi / 96.f;
    float rw = (region.right - region.left) / s, rh = (region.bottom - region.top) / s;
    float d = std::round((rw < rh ? rw : rh) * 0.22f);
    if (d < 64) d = 64;
    float margin = std::round(d * 0.12f);
    if (margin < 8) margin = 8;
    size_ = (int)std::lround(d * s);
    int m = (int)std::lround(margin * s);
    pos_ = POINT{region.right - m - size_, region.bottom - m - size_};
    // 进画面：不排除；不抢焦点；可以拖动
    if (!CreateWin(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP, pos_.x, pos_.y, size_, size_,
                   nullptr, L"Lemon Shot"))
        return false;
    shared_ = new Shared();
    shared_->id = deviceId;
    shared_->hwnd = hwnd_;
    thread_ = CreateThread(nullptr, 0, &CameraProc, shared_, 0, nullptr);
    if (!thread_) {
        shared_->Release();
        shared_->Release();
        shared_ = nullptr;
        DestroyWin();
        return false;
    }
    return true;
}

void CameraWindow::Close() {
    if (shared_) {
        {
            Guard g(shared_->lock);
            shared_->hwnd = nullptr;
        }
        InterlockedExchange(&shared_->stop, 1);
        shared_->Release();
        shared_ = nullptr;
    }
    if (thread_) {
        // 正常一帧之内就会退出；摄像头卡住时不无限等（线程退出时自己释放）
        WaitForSingleObject(thread_, 1500);
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    shown_ = false;
    dragging_ = false;
    DestroyWin();
}

void CameraWindow::Render() {
    if (!hwnd_ || fw_ <= 0 || fh_ <= 0) return;
    float s = dpi_ / 96.f;
    float d = size_ / s;
    ID2D1RenderTarget *rt = surface_.Begin(size_, size_, dpi_);
    if (!rt) return;
    {
        Canvas c(rt);
        float radius = rounded_ ? d * 0.18f : d / 2;
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        D2D1_ROUNDED_RECT rr = {RectF(0, 0, d, d).D2(), radius, radius};
        gfx::D2D()->CreateRoundedRectangleGeometry(&rr, clip.Put());
        c.PushGeometryMask(clip.Get());
        c.FillRect(RectF(0, 0, d, d), Color{0, 0, 0, 1});
        ComPtr<ID2D1Bitmap> bmp = gfx::CreateBitmap(rt, frame_.data(), fw_, fh_, fw_ * 4);
        if (bmp) {
            // 等比铺满（居中裁切），水平镜像（自拍视角）
            float k = std::fmax(d / fw_, d / fh_);
            float w = fw_ * k, h = fh_ * k;
            D2D1_MATRIX_3X2_F old = c.GetTransform();
            c.SetTransform(MatMul(MatScale(-1, 1, d / 2, 0), old));
            c.Bitmap(bmp.Get(), RectF((d - w) / 2, (d - h) / 2, w, h), nullptr);
            c.SetTransform(old);
        }
        c.PopLayer();
        c.StrokeRound(RectF(1, 1, d - 2, d - 2), std::fmax(0.f, radius - 1), Color{1, 1, 1, 0.9f}, 2);
    }
    surface_.End(hwnd_, pos_.x, pos_.y, 255);
    if (!shown_) {
        shown_ = true;
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

LRESULT CameraWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_REC_CAMFRAME:
        if (shared_) {
            Guard g(shared_->lock);
            shared_->posted = false;
            frame_ = shared_->frame;
            fw_ = shared_->w;
            fh_ = shared_->h;
        }
        Render();
        return 0;
    case WM_REC_CAMFAIL: {
        std::function<void()> cb = onFailed;
        if (cb) cb();  // 调用方会关掉本窗口
        return 0;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32646) /* IDC_SIZEALL */));
        return TRUE;
    case WM_LBUTTONDOWN: {
        POINT pt;
        GetCursorPos(&pt);
        dragOffset_ = POINT{pt.x - pos_.x, pt.y - pos_.y};
        dragging_ = true;
        SetCapture(hwnd_);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (dragging_) {
            POINT pt;
            GetCursorPos(&pt);
            pos_ = POINT{pt.x - dragOffset_.x, pt.y - dragOffset_.y};
            SetWindowPos(hwnd_, nullptr, pos_.x, pos_.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            if (GetCapture() == hwnd_) ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED: dragging_ = false; return 0;
    }
    return Window::Handle(msg, wp, lp);
}

}  // namespace rec
}  // namespace ls
