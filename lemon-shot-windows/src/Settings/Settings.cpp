#include "Settings/Settings.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <vector>

#include "../../res/resource.h"
#include "Util/Log.h"
#include "Util/Resources.h"
#include "Util/Util.h"

namespace ls {
namespace settings {

namespace {

Json g_defaults;
Json g_root;
Json g_state;
bool g_selftest = false;
std::wstring g_dir;
void (*g_scheduler)() = nullptr;

struct ListenerEntry {
    Listener fn;
    void *ctx;
};
std::vector<ListenerEntry> g_listeners;

std::wstring SettingsFile() { return PathJoin(g_dir, L"settings.json"); }
std::wstring StateFile() { return PathJoin(g_dir, L"state.json"); }

void Notify(const std::string &path) {
    // 回调里可能增删监听者，先复制一份
    std::vector<ListenerEntry> copy = g_listeners;
    for (auto &l : copy) l.fn(l.ctx, path);
}

const Json g_null;

}  // namespace

void Init(bool selftest) {
    g_selftest = selftest;
    std::string def = LoadResourceData(IDR_SETTINGS_DEFAULTS);
    if (!Json::Parse(def, g_defaults) || !g_defaults.IsObject()) {
        LS_LOG("settings: 内置默认值解析失败");
        g_defaults = Json::MakeObject();
    }
    g_root = g_defaults;
    g_state = Json::MakeObject();
    if (selftest) {
        g_dir = PathJoin(TempDir(), L"LemonShotSelfTest");
        return;
    }
    std::wstring exeDir = ExeDir();
    if (FileExists(PathJoin(exeDir, L"portable")))
        g_dir = PathJoin(exeDir, L"data");
    else
        g_dir = PathJoin(KnownFolder(FOLDERID_RoamingAppData), L"LemonShot");
    EnsureDir(g_dir);
    std::string data;
    if (ReadFileBytes(SettingsFile(), data)) {
        Json user;
        if (Json::Parse(data, user) && user.IsObject())
            g_root.DeepMerge(user);
        else
            LS_LOG("settings: settings.json 解析失败，使用默认值");
    }
    if (ReadFileBytes(StateFile(), data)) {
        Json st;
        if (Json::Parse(data, st) && st.IsObject()) g_state = st;
    }
}

bool IsSelfTest() { return g_selftest; }
const Json &Root() { return g_root; }
const Json &Defaults() { return g_defaults; }

const Json *Get(const std::string &path) {
    const Json *j = g_root.Path(path);
    if (!j) j = g_defaults.Path(path);
    return j;
}

bool Bool(const std::string &path) {
    const Json *j = Get(path);
    return j ? j->AsBool(false) : false;
}

double Num(const std::string &path) {
    const Json *j = Get(path);
    return j ? j->AsNumber(0) : 0;
}

int Int(const std::string &path) {
    double v = Num(path);
    return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

std::string Str(const std::string &path) {
    const Json *j = Get(path);
    return (j && j->IsString()) ? j->AsString() : std::string();
}

void SaveNow() {
    if (g_selftest) return;
    EnsureDir(g_dir);
    std::string s = g_root.Dump(true);
    if (!WriteFileAtomic(SettingsFile(), s.data(), s.size())) LS_LOG("settings: 写入失败 %lu", GetLastError());
}

void SetSaveScheduler(void (*schedule)()) { g_scheduler = schedule; }

void Set(const std::string &path, Json v) {
    const Json *old = g_root.Path(path);
    if (old && old->Equals(v)) return;
    g_root.SetPath(path, std::move(v));
    if (!g_selftest) {
        if (g_scheduler)
            g_scheduler();
        else
            SaveNow();
    }
    Notify(path);
}

void SetBool(const std::string &path, bool v) { Set(path, Json::MakeBool(v)); }
void SetNum(const std::string &path, double v) { Set(path, Json::MakeNumber(v)); }
void SetStr(const std::string &path, const std::string &v) { Set(path, Json::MakeString(v)); }

void Unset(const std::string &path) {
    size_t dot = path.rfind('.');
    Json *parent = dot == std::string::npos ? &g_root : g_root.Path(path.substr(0, dot));
    if (!parent || !parent->Remove(dot == std::string::npos ? path : path.substr(dot + 1))) return;
    if (!g_selftest) {
        if (g_scheduler)
            g_scheduler();
        else
            SaveNow();
    }
    Notify(path);
}

void AddListener(Listener fn, void *ctx) { g_listeners.push_back({fn, ctx}); }

void RemoveListener(Listener fn, void *ctx) {
    for (size_t i = 0; i < g_listeners.size(); ++i) {
        if (g_listeners[i].fn == fn && g_listeners[i].ctx == ctx) {
            g_listeners.erase(g_listeners.begin() + (long)i);
            return;
        }
    }
}

void Reset(const std::string &section) {
    if (section.empty()) {
        g_root = g_defaults;
    } else {
        const Json *d = g_defaults.Find(section);
        if (d)
            g_root.Set(section, *d);
        else
            g_root.Remove(section);
    }
    SaveNow();
    Notify(section);
}

bool ExportTo(const std::wstring &file) {
    std::string s = g_root.Dump(true);
    return WriteFileAtomic(file, s.data(), s.size());
}

bool ImportFrom(const std::wstring &file) {
    std::string data;
    Json user;
    if (!ReadFileBytes(file, data) || !Json::Parse(data, user) || !user.IsObject()) return false;
    Json merged = g_defaults;
    merged.DeepMerge(user);
    g_root = merged;
    SaveNow();
    Notify(std::string());
    return true;
}

std::wstring DataDir() {
    EnsureDir(g_dir);
    return g_dir;
}

std::wstring PinsDir() {
    std::wstring d = PathJoin(DataDir(), L"pins");
    EnsureDir(d);
    return d;
}

std::wstring ExpandPath(const std::string &path) {
    std::wstring p = Widen(path);
    struct {
        const wchar_t *token;
        const GUID *id;
    } map[] = {{L"{pictures}", &FOLDERID_Pictures}, {L"{movies}", &FOLDERID_Videos}, {L"{music}", &FOLDERID_Music}};
    for (auto &m : map) {
        if (p.find(m.token) != std::wstring::npos) {
            std::wstring dir = KnownFolder(*m.id);
            if (dir.empty()) dir = KnownFolder(FOLDERID_Profile);
            p = ReplaceAll(p, m.token, dir);
        }
    }
    for (auto &c : p)
        if (c == L'/') c = L'\\';
    return p;
}

Json &State() { return g_state; }

void SaveState() {
    if (g_selftest) return;
    EnsureDir(g_dir);
    std::string s = g_state.Dump(true);
    WriteFileAtomic(StateFile(), s.data(), s.size());
}

}  // namespace settings
}  // namespace ls
