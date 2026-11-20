#include "Record/RecordUI.h"

#include <cmath>

#include "../../res/resource.h"
#include "Settings/I18n.h"
#include "Util/Json.h"
#include "Util/Resources.h"
#include "Util/Util.h"

namespace ls {
namespace rec {

namespace {

const Tokens &K() { return theme::Tok(); }
const UiColors &U() { return theme::Ui(); }

TextStyle Ts(float size, int weight, Color c, HAlign h = HAlign::Left) {
    TextStyle t;
    t.size = size;
    t.weight = weight;
    t.color = c;
    t.h = h;
    t.v = VAlign::Center;
    return t;
}

float BodySize() { return K().fontSize.body; }

}  // namespace

const OverlayTokens &OTok() {
    static OverlayTokens t = [] {
        OverlayTokens o;
        o.lightBg = ParseColor("#FFFFFFE6");
        o.lightText = ParseColor("#1D1D1F");
        o.darkBg = ParseColor("#1C1C1EE6");
        o.darkText = ParseColor("#FFFFFF");
        o.clickColor = ParseColor("#2F6BFF66");
        o.clickRightColor = ParseColor("#FF950066");
        Json j;
        std::string raw = LoadResourceData(IDR_TOKENS);
        if (!raw.empty() && Json::Parse(raw, j)) {
            auto sz = [&](const char *name, OverlayTokens::KeySize &k) {
                std::string b = std::string("keysOverlay.") + name + ".";
                k.fontSize = (float)j.GetNumber(b + "fontSize", k.fontSize);
                k.padding = (float)j.GetNumber(b + "padding", k.padding);
                k.radius = (float)j.GetNumber(b + "radius", k.radius);
            };
            sz("small", o.small);
            sz("medium", o.medium);
            sz("large", o.large);
            o.lightBg = ParseColor(j.GetString("keysOverlay.lightBg", ""), o.lightBg);
            o.lightText = ParseColor(j.GetString("keysOverlay.lightText", ""), o.lightText);
            o.darkBg = ParseColor(j.GetString("keysOverlay.darkBg", ""), o.darkBg);
            o.darkText = ParseColor(j.GetString("keysOverlay.darkText", ""), o.darkText);
            o.margin = (float)j.GetNumber("keysOverlay.margin", o.margin);
            o.gap = (float)j.GetNumber("keysOverlay.gap", o.gap);
            o.clickRadius = (float)j.GetNumber("clickOverlay.radius", o.clickRadius);
            o.clickColor = ParseColor(j.GetString("clickOverlay.color", ""), o.clickColor);
            o.clickRightColor = ParseColor(j.GetString("clickOverlay.rightColor", ""), o.clickRightColor);
            o.clickMs = (float)j.GetNumber("clickOverlay.durationMs", o.clickMs);
        }
        return o;
    }();
    return t;
}

// ---------------------------------------------------------------- 小控件

void DrawToggle(Canvas &c, RectF r, const char *icon, bool on, bool hover, bool pressed, bool enabled) {
    const auto &u = U();
    float a = enabled ? 1.f : 0.45f;
    float rad = K().toolbar.buttonRadius;
    if (on)
        c.FillRound(r, rad, WithAlpha(u.surfaceSelected, a));
    else if (enabled && pressed)
        c.FillRound(r, rad, u.surfacePressed);
    else if (enabled && hover)
        c.FillRound(r, rad, u.surfaceHover);
    c.Icon(icon, r.cx(), r.cy(), K().toolbar.iconSize, WithAlpha(on ? u.iconActive : u.icon, a));
}

float ChipWidth(const std::vector<std::wstring> &labels) {
    float tw = 40;
    bool first = true;
    for (auto &l : labels) {
        float w = gfx::Measure(l, BodySize(), kRegular).w;
        if (first || w > tw) tw = w;
        first = false;
    }
    return std::ceil(tw + 8 + 4 + 12 + 6);
}

void DrawChip(Canvas &c, RectF r, const std::wstring &label, bool hover, bool pressed, bool enabled) {
    const auto &u = U();
    float a = enabled ? 1.f : 0.45f;
    float rad = K().toolbar.buttonRadius;
    if (enabled && pressed)
        c.FillRound(r, rad, u.surfacePressed);
    else if (enabled && hover)
        c.FillRound(r, rad, u.surfaceHover);
    c.Text(label, RectF(r.x + 8, r.y, r.w - 30, r.h), Ts(BodySize(), kRegular, WithAlpha(u.text, a)));
    c.Icon("chevron-down", r.r() - 12, r.cy(), 12, WithAlpha(u.textSecondary, a), 1.6f);
}

float PrimaryButtonWidth(const std::wstring &text) {
    return std::ceil(gfx::Measure(text, BodySize(), kMedium).w + 2 * K().window.buttonPaddingX);
}

void DrawPrimaryButton(Canvas &c, RectF r, const std::wstring &text, bool hover, bool pressed, bool enabled, bool danger) {
    const auto &u = U();
    Color bg = danger ? u.danger : ((hover || pressed) && enabled ? u.accentHover : u.accent);
    if (danger && (hover || pressed) && enabled) bg = WithAlpha(bg, 0.88f);
    if (!enabled) bg = WithAlpha(bg, 0.5f);
    c.FillRound(r, K().window.controlRadius, bg);
    c.Text(text, r, Ts(BodySize(), kMedium, u.accentText, HAlign::Center));
}

void DrawBarPanel(Canvas &c, RectF r, float radius) {
    const auto &t = K().toolbar;
    const auto &u = U();
    c.Shadow(r, radius, t.shadowBlur, t.shadowOffsetY, u.shadow);
    c.FillRound(r, radius, u.toolbarBg);
    c.StrokeRound(r.Inflate(-0.5f), radius - 0.5f, u.toolbarBorder, 1);
}

// ---------------------------------------------------------------- 控制条

namespace {
const float kPadLeft = 14, kDot = 8;

float ClockWidth(const std::wstring &clock) {
    const wchar_t *sample = clock.size() > 5 ? L"0:00:00" : L"00:00";
    return std::ceil(gfx::Measure(sample, BodySize(), kMedium).w) + 2;
}

float SepSpace() { return 2 * K().toolbar.separatorMargin + K().toolbar.separatorWidth; }
}  // namespace

SizeF BarView::Size() const {
    const auto &t = K().toolbar;
    float h = K().recordBar.height;
    float w = kPadLeft + kDot + 8 + ClockWidth(clock) + SepSpace() + 4 * t.button + 3 * t.spacing + (h - t.button) / 2;
    return SizeF{std::ceil(w), h};
}

RectF BarView::ButtonRect(RectF bar, int id) const {
    if (id < BAR_PAUSE || id > BAR_CANCEL) return RectF();
    const auto &t = K().toolbar;
    float sepX = bar.x + kPadLeft + kDot + 8 + ClockWidth(clock) + t.separatorMargin;
    float x = sepX + t.separatorWidth + t.separatorMargin + (id - BAR_PAUSE) * (t.button + t.spacing);
    return RectF(x, bar.cy() - t.button / 2, t.button, t.button);
}

int BarView::Hit(RectF bar, float x, float y) const {
    for (int id = BAR_PAUSE; id <= BAR_CANCEL; ++id)
        if (ButtonRect(bar, id).Inflate(1).Contains(x, y)) return id;
    return BAR_NONE;
}

std::wstring BarView::Tooltip(int id, bool paused) {
    switch (id) {
    case BAR_PAUSE: return T(paused ? "record.resume" : "record.pause");
    case BAR_PEN: return T("record.pen");
    case BAR_STOP: return T("record.stop");
    case BAR_CANCEL: return T("record.cancel");
    }
    return std::wstring();
}

void BarView::Draw(Canvas &c, RectF r) const {
    const auto &u = U();
    const auto &t = K().toolbar;
    const auto &rb = K().recordBar;
    DrawBarPanel(c, r, rb.radius);
    c.FillEllipse(r.x + kPadLeft + kDot / 2, r.cy(), kDot / 2, kDot / 2, paused ? u.textTertiary : rb.dot);
    float cx = r.x + kPadLeft + kDot + 8;
    float cw = ClockWidth(clock);
    TextStyle ts = Ts(BodySize(), kMedium, paused ? u.textSecondary : u.text);
    ts.tabular = true;
    c.Text(clock, RectF(cx, r.y, cw + 4, r.h), ts);
    float sepX = cx + cw + t.separatorMargin;
    c.FillRect(RectF(sepX, r.cy() - t.separatorHeight / 2, t.separatorWidth, t.separatorHeight), u.divider);
    for (int id = BAR_PAUSE; id <= BAR_CANCEL; ++id) {
        RectF b = ButtonRect(r, id);
        float rad = b.w / 2;
        bool sel = id == BAR_PEN && penOn;
        if (sel)
            c.FillRound(b, rad, u.surfaceSelected);
        else if (pressed == id)
            c.FillRound(b, rad, u.surfacePressed);
        else if (hover == id)
            c.FillRound(b, rad, u.surfaceHover);
        Color ic = sel ? u.iconActive : u.icon;
        switch (id) {
        case BAR_PAUSE: c.Icon(paused ? "play" : "pause", b.cx(), b.cy(), t.iconSize, ic); break;
        case BAR_PEN: c.Icon("pencil", b.cx(), b.cy(), t.iconSize, ic); break;
        case BAR_STOP: c.Icon("square", b.cx(), b.cy(), t.iconSize - 2, rb.dot, -1.f, true); break;
        case BAR_CANCEL: c.Icon("x", b.cx(), b.cy(), t.iconSize, ic); break;
        }
    }
}

// ---------------------------------------------------------------- 按键浮层

const OverlayTokens::KeySize &KeysView::Metrics() const {
    const OverlayTokens &o = OTok();
    if (size == "small") return o.small;
    if (size == "large") return o.large;
    return o.medium;
}

SizeF KeysView::PillSize(const std::wstring &text) const {
    const auto &m = Metrics();
    float lineH = gfx::Measure(L"Ag", m.fontSize, kMedium).h;
    float h = std::ceil(lineH + 2 * m.padding);
    float tw = gfx::Measure(text, m.fontSize, kMedium).w + 2 * m.padding * 1.4f;
    return SizeF{std::ceil(tw > h ? tw : h), h};
}

std::vector<std::pair<int, RectF>> KeysView::Layout(RectF r) const {
    std::vector<std::pair<int, RectF>> out;
    if (items.empty()) return out;
    const OverlayTokens &o = OTok();
    std::vector<SizeF> sizes;
    for (auto &it : items) sizes.push_back(PillSize(it.text));
    size_t start = 0;
    float margin = o.margin < r.w / 6 ? o.margin : r.w / 6;
    float avail = r.w - 2 * margin;
    if (avail < 0) avail = 0;
    auto total = [&](size_t from) {
        float w = 0;
        for (size_t i = from; i < sizes.size(); ++i) w += sizes[i].w;
        if (sizes.size() > from + 1) w += o.gap * (float)(sizes.size() - from - 1);
        return w;
    };
    while (start + 1 < sizes.size() && total(start) > avail) ++start;
    float w = total(start), h = 0;
    for (size_t i = start; i < sizes.size(); ++i) h = sizes[i].h > h ? sizes[i].h : h;
    float m = (r.h - h) / 2;
    if (m < 8) m = 8;
    if (m > o.margin) m = o.margin;
    float x;
    if (position == "bottomLeft")
        x = r.x + m;
    else if (position == "bottomRight")
        x = r.r() - m - w;
    else
        x = r.cx() - w / 2;
    float y = position == "topCenter" ? r.y + m : r.b() - m - h;
    for (size_t i = start; i < sizes.size(); ++i) {
        RectF pr(std::floor(x), std::floor(y + (h - sizes[i].h) / 2), sizes[i].w, sizes[i].h);
        out.push_back(std::make_pair((int)i, pr));
        x += sizes[i].w + o.gap;
    }
    return out;
}

void KeysView::Draw(Canvas &c, RectF r) const {
    const OverlayTokens &o = OTok();
    const auto &m = Metrics();
    Color bg = dark ? o.darkBg : o.lightBg;
    Color fg = dark ? o.darkText : o.lightText;
    for (auto &p : Layout(r)) {
        float a = items[(size_t)p.first].alpha;
        if (a <= 0.01f) continue;
        RectF pr = p.second;
        c.PushOpacity(a, pr.Inflate(14));
        c.Shadow(pr, m.radius, 8, 2, Color{0, 0, 0, 0.25f});
        c.FillRound(pr, m.radius, bg);
        c.Text(items[(size_t)p.first].text, pr, Ts(m.fontSize, kMedium, fg, HAlign::Center));
        c.PopLayer();
    }
}

// ---------------------------------------------------------------- GIF 进度

SizeF ProgressSize() { return SizeF{260, 52}; }

void DrawProgress(Canvas &c, RectF r, const std::wstring &text, double progress) {
    const auto &cap = theme::Cap();
    if (progress < 0) progress = 0;
    if (progress > 1) progress = 1;
    c.FillRound(r, 12, cap.tipBg);
    Color white = cap.tipText;
    TextStyle ts = Ts(13, kRegular, white);
    c.Text(text, RectF(r.x + 16, r.y + 8, r.w - 80, 20), ts);
    std::wstring pct = i18n::T("common.percent", {{"n", FormatW(L"%d", (int)std::floor(progress * 100))}});
    TextStyle ps = Ts(13, kRegular, WithAlpha(white, 0.8f), HAlign::Right);
    ps.tabular = true;
    c.Text(pct, RectF(r.r() - 64, r.y + 8, 48, 20), ps);
    RectF track(r.x + 16, r.b() - 16, r.w - 32, 4);
    c.FillRound(track, 2, Color{1, 1, 1, 0.22f});
    if (progress > 0) c.FillRound(RectF(track.x, track.y, track.w * (float)progress, track.h), 2, U().accent);
}

}  // namespace rec
}  // namespace ls
