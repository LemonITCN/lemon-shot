// 读取编进 exe 的 RCDATA 资源（res/app.rc）。
#pragma once

#include <string>

namespace ls {

std::string LoadResourceData(int id);

}  // namespace ls
