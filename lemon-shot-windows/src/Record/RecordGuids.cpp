// 实例化录屏/录音用到的 GUID（Media Foundation、Core Audio、属性键）。不链接 mfuuid 等静态库，
// 头文件里 DEFINE_GUID 声明的常量在这一个编译单元里展开为 selectany 定义，未用到的会被链接器丢掉。
#include <initguid.h>

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
