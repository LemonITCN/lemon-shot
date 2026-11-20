#include "Capture/StyleSettings.h"

#include <cmath>
#include <cstring>

#include "Settings/Settings.h"

namespace ls {
namespace style {

const float kLineWidths[3] = {2.f, 3.f, 6.f};

static const char *kKeys[LS_TOOL_COUNT] = {"",     "rect", "ellipse", "line", "arrow",    "pen",
                                           "mosaic", "text", "step",    "highlight", "magnify"};
static const char *kIcons[LS_TOOL_COUNT] = {"",       "square",   "circle",       "slash", "move-up-right", "pencil",
                                            "grid-3x3", "type", "list-ordered", "focus", "zoom-in"};

const char *ToolKey(int tool) { return tool > 0 && tool < LS_TOOL_COUNT ? kKeys[tool] : ""; }

const char *ToolNameKey(int tool) {
    static const char *names[LS_TOOL_COUNT] = {"",          "tool.rect", "tool.ellipse", "tool.line",
                                               "tool.arrow", "tool.pen", "tool.mosaic",  "tool.text",
                                               "tool.step",  "tool.highlight", "tool.magnify"};
    return tool > 0 && tool < LS_TOOL_COUNT ? names[tool] : "";
}

const char *ToolIcon(int tool) { return tool > 0 && tool < LS_TOOL_COUNT ? kIcons[tool] : ""; }

std::vector<Color> Palette() {
    std::vector<Color> out;
    const Json *p = settings::Get("annotate.palette");
    if (p && p->IsArray())
        for (size_t i = 0; i < p->Size(); ++i)
            if (p->At(i).IsString()) out.push_back(ParseColor(p->At(i).AsString()));
    while (out.size() < 10) out.push_back(Color{1, 0.23f, 0.19f, 1});
    return out;
}

ls_color ToLs(Color c) {
    auto b = [](float f) { return (uint8_t)(f <= 0 ? 0 : (f >= 1 ? 255 : (int)(f * 255.f + 0.5f))); };
    return ls_color{b(c.r), b(c.g), b(c.b), b(c.a)};
}

Color FromLs(ls_color c) { return Color{c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f}; }

static Color ColorSetting(const std::string &base, const std::vector<Color> &pal) {
    const Json *c = settings::Get(base + ".color");
    if (c && c->IsString()) return ParseColor(c->AsString(), pal[0]);
    int idx = c && c->IsNumber() ? (int)c->AsNumber() : 0;
    if (idx < 0 || idx >= (int)pal.size()) idx = 0;
    return pal[(size_t)idx];
}

static double NumOr(const std::string &path, double def) {
    const Json *j = settings::Get(path);
    return j && j->IsNumber() ? j->AsNumber() : def;
}

static bool BoolOr(const std::string &path, bool def) {
    const Json *j = settings::Get(path);
    return j && j->IsBool() ? j->AsBool() : def;
}

ls_style FromSettings(int tool) {
    ls_style st;
    std::memset(&st, 0, sizeof st);
    std::vector<Color> pal = Palette();
    std::string base = std::string("annotate.") + ToolKey(tool);
    st.color = ToLs(ColorSetting(base, pal));
    st.opacity = (float)NumOr(base + ".opacity", 1.0);
    st.line_width = (float)NumOr(base + ".lineWidth", 3.0);
    st.variant = (int32_t)NumOr(base + ".variant", 0);
    st.filled = BoolOr(base + ".filled", false) ? 1 : 0;
    st.corner_radius = (float)NumOr(base + ".cornerRadius", 0);
    st.font_size = 18;
    st.block_size = 10;
    st.zoom = 2;
    st.next_number = 1;
    switch (tool) {
    case LS_TOOL_MOSAIC:
        st.block_size = (float)NumOr(base + ".blockSize", 10);
        st.line_width = (float)NumOr(base + ".brushSize", 24);
        break;
    case LS_TOOL_TEXT:
        st.font_size = (float)NumOr(base + ".fontSize", 18);
        st.bold = BoolOr(base + ".bold", false) ? 1 : 0;
        break;
    case LS_TOOL_STEP:
        st.line_width = (float)NumOr(base + ".size", 24);
        st.next_number = (int32_t)NumOr(base + ".next", 1);
        if (st.next_number < 1) st.next_number = 1;
        break;
    case LS_TOOL_HIGHLIGHT:
        st.opacity = (float)NumOr(base + ".opacity", 0.55);
        st.corner_radius = (float)NumOr(base + ".cornerRadius", 6);
        break;
    case LS_TOOL_MAGNIFY:
        st.zoom = (float)NumOr(base + ".zoom", 2.0);
        st.line_width = (float)NumOr(base + ".lineWidth", 3.0);
        break;
    default: break;
    }
    if (st.opacity < 0.05f) st.opacity = 0.05f;
    if (st.opacity > 1.f) st.opacity = 1.f;
    return st;
}

static Json ColorJson(ls_color c) {
    std::vector<Color> pal = Palette();
    for (size_t i = 0; i < pal.size(); ++i) {
        ls_color p = ToLs(pal[i]);
        if (p.r == c.r && p.g == c.g && p.b == c.b) return Json::MakeNumber((double)i);
    }
    return Json::MakeString(ColorToHex(FromLs(c), false));
}

static double Round2(double v) { return std::floor(v * 100.0 + 0.5) / 100.0; }

void ToSettings(int tool, const ls_style &st) {
    std::string base = std::string("annotate.") + ToolKey(tool);
    if (tool <= 0 || tool >= LS_TOOL_COUNT) return;
    switch (tool) {
    case LS_TOOL_RECT:
        settings::Set(base + ".color", ColorJson(st.color));
        settings::SetNum(base + ".lineWidth", st.line_width);
        settings::SetBool(base + ".filled", st.filled != 0);
        settings::SetNum(base + ".cornerRadius", st.corner_radius);
        break;
    case LS_TOOL_ELLIPSE:
        settings::Set(base + ".color", ColorJson(st.color));
        settings::SetNum(base + ".lineWidth", st.line_width);
        settings::SetBool(base + ".filled", st.filled != 0);
        break;
    case LS_TOOL_LINE:
    case LS_TOOL_ARROW:
    case LS_TOOL_PEN:
        settings::Set(base + ".color", ColorJson(st.color));
        settings::SetNum(base + ".lineWidth", st.line_width);
        settings::SetNum(base + ".variant", st.variant);
        break;
    case LS_TOOL_MOSAIC:
        settings::SetNum(base + ".variant", st.variant);
        settings::SetNum(base + ".blockSize", st.block_size);
        settings::SetNum(base + ".brushSize", st.line_width);
        break;
    case LS_TOOL_TEXT:
        settings::Set(base + ".color", ColorJson(st.color));
        settings::SetNum(base + ".fontSize", st.font_size);
        settings::SetNum(base + ".variant", st.variant);
        settings::SetBool(base + ".bold", st.bold != 0);
        break;
    case LS_TOOL_STEP:
        settings::Set(base + ".color", ColorJson(st.color));
        settings::SetNum(base + ".size", st.line_width);
        settings::SetNum(base + ".variant", st.variant);
        break;
    case LS_TOOL_HIGHLIGHT:
        settings::SetNum(base + ".opacity", Round2(st.opacity));
        settings::SetNum(base + ".cornerRadius", st.corner_radius);
        break;
    case LS_TOOL_MAGNIFY:
        settings::Set(base + ".color", ColorJson(st.color));
        settings::SetNum(base + ".zoom", st.zoom);
        settings::SetNum(base + ".lineWidth", st.line_width);
        break;
    default: break;
    }
}

void ApplyAll(ls_session *s) {
    for (int t = 1; t < LS_TOOL_COUNT; ++t) {
        ls_style st = FromSettings(t);
        ls_session_set_style(s, t, &st);
    }
}

int NearestLevel(float w) {
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < 3; ++i) {
        float d = std::fabs(kLineWidths[i] - w);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

}  // namespace style
}  // namespace ls
