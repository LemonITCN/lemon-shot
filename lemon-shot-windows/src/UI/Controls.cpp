#include "UI/Controls.h"

#include <cmath>

namespace ls {
namespace ui {

static const Tokens &K() { return theme::Tok(); }
static const UiColors &U() { return theme::Ui(); }

TextStyle BodyText(Color c, HAlign h) {
    TextStyle st;
    st.size = K().fontSize.body;
    st.color = c;
    st.h = h;
    st.v = VAlign::Center;
    return st;
}

void DrawSwitch(Canvas &c, RectF r, bool on, CtrlState st) {
    const auto &u = U();
    float h = r.h;
    Color track = on ? (st.hover ? u.accentHover : u.accent) : u.controlTrack;
    if (st.disabled) track = WithAlpha(track, 0.5f);
    c.FillRound(r, h / 2, track);
    float d = h - 4;
    float kx = on ? r.r() - 2 - d : r.x + 2;
    c.FillEllipse(kx + d / 2, r.y + 2 + d / 2 + 0.5f, d / 2, d / 2, Color{0, 0, 0, 0.12f});
    c.FillEllipse(kx + d / 2, r.y + 2 + d / 2, d / 2, d / 2, u.controlKnob);
}

static const float kSegPad = 12.f;

float SegmentedWidth(const std::vector<std::wstring> &labels) {
    float w = 4;
    for (auto &l : labels) w += gfx::Measure(l, K().fontSize.body, kRegular).w + kSegPad * 2;
    return w;
}

static std::vector<RectF> SegmentRects(RectF r, const std::vector<std::wstring> &labels) {
    std::vector<RectF> out;
    float total = 0;
    std::vector<float> ws;
    for (auto &l : labels) {
        float w = gfx::Measure(l, K().fontSize.body, kRegular).w + kSegPad * 2;
        ws.push_back(w);
        total += w;
    }
    float inner = r.w - 4;
    float k = total > 0 ? inner / total : 1;
    float x = r.x + 2;
    for (float w : ws) {
        out.push_back(RectF(x, r.y + 2, w * k, r.h - 4));
        x += w * k;
    }
    return out;
}

void DrawSegmented(Canvas &c, RectF r, const std::vector<std::wstring> &labels, int selected, int hover, CtrlState st) {
    const auto &u = U();
    float rad = K().window.controlRadius;
    c.FillRound(r, rad, u.controlTrack);
    auto rects = SegmentRects(r, labels);
    for (size_t i = 0; i < rects.size(); ++i) {
        RectF s = rects[i];
        if ((int)i == selected) {
            c.FillRound(s.Offset(0, 0.5f), rad - 2, Color{0, 0, 0, 0.08f});
            c.FillRound(s, rad - 2, theme::IsDark() ? u.surfacePressed : u.surface);
        } else if ((int)i == hover) {
            c.FillRound(s, rad - 2, WithAlpha(u.surface, 0.5f));
        }
        Color tc = (int)i == selected ? u.text : u.textSecondary;
        if (st.disabled) tc = u.textTertiary;
        c.Text(labels[i], s, BodyText(tc, HAlign::Center));
    }
}

int SegmentedHit(RectF r, const std::vector<std::wstring> &labels, float x, float y) {
    if (!r.Contains(x, y)) return -1;
    auto rects = SegmentRects(r, labels);
    for (size_t i = 0; i < rects.size(); ++i)
        if (x >= rects[i].x && x < rects[i].r()) return (int)i;
    return -1;
}

void DrawInputFrame(Canvas &c, RectF r, CtrlState st) {
    const auto &u = U();
    float rad = K().window.controlRadius;
    c.FillRound(r, rad, u.inputBg);
    if (st.focused) {
        c.StrokeRound(r.Inflate(1.5f), rad + 1.5f, u.focusRing, 3);
        c.StrokeRound(r.Inflate(-0.5f), rad - 0.5f, u.accent, 1);
    } else {
        c.StrokeRound(r.Inflate(-0.5f), rad - 0.5f, st.hover ? u.textTertiary : u.inputBorder, 1);
    }
}

void DrawDropdown(Canvas &c, RectF r, const std::wstring &text, CtrlState st) {
    const auto &u = U();
    DrawInputFrame(c, r, st);
    TextStyle ts = BodyText(st.disabled ? u.textTertiary : u.text);
    ts.ellipsis = true;
    c.Text(text, RectF(r.x + 10, r.y, r.w - 34, r.h), ts);
    c.Icon("chevron-down", r.r() - 16, r.cy(), 14, u.textSecondary, 1.75f);
}

float DropdownWidth(const std::vector<std::wstring> &options) {
    float w = 0;
    for (auto &o : options) {
        float t = gfx::Measure(o, K().fontSize.body, kRegular).w;
        if (t > w) w = t;
    }
    w += 44;
    return w < 120 ? 120 : w;
}

static const float kKnob = 16.f;

void DrawSlider(Canvas &c, RectF r, float t, CtrlState st) {
    const auto &u = U();
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float x0 = r.x + kKnob / 2, x1 = r.r() - kKnob / 2;
    float cy = r.cy();
    c.FillRound(RectF(x0, cy - 2, x1 - x0, 4), 2, u.controlTrack);
    float kx = x0 + (x1 - x0) * t;
    c.FillRound(RectF(x0, cy - 2, kx - x0, 4), 2, st.disabled ? u.textTertiary : u.accent);
    c.FillEllipse(kx, cy + 0.5f, kKnob / 2 + 0.5f, kKnob / 2 + 0.5f, Color{0, 0, 0, 0.15f});
    c.FillEllipse(kx, cy, kKnob / 2, kKnob / 2, u.controlKnob);
    if (st.pressed || st.focused) c.StrokeEllipse(kx, cy, kKnob / 2 + 1.5f, kKnob / 2 + 1.5f, u.focusRing, 3);
}

float SliderValueAt(RectF r, float x) {
    float x0 = r.x + kKnob / 2, x1 = r.r() - kKnob / 2;
    if (x1 <= x0) return 0;
    float t = (x - x0) / (x1 - x0);
    return t < 0 ? 0 : (t > 1 ? 1 : t);
}

float StepperWidth() { return 104.f; }

void DrawStepper(Canvas &c, RectF r, const std::wstring &value, int hoverPart, CtrlState st) {
    const auto &u = U();
    DrawInputFrame(c, r, st);
    float bw = r.h;
    RectF minus(r.x, r.y, bw, r.h), plus(r.r() - bw, r.y, bw, r.h);
    float rad = K().window.controlRadius;
    if (hoverPart == 1) c.FillRound(minus.Inflate(-3), rad - 2, u.surfaceHover);
    if (hoverPart == 2) c.FillRound(plus.Inflate(-3), rad - 2, u.surfaceHover);
    c.Line(minus.r(), r.y + 6, minus.r(), r.b() - 6, u.divider, 1);
    c.Line(plus.x, r.y + 6, plus.x, r.b() - 6, u.divider, 1);
    c.Icon("minus", minus.cx(), minus.cy(), 14, u.icon, 1.75f);
    c.Icon("plus", plus.cx(), plus.cy(), 14, u.icon, 1.75f);
    TextStyle ts = BodyText(u.text, HAlign::Center);
    ts.tabular = true;
    c.Text(value, RectF(minus.r(), r.y, plus.x - minus.r(), r.h), ts);
}

int StepperHit(RectF r, float x, float y) {
    if (!r.Contains(x, y)) return 0;
    if (x < r.x + r.h) return 1;
    if (x >= r.r() - r.h) return 2;
    return 0;
}

// 按钮同 Mac ButtonW：中粗正文字，普通按钮 inputBorder 边；带图标时宽度多 22（16pt 图标 + 6 间距），图标与文字整体居中
float ButtonWidth(const std::wstring &text, const char *icon) {
    float w = gfx::Measure(text, K().fontSize.body, kMedium).w + K().window.buttonPaddingX * 2;
    if (icon) w += 22;
    return std::ceil(w);
}

void DrawButton(Canvas &c, RectF r, const std::wstring &text, bool primary, CtrlState st, const char *icon) {
    const auto &u = U();
    float rad = K().window.controlRadius;
    Color tc;
    if (primary) {
        Color bg = st.hover || st.pressed ? u.accentHover : u.accent;
        if (st.disabled) bg = WithAlpha(bg, 0.5f);
        c.FillRound(r, rad, bg);
        tc = u.accentText;
    } else {
        Color bg = st.pressed ? u.surfacePressed : (st.hover ? u.surfaceHover : u.surface);
        c.FillRound(r, rad, bg);
        c.StrokeRound(r.Inflate(-0.5f), rad - 0.5f, u.inputBorder, 1);
        tc = st.disabled ? u.textTertiary : u.text;
    }
    float tw = gfx::Measure(text, K().fontSize.body, kMedium).w;
    float x = r.cx() - tw / 2;
    if (icon) {
        x = r.cx() - (tw + 22) / 2;
        c.Icon(icon, x + 8, r.cy(), 16, tc, 1.6f);
        x += 22;
    }
    TextStyle ts = BodyText(tc);
    ts.weight = kMedium;
    c.Text(text, RectF(x, r.y, tw + 2, r.h), ts);
}

void DrawIconButton(Canvas &c, RectF r, const char *icon, float iconSize, bool selected, CtrlState st) {
    const auto &u = U();
    float rad = K().toolbar.buttonRadius;
    if (selected)
        c.FillRound(r, rad, u.surfaceSelected);
    else if (st.pressed)
        c.FillRound(r, rad, u.surfacePressed);
    else if (st.hover)
        c.FillRound(r, rad, u.surfaceHover);
    Color ic = selected ? u.iconActive : (st.disabled ? u.textTertiary : u.icon);
    c.Icon(icon, r.cx(), r.cy(), iconSize, ic);
}

void DrawSwatch(Canvas &c, float cx, float cy, float d, Color color, bool selected, bool hover) {
    const auto &u = U();
    if (selected) c.StrokeEllipse(cx, cy, d / 2 + 2.5f, d / 2 + 2.5f, u.accent, 2);
    c.FillEllipse(cx, cy, d / 2, d / 2, color);
    // 浅色色块在浅底上看不清：加一圈细边
    c.StrokeEllipse(cx, cy, d / 2 - 0.5f, d / 2 - 0.5f, Color{0, 0, 0, 0.15f}, 1);
    if (hover && !selected) c.StrokeEllipse(cx, cy, d / 2 + 2.5f, d / 2 + 2.5f, u.border, 1.5f);
}

void DrawInfoIcon(Canvas &c, float cx, float cy, float size, CtrlState st) {
    const auto &u = U();
    c.Icon("info", cx, cy, size, st.hover ? u.accent : u.textSecondary, 1.75f);
}

RectF MeasureBubble(const std::wstring &text, float maxWidth) {
    // 同 Mac BubbleWidget(.tooltip)：左右 8、上下 5，宽高向上取整
    float pad = K().capture.tipPaddingX;
    float size = K().fontSize.caption + 1;
    gfx::TextSize ts = gfx::Measure(text, size, kRegular, false, 100000.f);
    if (ts.w > maxWidth - pad * 2) ts = gfx::Measure(text, size, kRegular, false, maxWidth - pad * 2);
    return RectF(0, 0, std::ceil(ts.w + pad * 2), std::ceil(ts.h + 10));
}

RectF MeasureTip(const std::wstring &text) {
    // 同 Mac CaptureRender.tooltipSize：单行，宽 ceil(文字宽 + 16)，高固定 tipHeight（24）
    float pad = K().capture.tipPaddingX;
    float w = gfx::Measure(text, K().fontSize.caption + 1, kRegular).w;
    return RectF(0, 0, std::ceil(w + pad * 2), K().capture.tipHeight);
}

void DrawBubble(Canvas &c, RectF r, const std::wstring &text) {
    const auto &cap = theme::Cap();
    float pad = K().capture.tipPaddingX;
    c.FillRound(r, K().capture.tipRadius, cap.tipBg);
    TextStyle ts;
    ts.size = K().fontSize.caption + 1;
    ts.color = cap.tipText;
    ts.wrap = true;
    ts.v = VAlign::Center;
    c.Text(text, RectF(r.x + pad, r.y, r.w - pad * 2 + 1, r.h), ts);
}

void DrawPanel(Canvas &c, RectF r, float radius) {
    const auto &u = U();
    c.Shadow(r, radius, K().toolbar.shadowBlur, K().toolbar.shadowOffsetY, u.shadow);
    c.FillRound(r, radius, u.popoverBg);
    c.StrokeRound(r.Inflate(-0.5f), radius - 0.5f, u.toolbarBorder, 1);
}

}  // namespace ui
}  // namespace ls
