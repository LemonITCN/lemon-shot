#include "Pin/VirtualDesktop.h"

#include <objbase.h>
#include <servprov.h>

#include "Util/Api.h"
#include "Util/ComPtr.h"

namespace ls {
namespace vdesk {

namespace {

// 未公开接口：只用到 vtable 前几项，这几项从 Win10 1607 到 Win11 一直没变
const CLSID kCLSID_ImmersiveShell = {0xC2F03A33, 0x21F5, 0x47FA, {0xB4, 0xBB, 0x15, 0x63, 0x62, 0xA2, 0xF2, 0x39}};
const GUID kSID_PinnedApps = {0xB5A399E7, 0x1C87, 0x46B8, {0x88, 0xE9, 0xFC, 0x57, 0x47, 0xB1, 0x71, 0xBD}};
const IID kIID_PinnedApps = {0x4CE81583, 0x1E4C, 0x4632, {0xA6, 0x21, 0x07, 0xA5, 0x35, 0x43, 0x14, 0x8F}};
const IID kIID_ViewCollection = {0x1841C6D7, 0x4F9D, 0x42C0, {0xAF, 0x41, 0x87, 0x47, 0x53, 0x8F, 0x10, 0xE5}};

struct IApplicationViewCollection : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetViews(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByZOrder(void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewsByAppUserModelId(LPCWSTR, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewForHwnd(HWND, IUnknown **) = 0;
};

struct IVirtualDesktopPinnedApps : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsAppIdPinned(LPCWSTR, BOOL *) = 0;
    virtual HRESULT STDMETHODCALLTYPE PinAppID(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnpinAppID(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsViewPinned(IUnknown *, BOOL *) = 0;
    virtual HRESULT STDMETHODCALLTYPE PinView(IUnknown *) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnpinView(IUnknown *) = 0;
};

bool Get(HWND hwnd, IVirtualDesktopPinnedApps **apps, IUnknown **view) {
    *apps = nullptr;
    *view = nullptr;
    if (!api::IsWin10OrLater() || !hwnd) return false;
    IServiceProvider *sp = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_ImmersiveShell, nullptr, CLSCTX_LOCAL_SERVER, IID_IServiceProvider, (void **)&sp)) || !sp)
        return false;
    IApplicationViewCollection *col = nullptr;
    bool ok = SUCCEEDED(sp->QueryService(kSID_PinnedApps, kIID_PinnedApps, (void **)apps)) && *apps &&
              SUCCEEDED(sp->QueryService(kIID_ViewCollection, kIID_ViewCollection, (void **)&col)) && col &&
              SUCCEEDED(col->GetViewForHwnd(hwnd, view)) && *view;
    if (col) col->Release();
    sp->Release();
    if (!ok) {
        if (*apps) (*apps)->Release();
        if (*view) (*view)->Release();
        *apps = nullptr;
        *view = nullptr;
    }
    return ok;
}

}  // namespace

bool Available(HWND hwnd) {
    IVirtualDesktopPinnedApps *apps;
    IUnknown *view;
    if (!Get(hwnd, &apps, &view)) return false;
    view->Release();
    apps->Release();
    return true;
}

bool Pin(HWND hwnd, bool pin) {
    IVirtualDesktopPinnedApps *apps;
    IUnknown *view;
    if (!Get(hwnd, &apps, &view)) return false;
    HRESULT hr = pin ? apps->PinView(view) : apps->UnpinView(view);
    view->Release();
    apps->Release();
    return SUCCEEDED(hr);
}

}  // namespace vdesk
}  // namespace ls
