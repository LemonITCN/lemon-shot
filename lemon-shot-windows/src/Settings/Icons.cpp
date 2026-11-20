#include "Settings/Icons.h"

#include <map>
#include <string>

#include "../../res/resource.h"
#include "UI/Gfx.h"
#include "Util/Json.h"
#include "Util/Log.h"
#include "Util/Resources.h"

namespace ls {
namespace icons {

namespace {

struct Entry {
    std::string d;
    ComPtr<ID2D1PathGeometry> geo;
    bool tried = false;
};
std::map<std::string, Entry> g_icons;

}  // namespace

void Init() {
    g_icons.clear();
    Json j;
    if (!Json::Parse(LoadResourceData(IDR_ICONS), j)) {
        LS_LOG("icons: icons.json 解析失败");
        return;
    }
    const Json *icons = j.Find("icons");
    if (!icons || !icons->IsObject()) return;
    for (size_t i = 0; i < icons->Keys().size(); ++i) {
        const Json &v = icons->ValueAt(i);
        const Json *d = v.Find("d");
        if (d && d->IsString()) g_icons[icons->Keys()[i]].d = d->AsString();
    }
}

ID2D1Geometry *Get(const char *name) {
    auto it = g_icons.find(name);
    if (it == g_icons.end()) return nullptr;
    Entry &e = it->second;
    if (!e.tried) {
        e.tried = true;
        ls_path *path = ls_path_create();
        if (ls_svg_path_parse(e.d.c_str(), 1.f, 0.f, 0.f, path) != 0) LS_LOG("icons: %s 路径有语法错误", name);
        // 图标只描边（少数实心图示也是闭合图形），非零规则即可
        e.geo = gfx::PathGeometry(path, false);
        ls_path_destroy(path);
    }
    return e.geo.Get();
}

void Shutdown() { g_icons.clear(); }

}  // namespace icons
}  // namespace ls
