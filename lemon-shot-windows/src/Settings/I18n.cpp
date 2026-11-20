#include "Settings/I18n.h"

#include <windows.h>

#include <map>

#include "../../res/resource.h"
#include "Settings/Settings.h"
#include "Util/Json.h"
#include "Util/Resources.h"
#include "Util/Util.h"

namespace ls {
namespace i18n {

namespace {

std::map<std::string, std::wstring> g_cur;
std::map<std::string, std::wstring> g_en;
std::string g_force;
const char *g_lang = "en";

void LoadInto(int id, std::map<std::string, std::wstring> &out) {
    out.clear();
    Json j;
    if (!Json::Parse(LoadResourceData(id), j) || !j.IsObject()) return;
    for (size_t i = 0; i < j.Keys().size(); ++i) {
        const Json &v = j.ValueAt(i);
        if (v.IsString()) out[j.Keys()[i]] = Widen(v.AsString());
    }
}

}  // namespace

void Init(const char *force) {
    g_force = force ? force : "";
    Reload();
}

void Reload() {
    std::string lang = g_force;
    if (lang.empty()) lang = settings::Str("general.language");
    if (lang.empty() || lang == "system") {
        LANGID id = GetUserDefaultUILanguage();
        lang = PRIMARYLANGID(id) == LANG_CHINESE ? "zh-Hans" : "en";
    }
    if (lang.size() >= 2 && lang[0] == 'z' && lang[1] == 'h') lang = "zh-Hans";
    if (lang != "zh-Hans") lang = "en";
    g_lang = lang == "zh-Hans" ? "zh-Hans" : "en";
    LoadInto(IDR_I18N_EN, g_en);
    if (lang == "zh-Hans")
        LoadInto(IDR_I18N_ZH_HANS, g_cur);
    else
        g_cur = g_en;
}

const char *Lang() { return g_lang; }
bool IsChinese() { return g_lang[0] == 'z'; }

const std::wstring &T(const char *key) {
    auto it = g_cur.find(key);
    if (it != g_cur.end()) return it->second;
    it = g_en.find(key);
    if (it != g_en.end()) return it->second;
    // 缺键：缓存键名本身，保证返回的引用有效
    std::wstring &slot = g_cur[key];
    slot = Widen(key);
    return slot;
}

std::wstring T(const char *key, std::initializer_list<std::pair<const char *, std::wstring>> args) {
    std::wstring s = T(key);
    for (auto &a : args) s = ReplaceAll(s, L"{" + Widen(a.first) + L"}", a.second);
    return s;
}

bool Has(const char *key) { return g_cur.count(key) || g_en.count(key); }

}  // namespace i18n
}  // namespace ls
