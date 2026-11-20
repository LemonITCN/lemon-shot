#include "Util/Util.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace ls {

std::wstring Widen(const char *s, size_t n) {
    if (!s || n == 0) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, &w[0], len);
    return w;
}

std::wstring Widen(const std::string &s) { return Widen(s.data(), s.size()); }

std::string Narrow(const wchar_t *w, size_t n) {
    if (!w || n == 0) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, (int)n, &s[0], len, nullptr, nullptr);
    return s;
}

std::string Narrow(const std::wstring &w) { return Narrow(w.data(), w.size()); }

std::string Format(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return std::string();
    if ((size_t)n < sizeof buf) return std::string(buf, (size_t)n);
    std::string out((size_t)n + 1, '\0');
    va_start(ap, fmt);
    vsnprintf(&out[0], out.size(), fmt, ap);
    va_end(ap);
    out.resize((size_t)n);
    return out;
}

std::wstring FormatW(const wchar_t *fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    if (n < 0) {
        buf[511] = 0;
        return std::wstring(buf);
    }
    return std::wstring(buf, (size_t)n);
}

std::wstring ReplaceAll(std::wstring s, const std::wstring &from, const std::wstring &to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string ReplaceAll(std::string s, const std::string &from, const std::string &to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::vector<std::string> Split(const std::string &s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

std::string ToLower(std::string s) {
    for (auto &c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::wstring Trim(const std::wstring &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

bool StartsWith(const std::string &s, const char *prefix) {
    size_t n = std::strlen(prefix);
    return s.size() >= n && std::memcmp(s.data(), prefix, n) == 0;
}

std::wstring NumberText(double v, int digits) {
    if (v > -2e9 && v < 2e9 && v == (double)(int)v) return FormatW(L"%d", (int)v);
    wchar_t buf[64];
    _snwprintf(buf, 63, L"%.*f", digits, v);
    buf[63] = 0;
    std::wstring s(buf);
    if (s.find(L'.') != std::wstring::npos) {
        while (!s.empty() && s.back() == L'0') s.pop_back();
        if (!s.empty() && s.back() == L'.') s.pop_back();
    }
    return s;
}

// ---------------------------------------------------------------- 文件

bool ReadFileBytes(const std::wstring &path, std::string &out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 512LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)size.QuadPart);
    DWORD total = 0;
    bool ok = true;
    while (total < (DWORD)size.QuadPart) {
        DWORD got = 0;
        if (!ReadFile(h, &out[total], (DWORD)size.QuadPart - total, &got, nullptr) || got == 0) {
            ok = false;
            break;
        }
        total += got;
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool WriteFileAtomic(const std::wstring &path, const void *data, size_t size) {
    std::wstring tmp = path + FormatW(L".%lu.tmp", GetCurrentProcessId());
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const char *p = (const char *)data;
    size_t left = size;
    bool ok = true;
    while (left > 0) {
        DWORD chunk = left > 0x10000000 ? 0x10000000 : (DWORD)left;
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        p += wrote;
        left -= wrote;
    }
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool FileExists(const std::wstring &path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring &path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring &path) {
    if (path.empty()) return false;
    if (DirExists(path)) return true;
    int r = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS || DirExists(path);
}

std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
        if (n == 0) return std::wstring();
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring ExeDir() { return PathDir(ExePath()); }

std::wstring PathJoin(const std::wstring &a, const std::wstring &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring PathDir(const std::wstring &path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return std::wstring();
    return path.substr(0, p);
}

std::wstring PathName(const std::wstring &path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return path;
    return path.substr(p + 1);
}

std::wstring KnownFolder(const GUID &id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

std::wstring TempDir() {
    wchar_t buf[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    if (n == 0 || n > MAX_PATH) return L".";
    std::wstring s(buf, n);
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

std::wstring SanitizeFileName(const std::wstring &name) {
    std::wstring s = name;
    for (auto &c : s) {
        if (c < 32 || c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' ||
            c == L'>' || c == L'|')
            c = L'_';
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    if (s.empty()) s = L"Lemon Shot";
    return s;
}

std::wstring UniquePath(const std::wstring &dir, const std::wstring &stem, const std::wstring &ext) {
    std::wstring p = PathJoin(dir, stem + L"." + ext);
    for (int i = 2; FileExists(p) && i < 10000; ++i) p = PathJoin(dir, stem + FormatW(L" (%d).", i) + ext);
    return p;
}

// ---------------------------------------------------------------- 其他

uint64_t Fnv1a64(const void *data, size_t size, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

double NowSeconds() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

void ShellOpen(const std::wstring &target) {
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RevealInFolder(const std::wstring &file) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str());
    if (pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr)) return;
    }
    std::wstring dir = DirExists(file) ? file : PathDir(file);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool CopyTextToClipboard(HWND owner, const std::wstring &text) {
    // 剪贴板可能被别的程序短暂占用，重试几次
    bool opened = false;
    for (int i = 0; i < 10 && !opened; ++i) {
        opened = OpenClipboard(owner) != 0;
        if (!opened) Sleep(15);
    }
    if (!opened) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h) {
        void *p = GlobalLock(h);
        if (p) {
            std::memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) GlobalFree(h);
    }
    CloseClipboard();
    return ok;
}

}  // namespace ls
