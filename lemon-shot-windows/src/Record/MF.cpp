#include "Record/MF.h"

#include "Record/RecordUtil.h"
#include "Util/Api.h"
#include "Util/Log.h"

namespace ls {
namespace mf {

namespace {

typedef HRESULT(WINAPI *MFStartupFn)(ULONG, DWORD);
typedef HRESULT(WINAPI *MFShutdownFn)(void);
typedef HRESULT(WINAPI *MFCreateMediaTypeFn)(IMFMediaType **);
typedef HRESULT(WINAPI *MFCreateSampleFn)(IMFSample **);
typedef HRESULT(WINAPI *MFCreateMemoryBufferFn)(DWORD, IMFMediaBuffer **);
typedef HRESULT(WINAPI *MFCreateAttributesFn)(IMFAttributes **, UINT32);
typedef HRESULT(WINAPI *MFTEnumExFn)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *,
                                     IMFActivate ***, UINT32 *);
typedef HRESULT(WINAPI *MFCreateSinkWriterFromURLFn)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **);
typedef HRESULT(WINAPI *MFCreateSourceReaderFromMediaSourceFn)(IMFMediaSource *, IMFAttributes *, IMFSourceReader **);
typedef HRESULT(WINAPI *MFEnumDeviceSourcesFn)(IMFAttributes *, IMFActivate ***, UINT32 *);

struct Fns {
    MFStartupFn startup = nullptr;
    MFShutdownFn shutdown = nullptr;
    MFCreateMediaTypeFn mediaType = nullptr;
    MFCreateSampleFn sample = nullptr;
    MFCreateMemoryBufferFn memBuffer = nullptr;
    MFCreateAttributesFn attributes = nullptr;
    MFTEnumExFn tenumEx = nullptr;
    MFCreateSinkWriterFromURLFn sinkWriter = nullptr;
    MFCreateSourceReaderFromMediaSourceFn sourceReader = nullptr;
    MFEnumDeviceSourcesFn enumDevices = nullptr;
};

Fns g_fn;
rec::Lock g_lock;
int g_loadState = 0;  // 0 未尝试，1 可用，-1 不可用
int g_refs = 0;

template <class T>
void Load(HMODULE m, const char *name, T &out) {
    out = m ? reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(m, name))) : nullptr;
}

bool LoadLocked() {
    if (g_loadState) return g_loadState > 0;
    HMODULE plat = LoadSystemDll(L"mfplat.dll");
    HMODULE rw = LoadSystemDll(L"mfreadwrite.dll");
    HMODULE mfdll = LoadSystemDll(L"mf.dll");
    Load(plat, "MFStartup", g_fn.startup);
    Load(plat, "MFShutdown", g_fn.shutdown);
    Load(plat, "MFCreateMediaType", g_fn.mediaType);
    Load(plat, "MFCreateSample", g_fn.sample);
    Load(plat, "MFCreateMemoryBuffer", g_fn.memBuffer);
    Load(plat, "MFCreateAttributes", g_fn.attributes);
    Load(plat, "MFTEnumEx", g_fn.tenumEx);
    Load(rw, "MFCreateSinkWriterFromURL", g_fn.sinkWriter);
    Load(rw, "MFCreateSourceReaderFromMediaSource", g_fn.sourceReader);
    Load(mfdll, "MFEnumDeviceSources", g_fn.enumDevices);
    bool ok = g_fn.startup && g_fn.shutdown && g_fn.mediaType && g_fn.sample && g_fn.memBuffer && g_fn.attributes &&
              g_fn.sinkWriter && g_fn.sourceReader;
    g_loadState = ok ? 1 : -1;
    LS_LOG("mf: Media Foundation %s（mfplat %p，mfreadwrite %p，mf %p）", ok ? "可用" : "不可用", (void *)plat, (void *)rw,
           (void *)mfdll);
    // 加载失败的 DLL 不卸载：进程内只尝试一次，保留句柄无害
    return ok;
}

}  // namespace

HMODULE LoadSystemDll(const wchar_t *name) {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 32) return nullptr;
    std::wstring path(dir, n);
    path += L"\\";
    path += name;
    return LoadLibraryW(path.c_str());
}

bool Available() {
    rec::Guard g(g_lock);
    return LoadLocked();
}

bool Startup() {
    rec::Guard g(g_lock);
    if (!LoadLocked()) return false;
    // Win7 SDK 的版本号（0x00020070）在更新的系统上同样可用
    HRESULT hr = g_fn.startup((2UL << 16) | 0x0070UL, 0 /* MFSTARTUP_FULL */);
    if (FAILED(hr)) {
        LS_LOG("mf: MFStartup 失败 0x%08lx", (unsigned long)hr);
        return false;
    }
    ++g_refs;
    return true;
}

void Shutdown() {
    rec::Guard g(g_lock);
    if (g_refs <= 0 || !g_fn.shutdown) return;
    --g_refs;
    g_fn.shutdown();
}

bool HasMp3Encoder() {
    if (!api::IsWin8OrLater()) return false;
    if (!Startup()) return false;
    bool found = false;
    if (g_fn.tenumEx) {
        MFT_REGISTER_TYPE_INFO out = {MFMediaType_Audio, MFAudioFormat_MP3};
        IMFActivate **list = nullptr;
        UINT32 count = 0;
        HRESULT hr = g_fn.tenumEx(MFT_CATEGORY_AUDIO_ENCODER,
                                  MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, nullptr,
                                  &out, &list, &count);
        if (SUCCEEDED(hr) && list) {
            found = count > 0;
            for (UINT32 i = 0; i < count; ++i)
                if (list[i]) list[i]->Release();
            CoTaskMemFree(list);
        }
    }
    Shutdown();
    return found;
}

HRESULT CreateMediaType(IMFMediaType **out) { return g_fn.mediaType ? g_fn.mediaType(out) : E_NOTIMPL; }
HRESULT CreateSample(IMFSample **out) { return g_fn.sample ? g_fn.sample(out) : E_NOTIMPL; }
HRESULT CreateMemoryBuffer(DWORD size, IMFMediaBuffer **out) {
    return g_fn.memBuffer ? g_fn.memBuffer(size, out) : E_NOTIMPL;
}
HRESULT CreateAttributes(IMFAttributes **out, UINT32 initial) {
    return g_fn.attributes ? g_fn.attributes(out, initial) : E_NOTIMPL;
}
HRESULT CreateSinkWriterFromURL(const wchar_t *url, IMFAttributes *attr, IMFSinkWriter **out) {
    return g_fn.sinkWriter ? g_fn.sinkWriter(url, nullptr, attr, out) : E_NOTIMPL;
}
HRESULT CreateSourceReaderFromMediaSource(IMFMediaSource *src, IMFAttributes *attr, IMFSourceReader **out) {
    return g_fn.sourceReader ? g_fn.sourceReader(src, attr, out) : E_NOTIMPL;
}
HRESULT EnumDeviceSources(IMFAttributes *attr, IMFActivate ***out, UINT32 *count) {
    return g_fn.enumDevices ? g_fn.enumDevices(attr, out, count) : E_NOTIMPL;
}
HRESULT TEnumEx(GUID category, UINT32 flags, const MFT_REGISTER_TYPE_INFO *in, const MFT_REGISTER_TYPE_INFO *out,
                IMFActivate ***list, UINT32 *count) {
    return g_fn.tenumEx ? g_fn.tenumEx(category, flags, in, out, list, count) : E_NOTIMPL;
}

}  // namespace mf
}  // namespace ls
