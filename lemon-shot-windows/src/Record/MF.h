// Media Foundation 运行时加载（spec 8.3、10）：mfplat.dll、mfreadwrite.dll、mf.dll 全部 LoadLibrary，
// N 版系统、没装「媒体基础」功能的服务器上没有，这时 Available() 为假，调用方降级（GIF、WAV、禁用摄像头）。
// MFStartup / MFShutdown 按引用计数配对：Startup() 成功后必须调用一次 Shutdown()。
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace ls {
namespace mf {

// 三个 DLL 与所需函数是否齐全（只加载，不调用 MFStartup）
bool Available();
bool Startup();
void Shutdown();

// MP3 编码器（Windows 8+）是否存在
bool HasMp3Encoder();

HRESULT CreateMediaType(IMFMediaType **out);
HRESULT CreateSample(IMFSample **out);
HRESULT CreateMemoryBuffer(DWORD size, IMFMediaBuffer **out);
HRESULT CreateAttributes(IMFAttributes **out, UINT32 initial);
HRESULT CreateSinkWriterFromURL(const wchar_t *url, IMFAttributes *attr, IMFSinkWriter **out);
HRESULT CreateSourceReaderFromMediaSource(IMFMediaSource *src, IMFAttributes *attr, IMFSourceReader **out);
HRESULT EnumDeviceSources(IMFAttributes *attr, IMFActivate ***out, UINT32 *count);
HRESULT TEnumEx(GUID category, UINT32 flags, const MFT_REGISTER_TYPE_INFO *in, const MFT_REGISTER_TYPE_INFO *out,
                IMFActivate ***list, UINT32 *count);

// 从 System32 加载系统 DLL（不在当前目录找，避免 DLL 劫持）
HMODULE LoadSystemDll(const wchar_t *name);

}  // namespace mf
}  // namespace ls
