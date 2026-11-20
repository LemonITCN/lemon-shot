// 设备枚举（spec 8.1、11）：麦克风（Core Audio IMMDeviceEnumerator，Vista 起都有）、摄像头（Media Foundation）。
// 设置里存设备 id（麦克风为端点 id，摄像头为符号链接）；空串 = 系统默认。自测时返回空列表。
#pragma once

#include <string>
#include <vector>

struct IMFMediaSource;
struct IMFActivate;

namespace ls {
namespace rec {

struct Device {
    std::string id;
    std::wstring name;
};

// 调用线程须已初始化 COM
std::vector<Device> Microphones();
std::vector<Device> Cameras();  // 没有 Media Foundation 时为空

// 打开摄像头：id 为空或找不到时用第一个。调用线程须已 mf::Startup()。
// 成功时 *act 保留激活对象（结束时 ShutdownObject 再 Release），*src 为媒体源。
bool OpenCamera(const std::string &id, IMFMediaSource **src, IMFActivate **act);

}  // namespace rec
}  // namespace ls
