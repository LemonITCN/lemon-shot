#include "Record/Devices.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include "Record/MF.h"
#include "Settings/Settings.h"
#include "Util/ComPtr.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace rec {

std::vector<Device> Microphones() {
    std::vector<Device> out;
    if (settings::IsSelfTest()) return out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), en.PutVoid())))
        return out;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, col.Put()))) return out;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, dev.Put()))) continue;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id)) || !id) continue;
        Device d;
        d.id = Narrow(std::wstring(id));
        CoTaskMemFree(id);
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.Put()))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                d.name = v.pwszVal;
            PropVariantClear(&v);
        }
        if (d.name.empty()) d.name = Widen(d.id);
        out.push_back(d);
    }
    return out;
}

namespace {

// 列出摄像头激活对象（调用者 Release 每一项并 CoTaskMemFree 数组）
bool EnumCameras(IMFActivate ***list, UINT32 *count) {
    *list = nullptr;
    *count = 0;
    ComPtr<IMFAttributes> attr;
    if (FAILED(mf::CreateAttributes(attr.Put(), 1))) return false;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    return SUCCEEDED(mf::EnumDeviceSources(attr.Get(), list, count)) && *list;
}

std::wstring AllocString(IMFActivate *a, const GUID &key) {
    std::wstring s;
    WCHAR *p = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(a->GetAllocatedString(key, &p, &len)) && p) {
        s.assign(p, len);
        CoTaskMemFree(p);
    }
    return s;
}

}  // namespace

std::vector<Device> Cameras() {
    std::vector<Device> out;
    if (settings::IsSelfTest() || !mf::Available()) return out;
    if (!mf::Startup()) return out;
    IMFActivate **list = nullptr;
    UINT32 count = 0;
    if (EnumCameras(&list, &count)) {
        for (UINT32 i = 0; i < count; ++i) {
            if (!list[i]) continue;
            Device d;
            d.id = Narrow(AllocString(list[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK));
            d.name = AllocString(list[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
            if (d.name.empty()) d.name = Widen(d.id);
            if (!d.id.empty()) out.push_back(d);
            list[i]->Release();
        }
        CoTaskMemFree(list);
    }
    mf::Shutdown();
    return out;
}

bool OpenCamera(const std::string &id, IMFMediaSource **src, IMFActivate **act) {
    *src = nullptr;
    *act = nullptr;
    IMFActivate **list = nullptr;
    UINT32 count = 0;
    if (!EnumCameras(&list, &count)) return false;
    int pick = -1;
    for (UINT32 i = 0; i < count; ++i) {
        if (!list[i]) continue;
        if (pick < 0) pick = (int)i;  // 默认第一个
        if (!id.empty() && Narrow(AllocString(list[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK)) == id) {
            pick = (int)i;
            break;
        }
    }
    bool ok = false;
    if (pick >= 0) {
        IMFMediaSource *s = nullptr;
        HRESULT hr = list[pick]->ActivateObject(__uuidof(IMFMediaSource), (void **)&s);
        if (SUCCEEDED(hr) && s) {
            *src = s;
            *act = list[pick];
            (*act)->AddRef();
            ok = true;
        } else {
            LS_LOG("record: 摄像头激活失败 0x%08lx", (unsigned long)hr);
        }
    }
    for (UINT32 i = 0; i < count; ++i)
        if (list[i]) list[i]->Release();
    CoTaskMemFree(list);
    return ok;
}

}  // namespace rec
}  // namespace ls
