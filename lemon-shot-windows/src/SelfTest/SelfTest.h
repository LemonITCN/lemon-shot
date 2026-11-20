// 自测截图（spec 1.1）：用 WIC 位图渲染目标离屏画全部界面状态，存 PNG 后退出，不创建任何窗口。
#pragma once

#include <string>

namespace ls {
namespace selftest {

int Run(const std::wstring &dir, float scale);

}  // namespace selftest
}  // namespace ls
