// 快捷键：全局快捷键（RegisterHotKey，默认值取 shortcuts.json 的 win 字段）与截图时单键。
// 用户修改存在设置 shortcuts.win.<id> 与 shortcuts.capture.<id>（settings.defaults.json 没有这一节，按未知键保留）。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "lemonshot.h"

namespace ls {
namespace hotkeys {

void Init();

// ---------------------------------------------------------------- 全局
const std::vector<std::string> &GlobalIds();  // 18 项，顺序同 shortcuts.json
std::string DefaultGlobal(const std::string &id);
std::string Global(const std::string &id);
void SetGlobal(const std::string &id, const std::string &combo);  // 写设置（空串 = 不用）
void ResetAll();

bool Parse(const std::string &combo, UINT *mods, UINT *vk);  // mods 为 MOD_*
std::string Compose(UINT mods, UINT vk);                       // 反向：录入快捷键用；不支持的键返回空
std::wstring Display(const std::string &combo);                 // "Ctrl+Alt+W"
std::wstring KeyName(UINT vk);

void RegisterAll(HWND hwnd);
void UnregisterAll(HWND hwnd);
bool IsFailed(const std::string &id);
std::vector<std::string> FailedIds();
// WM_HOTKEY 的编号 → id；没有返回空串
std::string IdForHotkey(int hotkeyId);
// 与其他全局快捷键重复
bool IsDuplicate(const std::string &id);

// ---------------------------------------------------------------- 截图时单键
const std::vector<std::string> &CaptureIds();  // 21 项
std::string DefaultCaptureKey(const std::string &id);
std::string CaptureKey(const std::string &id);
void SetCaptureKey(const std::string &id, const std::string &key);
std::wstring CaptureKeyDisplay(const char *id);
int KeyCode(const std::string &name);  // → ls 键值（'A'、'0'、LS_KEY_TAB…），无效返回 0
void BindSession(ls_session *s);

}  // namespace hotkeys
}  // namespace ls
