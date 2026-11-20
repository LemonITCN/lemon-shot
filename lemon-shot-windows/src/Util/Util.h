// 通用工具：UTF-8/UTF-16 转换、格式化、文件读写、路径、计时。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ls {

// ---------------------------------------------------------------- 字符串
std::wstring Widen(const char *s, size_t n);
std::wstring Widen(const std::string &s);
std::string Narrow(const wchar_t *w, size_t n);
std::string Narrow(const std::wstring &w);

std::string Format(const char *fmt, ...);
std::wstring FormatW(const wchar_t *fmt, ...);
std::wstring ReplaceAll(std::wstring s, const std::wstring &from, const std::wstring &to);
std::string ReplaceAll(std::string s, const std::string &from, const std::string &to);
std::vector<std::string> Split(const std::string &s, char sep);
std::string ToLower(std::string s);
std::wstring Trim(const std::wstring &s);
bool StartsWith(const std::string &s, const char *prefix);
// 数字转字符串：整数不带小数点，其他最多 digits 位小数并去掉末尾 0
std::wstring NumberText(double v, int digits = 2);

// ---------------------------------------------------------------- 文件与路径
bool ReadFileBytes(const std::wstring &path, std::string &out);
// 原子写：先写同目录临时文件，再 MoveFileEx 替换
bool WriteFileAtomic(const std::wstring &path, const void *data, size_t size);
bool FileExists(const std::wstring &path);
bool DirExists(const std::wstring &path);
bool EnsureDir(const std::wstring &path);  // 递归创建
std::wstring ExePath();
std::wstring ExeDir();
std::wstring PathJoin(const std::wstring &a, const std::wstring &b);
std::wstring PathDir(const std::wstring &path);
std::wstring PathName(const std::wstring &path);  // 最后一级
std::wstring KnownFolder(const GUID &id);          // SHGetKnownFolderPath
std::wstring TempDir();
// 文件名里不允许的字符替换成 _
std::wstring SanitizeFileName(const std::wstring &name);
// 同名文件存在时加 " (2)"、" (3)"…
std::wstring UniquePath(const std::wstring &dir, const std::wstring &stem, const std::wstring &ext);

// ---------------------------------------------------------------- 其他
uint64_t Fnv1a64(const void *data, size_t size, uint64_t seed = 1469598103934665603ULL);
double NowSeconds();  // 单调时钟，秒
void ShellOpen(const std::wstring &target);           // 用系统默认方式打开文件/网址
void RevealInFolder(const std::wstring &file);        // 在资源管理器中显示并选中
bool CopyTextToClipboard(HWND owner, const std::wstring &text);

template <class T>
inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace ls
