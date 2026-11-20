// 界面文案：i18n/zh-Hans.json 与 en.json（RCDATA）。general.language 为 system 时按系统界面语言。
#pragma once

#include <initializer_list>
#include <string>
#include <utility>

namespace ls {
namespace i18n {

// force：自测时指定 "zh-Hans" / "en"，空则按设置
void Init(const char *force);
void Reload();  // 语言设置变化后调用
const char *Lang();  // "zh-Hans" 或 "en"
bool IsChinese();

// 取文案；缺失时退回英文，再退回键名
const std::wstring &T(const char *key);
// 占位符 {name} 替换
std::wstring T(const char *key, std::initializer_list<std::pair<const char *, std::wstring>> args);
bool Has(const char *key);

}  // namespace i18n

inline const std::wstring &T(const char *key) { return i18n::T(key); }

}  // namespace ls
