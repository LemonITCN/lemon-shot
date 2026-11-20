#include "App/Hotkeys.h"

#include <cstdlib>
#include <map>

#include "../../res/resource.h"
#include "Settings/Settings.h"
#include "Util/Json.h"
#include "Util/Log.h"
#include "Util/Resources.h"
#include "Util/Util.h"

namespace ls {
namespace hotkeys {

namespace {

std::vector<std::string> g_globalIds;
std::map<std::string, std::string> g_globalDefaults;
std::vector<std::string> g_captureIds;
std::map<std::string, std::string> g_captureDefaults;
std::vector<std::string> g_failed;
std::vector<int> g_registered;

struct KeyNameEntry {
    const char *name;
    UINT vk;
};
const KeyNameEntry kNames[] = {
    {"Space", VK_SPACE},      {"Enter", VK_RETURN},      {"Esc", VK_ESCAPE},       {"Tab", VK_TAB},
    {"Up", VK_UP},            {"Down", VK_DOWN},         {"Left", VK_LEFT},        {"Right", VK_RIGHT},
    {"Delete", VK_DELETE},    {"Backspace", VK_BACK},    {"Home", VK_HOME},        {"End", VK_END},
    {"PageUp", VK_PRIOR},     {"PageDown", VK_NEXT},     {"Insert", VK_INSERT},    {"PrintScreen", VK_SNAPSHOT},
};

UINT VkFromName(const std::string &k) {
    if (k.size() == 1) {
        char c = k[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (UINT)c;
        return 0;
    }
    if ((k[0] == 'F' || k[0] == 'f') && k.size() <= 3) {
        int n = atoi(k.c_str() + 1);
        if (n >= 1 && n <= 24) return (UINT)(VK_F1 + n - 1);
    }
    std::string lk = ToLower(k);
    for (auto &e : kNames)
        if (ToLower(e.name) == lk) return e.vk;
    return 0;
}

std::string NameFromVk(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return std::string(1, (char)('0' + vk - VK_NUMPAD0));
    if (vk >= VK_F1 && vk <= VK_F24) return Format("F%u", vk - VK_F1 + 1);
    for (auto &e : kNames)
        if (e.vk == vk) return e.name;
    return std::string();
}

int BindingFor(const std::string &id) {
    static const char *tools[] = {"toolRect",   "toolEllipse", "toolLine", "toolArrow",     "toolPen",
                                  "toolMosaic", "toolText",    "toolStep", "toolHighlight", "toolMagnify"};
    for (int i = 0; i < 10; ++i)
        if (id == tools[i]) return LS_BIND_TOOL_RECT + i;
    if (id == "longshot") return LS_BIND_LONGSHOT;
    if (id == "ocr") return LS_BIND_OCR;
    if (id == "translate") return LS_BIND_TRANSLATE;
    if (id == "pin") return LS_BIND_PIN;
    if (id == "record") return LS_BIND_RECORD;
    if (id == "lastArea") return LS_BIND_LAST_AREA;
    if (id == "fullscreen") return LS_BIND_FULLSCREEN;
    if (id == "copyRGB") return LS_BIND_COPY_RGB;
    if (id == "copyHEX") return LS_BIND_COPY_HEX;
    if (id == "copyCustom") return LS_BIND_COPY_CUSTOM;
    if (id == "toggleToolbar") return LS_BIND_TOGGLE_TOOLBAR;
    return 0;
}

}  // namespace

void Init() {
    Json j;
    if (!Json::Parse(LoadResourceData(IDR_SHORTCUTS), j)) {
        LS_LOG("hotkeys: shortcuts.json 解析失败");
        return;
    }
    g_globalIds.clear();
    g_captureIds.clear();
    const Json *g = j.Find("global");
    if (g && g->IsArray()) {
        for (size_t i = 0; i < g->Size(); ++i) {
            std::string id = g->At(i).GetString("id", "");
            if (id.empty()) continue;
            g_globalIds.push_back(id);
            g_globalDefaults[id] = g->At(i).GetString("win", "");
        }
    }
    const Json *c = j.Find("capture");
    if (c && c->IsArray()) {
        for (size_t i = 0; i < c->Size(); ++i) {
            std::string id = c->At(i).GetString("id", "");
            if (id.empty()) continue;
            g_captureIds.push_back(id);
            g_captureDefaults[id] = c->At(i).GetString("key", "");
        }
    }
}

const std::vector<std::string> &GlobalIds() { return g_globalIds; }

std::string DefaultGlobal(const std::string &id) {
    auto it = g_globalDefaults.find(id);
    return it == g_globalDefaults.end() ? std::string() : it->second;
}

std::string Global(const std::string &id) {
    const Json *j = settings::Get("shortcuts.win." + id);
    if (j && j->IsString()) return j->AsString();
    return DefaultGlobal(id);
}

// 只把改过的项写进设置；改回默认值时删掉该项
void SetGlobal(const std::string &id, const std::string &combo) {
    if (combo == DefaultGlobal(id))
        settings::Unset("shortcuts.win." + id);
    else
        settings::SetStr("shortcuts.win." + id, combo);
}

void ResetAll() { settings::Reset("shortcuts"); }

bool Parse(const std::string &combo, UINT *mods, UINT *vk) {
    *mods = 0;
    *vk = 0;
    if (combo.empty()) return false;
    std::vector<std::string> parts = Split(combo, '+');
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::string m = ToLower(parts[i]);
        if (m == "ctrl" || m == "control") *mods |= MOD_CONTROL;
        else if (m == "alt" || m == "option") *mods |= MOD_ALT;
        else if (m == "shift") *mods |= MOD_SHIFT;
        else if (m == "win") *mods |= MOD_WIN;
        else return false;  // cmd 等 Windows 没有的修饰键
    }
    *vk = VkFromName(parts.back());
    return *vk != 0;
}

std::string Compose(UINT mods, UINT vk) {
    std::string key = NameFromVk(vk);
    if (key.empty()) return std::string();
    std::string s;
    if (mods & MOD_CONTROL) s += "ctrl+";
    if (mods & MOD_ALT) s += "alt+";
    if (mods & MOD_SHIFT) s += "shift+";
    if (mods & MOD_WIN) s += "win+";
    return s + key;
}

std::wstring KeyName(UINT vk) { return Widen(NameFromVk(vk)); }

std::wstring Display(const std::string &combo) {
    UINT mods, vk;
    if (!Parse(combo, &mods, &vk)) return std::wstring();
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_WIN) s += L"Win+";
    return s + KeyName(vk);
}

void RegisterAll(HWND hwnd) {
    UnregisterAll(hwnd);
    g_failed.clear();
    for (size_t i = 0; i < g_globalIds.size(); ++i) {
        UINT mods, vk;
        if (!Parse(Global(g_globalIds[i]), &mods, &vk)) continue;
        int id = (int)i + 1;
        if (RegisterHotKey(hwnd, id, mods | 0x4000 /* MOD_NOREPEAT */, vk)) {
            g_registered.push_back(id);
        } else {
            LS_LOG("hotkeys: %s 注册失败 %lu", g_globalIds[i].c_str(), GetLastError());
            g_failed.push_back(g_globalIds[i]);
        }
    }
}

void UnregisterAll(HWND hwnd) {
    for (int id : g_registered) UnregisterHotKey(hwnd, id);
    g_registered.clear();
}

bool IsFailed(const std::string &id) {
    for (auto &f : g_failed)
        if (f == id) return true;
    return false;
}

std::vector<std::string> FailedIds() { return g_failed; }

std::string IdForHotkey(int hotkeyId) {
    if (hotkeyId < 1 || hotkeyId > (int)g_globalIds.size()) return std::string();
    return g_globalIds[(size_t)hotkeyId - 1];
}

bool IsDuplicate(const std::string &id) {
    std::string mine = Global(id);
    UINT m1, v1;
    if (!Parse(mine, &m1, &v1)) return false;
    for (auto &other : g_globalIds) {
        if (other == id) continue;
        UINT m2, v2;
        if (Parse(Global(other), &m2, &v2) && m1 == m2 && v1 == v2) return true;
    }
    return false;
}

const std::vector<std::string> &CaptureIds() { return g_captureIds; }

std::string DefaultCaptureKey(const std::string &id) {
    auto it = g_captureDefaults.find(id);
    return it == g_captureDefaults.end() ? std::string() : it->second;
}

std::string CaptureKey(const std::string &id) {
    const Json *j = settings::Get("shortcuts.capture." + id);
    if (j && j->IsString()) return j->AsString();
    return DefaultCaptureKey(id);
}

void SetCaptureKey(const std::string &id, const std::string &key) {
    if (key == DefaultCaptureKey(id))
        settings::Unset("shortcuts.capture." + id);
    else
        settings::SetStr("shortcuts.capture." + id, key);
}

std::wstring CaptureKeyDisplay(const char *id) { return Widen(CaptureKey(id)); }

int KeyCode(const std::string &name) {
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        return 0;
    }
    std::string l = ToLower(name);
    if (l == "tab") return LS_KEY_TAB;
    if (l == "space") return 32;
    if (l == "enter") return LS_KEY_ENTER;
    if (l == "esc") return LS_KEY_ESCAPE;
    if (l == "delete") return LS_KEY_DELETE;
    if (l == "backspace") return LS_KEY_BACKSPACE;
    return 0;
}

void BindSession(ls_session *s) {
    for (auto &id : g_captureIds) {
        int b = BindingFor(id);
        int k = KeyCode(CaptureKey(id));
        if (b) ls_session_bind_key(s, (ls_binding)b, k);
    }
}

}  // namespace hotkeys
}  // namespace ls
