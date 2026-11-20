// 设置：内置默认值（RCDATA）+ 用户文件深合并；写盘只写完整对象并原子替换。
// 位置：%APPDATA%\LemonShot\settings.json；exe 旁有 portable 文件时用 exe 旁的 data\ 目录。
#pragma once

#include <string>

#include "Util/Json.h"

namespace ls {
namespace settings {

// selftest = true：只用内置默认值，不读也不写任何文件
void Init(bool selftest);
bool IsSelfTest();

const Json &Root();
const Json &Defaults();

bool Bool(const std::string &path);
double Num(const std::string &path);
int Int(const std::string &path);
std::string Str(const std::string &path);
const Json *Get(const std::string &path);

// 修改后立即生效（通知监听者），稍后自动写盘（约 300 ms 合并一次）
void Set(const std::string &path, Json v);
void SetBool(const std::string &path, bool v);
void SetNum(const std::string &path, double v);
void SetStr(const std::string &path, const std::string &v);
// 删除一项（回到默认值）
void Unset(const std::string &path);

void SaveNow();
// 平台定时器：Set 后调用它安排一次延迟写盘；没有设置时立即写
void SetSaveScheduler(void (*schedule)());

// 监听设置变化：path 为被修改的点路径（整体重置时为空串）
typedef void (*Listener)(void *ctx, const std::string &path);
void AddListener(Listener fn, void *ctx);
void RemoveListener(Listener fn, void *ctx);

// 恢复默认：section 为空时全部恢复，否则只恢复该顶层键
void Reset(const std::string &section);
bool ExportTo(const std::wstring &file);
bool ImportFrom(const std::wstring &file);

std::wstring DataDir();  // 设置目录（已确保存在，selftest 下为临时目录）
std::wstring PinsDir();  // DataDir\pins
// 展开 {pictures} {movies} {music}，斜杠统一成反斜杠
std::wstring ExpandPath(const std::string &path);

// 运行状态 state.json（上次区域等）
Json &State();
void SaveState();

}  // namespace settings
}  // namespace ls
