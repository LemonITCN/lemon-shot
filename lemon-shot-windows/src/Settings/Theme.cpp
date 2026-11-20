#include "Settings/Theme.h"

#include <windows.h>

#include <cstdio>

#include "../../res/resource.h"
#include "Settings/Settings.h"
#include "Util/Api.h"
#include "Util/Json.h"
#include "Util/Resources.h"
#include "Util/Util.h"

namespace ls {

static int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Color ParseColor(const std::string &hex, Color def) {
    if (hex.size() < 7 || hex[0] != '#') return def;
    int v[8];
    size_t n = hex.size() - 1;
    if (n != 6 && n != 8) return def;
    for (size_t i = 0; i < n; ++i) {
        v[i] = HexDigit(hex[i + 1]);
        if (v[i] < 0) return def;
    }
    Color c;
    c.r = (float)(v[0] * 16 + v[1]) / 255.f;
    c.g = (float)(v[2] * 16 + v[3]) / 255.f;
    c.b = (float)(v[4] * 16 + v[5]) / 255.f;
    c.a = n == 8 ? (float)(v[6] * 16 + v[7]) / 255.f : 1.f;
    return c;
}

std::string ColorToHex(Color c, bool withAlpha) {
    auto b = [](float f) { return (int)(Clamp(f, 0.f, 1.f) * 255.f + 0.5f); };
    char buf[16];
    if (withAlpha)
        snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", b(c.r), b(c.g), b(c.b), b(c.a));
    else
        snprintf(buf, sizeof buf, "#%02X%02X%02X", b(c.r), b(c.g), b(c.b));
    return buf;
}

namespace theme {

namespace {

Tokens g_tok;
bool g_dark = false;
int g_force = 0;

float N(const Json &j, const char *path, float def) { return (float)j.GetNumber(path, def); }
Color C(const Json &j, const std::string &path, const char *def) {
    return ParseColor(j.GetString(path, def), ParseColor(def));
}

void LoadUi(const Json &j, const std::string &base, UiColors &u, bool dark) {
    auto c = [&](const char *name, const char *defLight, const char *defDark) {
        return C(j, base + "." + name, dark ? defDark : defLight);
    };
    u.accent = c("accent", "#2F6BFF", "#4C82FF");
    u.accentHover = c("accentHover", "#2559D9", "#6B98FF");
    u.accentText = c("accentText", "#FFFFFF", "#FFFFFF");
    u.danger = c("danger", "#E5484D", "#FF6369");
    u.success = c("success", "#30A46C", "#3DD68C");
    u.warning = c("warning", "#F5A524", "#FFB224");
    u.windowBg = c("windowBg", "#F5F5F7", "#1C1C1E");
    u.surface = c("surface", "#FFFFFF", "#2C2C2E");
    u.surfaceHover = c("surfaceHover", "#F0F0F2", "#363638");
    u.surfacePressed = c("surfacePressed", "#E6E6EA", "#404043");
    u.surfaceSelected = c("surfaceSelected", "#E8EEFF", "#24345C");
    u.border = c("border", "#E2E2E6", "#3A3A3C");
    u.divider = c("divider", "#ECECEF", "#333336");
    u.text = c("text", "#1D1D1F", "#F5F5F7");
    u.textSecondary = c("textSecondary", "#6E6E73", "#A1A1A6");
    u.textTertiary = c("textTertiary", "#A1A1A6", "#6E6E73");
    u.icon = c("icon", "#3A3A3C", "#E5E5EA");
    u.iconActive = c("iconActive", "#2F6BFF", "#6B98FF");
    u.toolbarBg = c("toolbarBg", "#FFFFFFF2", "#2C2C2EF2");
    u.toolbarBorder = c("toolbarBorder", "#00000014", "#FFFFFF1F");
    u.popoverBg = c("popoverBg", "#FFFFFFF7", "#2C2C2EF7");
    u.shadow = c("shadow", "#00000033", "#00000080");
    u.controlTrack = c("controlTrack", "#E3E3E8", "#48484A");
    u.controlKnob = c("controlKnob", "#FFFFFF", "#F5F5F7");
    u.inputBg = c("inputBg", "#FFFFFF", "#1C1C1E");
    u.inputBorder = c("inputBorder", "#D2D2D7", "#48484A");
    u.focusRing = c("focusRing", "#2F6BFF66", "#4C82FF80");
}

std::vector<std::wstring> Families(const Json *arr, const wchar_t *fallback) {
    std::vector<std::wstring> out;
    if (arr && arr->IsArray()) {
        for (size_t i = 0; i < arr->Size(); ++i)
            if (arr->At(i).IsString()) out.push_back(Widen(arr->At(i).AsString()));
    }
    if (out.empty()) out.push_back(fallback);
    return out;
}

bool ReadAppsUseLightTheme(const wchar_t *value, DWORD &out) {
    DWORD type = 0, size = sizeof(DWORD);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", value,
                        RRF_RT_REG_DWORD, &type, &out, &size) == ERROR_SUCCESS;
}

}  // namespace

void Init() {
    Json j;
    std::string data = LoadResourceData(IDR_TOKENS);
    if (!Json::Parse(data, j)) j = Json::MakeObject();
    Tokens &t = g_tok;
    t.fontFamilies = Families(j.Path("font.win"), L"Segoe UI");
    t.monoFamilies = Families(j.Path("font.mono.win"), L"Consolas");
    t.fontSize = {N(j, "font.size.caption", 11), N(j, "font.size.body", 13), N(j, "font.size.title", 15),
                  N(j, "font.size.header", 20)};
    auto &c = t.capture;
    c.selectionBorderWidth = N(j, "capture.selectionBorderWidth", 1);
    c.handleSize = N(j, "capture.handleSize", 7);
    c.handleHitSize = N(j, "capture.handleHitSize", 14);
    c.edgeHitSize = N(j, "capture.edgeHitSize", 6);
    c.minSelection = N(j, "capture.minSelection", 4);
    c.windowHoverBorderWidth = N(j, "capture.windowHoverBorderWidth", 3);
    c.tipHeight = N(j, "capture.tipHeight", 24);
    c.tipPaddingX = N(j, "capture.tipPaddingX", 8);
    c.tipRadius = N(j, "capture.tipRadius", 4);
    c.tipGap = N(j, "capture.tipGap", 6);
    c.toolbarGap = N(j, "capture.toolbarGap", 8);
    c.toolbarScreenMargin = N(j, "capture.toolbarScreenMargin", 6);
    c.magnifier = {N(j, "capture.magnifier.zoom", 8),       N(j, "capture.magnifier.pixels", 15),
                   N(j, "capture.magnifier.size", 120),     N(j, "capture.magnifier.infoHeight", 64),
                   N(j, "capture.magnifier.radius", 6),     N(j, "capture.magnifier.offset", 20)};
    auto &tb = t.toolbar;
    tb.height = N(j, "toolbar.height", 40);
    tb.paddingX = N(j, "toolbar.paddingX", 6);
    tb.radius = N(j, "toolbar.radius", 10);
    tb.button = N(j, "toolbar.button", 30);
    tb.buttonRadius = N(j, "toolbar.buttonRadius", 6);
    tb.iconSize = N(j, "toolbar.iconSize", 18);
    tb.iconStroke = N(j, "toolbar.iconStroke", 1.75f);
    tb.spacing = N(j, "toolbar.spacing", 2);
    tb.separatorWidth = N(j, "toolbar.separatorWidth", 1);
    tb.separatorHeight = N(j, "toolbar.separatorHeight", 18);
    tb.separatorMargin = N(j, "toolbar.separatorMargin", 6);
    tb.shadowBlur = N(j, "toolbar.shadowBlur", 16);
    tb.shadowOffsetY = N(j, "toolbar.shadowOffsetY", 4);
    auto &p = t.popover;
    p.radius = N(j, "popover.radius", 10);
    p.padding = N(j, "popover.padding", 10);
    p.gap = N(j, "popover.gap", 6);
    p.arrow = N(j, "popover.arrow", 0);
    p.swatch = N(j, "popover.swatch", 18);
    p.swatchGap = N(j, "popover.swatchGap", 6);
    p.swatchRadius = N(j, "popover.swatchRadius", 9);
    p.sliderWidth = N(j, "popover.sliderWidth", 140);
    p.rowHeight = N(j, "popover.rowHeight", 28);
    auto &w = t.window;
    w.radius = N(j, "window.radius", 12);
    w.titleBarHeight = N(j, "window.titleBarHeight", 44);
    w.sidebarWidth = N(j, "window.sidebarWidth", 180);
    w.contentPadding = N(j, "window.contentPadding", 24);
    w.sectionGap = N(j, "window.sectionGap", 20);
    w.rowHeight = N(j, "window.rowHeight", 36);
    w.rowGap = N(j, "window.rowGap", 4);
    w.labelWidth = N(j, "window.labelWidth", 160);
    w.controlHeight = N(j, "window.controlHeight", 28);
    w.controlRadius = N(j, "window.controlRadius", 6);
    w.switchWidth = N(j, "window.switchWidth", 36);
    w.switchHeight = N(j, "window.switchHeight", 20);
    w.checkbox = N(j, "window.checkbox", 16);
    w.buttonPaddingX = N(j, "window.buttonPaddingX", 14);
    w.sidebarItemHeight = N(j, "window.sidebarItemHeight", 32);
    w.sidebarItemRadius = N(j, "window.sidebarItemRadius", 6);
    w.sidebarIcon = N(j, "window.sidebarIcon", 16);
    auto &pn = t.pin;
    pn.minSize = N(j, "pin.minSize", 24);
    pn.borderWidth = N(j, "pin.borderWidth", 1);
    pn.activeBorder = C(j, "pin.activeBorder", "#2F6BFF");
    pn.inactiveBorder = C(j, "pin.inactiveBorder", "#00000026");
    pn.shadowBlur = N(j, "pin.shadowBlur", 18);
    pn.shadowOffsetY = N(j, "pin.shadowOffsetY", 6);
    pn.zoomStep = N(j, "pin.zoomStep", 0.1f);
    pn.zoomMin = N(j, "pin.zoomMin", 0.1f);
    pn.zoomMax = N(j, "pin.zoomMax", 8.f);
    pn.opacityStep = N(j, "pin.opacityStep", 0.1f);
    auto &rb = t.recordBar;
    rb.height = N(j, "recordBar.height", 40);
    rb.radius = N(j, "recordBar.radius", 20);
    rb.countdownSize = N(j, "recordBar.countdownSize", 120);
    rb.regionBorder = C(j, "recordBar.regionBorder", "#FF3B30");
    rb.regionBorderWidth = N(j, "recordBar.regionBorderWidth", 2);
    rb.dot = C(j, "recordBar.dot", "#FF3B30");
    t.animation = {N(j, "animation.fastMs", 120), N(j, "animation.normalMs", 200)};
    LoadUi(j, "color.light", t.light, false);
    LoadUi(j, "color.dark", t.dark, true);
    auto &k = t.cap;
    k.mask = C(j, "color.capture.mask", "#00000073");
    k.selectionBorder = C(j, "color.capture.selectionBorder", "#2F6BFF");
    k.windowHover = C(j, "color.capture.windowHover", "#2F6BFF");
    k.windowHoverFill = C(j, "color.capture.windowHoverFill", "#2F6BFF1F");
    k.handleFill = C(j, "color.capture.handleFill", "#FFFFFF");
    k.handleBorder = C(j, "color.capture.handleBorder", "#2F6BFF");
    k.tipBg = C(j, "color.capture.tipBg", "#000000B8");
    k.tipText = C(j, "color.capture.tipText", "#FFFFFF");
    k.crosshair = C(j, "color.capture.crosshair", "#2F6BFFB3");
    k.magnifierBorder = C(j, "color.capture.magnifierBorder", "#FFFFFF");
    k.magnifierGrid = C(j, "color.capture.magnifierGrid", "#FFFFFF26");
    k.magnifierCenter = C(j, "color.capture.magnifierCenter", "#2F6BFF");
    k.shapeHandleFill = C(j, "color.capture.shapeHandleFill", "#FFFFFF");
    k.shapeHandleBorder = C(j, "color.capture.shapeHandleBorder", "#2F6BFF");
    k.shapeSelectionDash = C(j, "color.capture.shapeSelectionDash", "#2F6BFF");
    Refresh();
}

const Tokens &Tok() { return g_tok; }
const UiColors &Ui() { return g_dark ? g_tok.dark : g_tok.light; }
const CaptureColors &Cap() { return g_tok.cap; }
bool IsDark() { return g_dark; }

bool Refresh() {
    bool dark = false;
    if (g_force == 1) {
        dark = false;
    } else if (g_force == 2) {
        dark = true;
    } else {
        std::string mode = settings::Str("general.theme");
        if (mode == "dark") {
            dark = true;
        } else if (mode == "light") {
            dark = false;
        } else {
            // 跟随系统：Win10 1809+ 才有这个值；Win7/8 没有 → 浅色
            DWORD v = 1;
            dark = ReadAppsUseLightTheme(L"AppsUseLightTheme", v) && v == 0;
        }
    }
    bool changed = dark != g_dark;
    g_dark = dark;
    return changed;
}

void ForceMode(int mode) {
    g_force = mode;
    Refresh();
}

bool SystemTaskbarDark() {
    DWORD v = 0;
    if (ReadAppsUseLightTheme(L"SystemUsesLightTheme", v)) return v == 0;
    return true;
}

}  // namespace theme
}  // namespace ls
