// 在这一个编译单元里实例化 WIC / 已知文件夹等头文件里 DEFINE_GUID 声明的 GUID（selectany，未用到的会被链接器丢掉）。
#include <initguid.h>

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <wincodec.h>
