#include "Capture/StylePopover.h"

#include <cmath>
#include <cstring>

#include "Capture/Annotations.h"
#include "Capture/StyleSettings.h"
#include "Settings/I18n.h"
#include "UI/Controls.h"
#include "Util/Util.h"

namespace ls {

namespace {

const float kZooms[4] = {1.5f, 2.f, 3.f, 4.f};
const float kDotRadius[3] = {2.f, 3.f, 5.f};  // 粗细三档的圆点（半径）
const char *kMosaicIcons[4] = {"grid-3x3", "brush", "droplet", "blend"};
const char *kPenIcons[2] = {"pencil", "highlighter"};
const float kGap = 4.f, kLabelGap = 6.f, kSepW = 17.f, kValueW = 48.f, kStepperW = 92.f, kIconSize = 18.f;
const float kCornerOn = 6.f;

float LabelSize() { return theme::Tok().fontSize.caption + 1; }
float SliderW() { return theme::Tok().popover.sliderWidth; }

PopItem MakeItem(PopKind k, PopField f, RectF r, int index = 0) {
    PopItem it;
    std::memset(&it, 0, sizeof it);
    it.kind = k;
    it.field = f;
    it.r = r;
    it.index = index;
    return it;
}

// 一行控件，从左到右排；null 分隔用 Sep()
struct Row {
    std::vector<PopItem> items;
    float x0, y, h;
    float x;
    bool first = true;
    // false = 偏好设置「标注」页：同 Mac FormPage 的行排版，控件之间一律 8pt，分隔换成 10pt 留白（前后各 8pt）
    bool lines = true;

    Row(float x0_, float y_, float h_) : x0(x0_), y(y_), h(h_), x(x0_) {}

    void Advance(bool isLabel) {
        if (!first) x += !lines ? 8.f : (isLabel ? kLabelGap : kGap);
        first = false;
    }
    PopItem &Add(PopKind k, PopField f, float w, int index = 0) {
        Advance(k == PK_LABEL);
        items.push_back(MakeItem(k, f, RectF(x, y, w, h), index));
        x += w;
        return items.back();
    }
    void Sep() {
        if (!lines) {
            if (!first) x += 8.f + 10.f;
            return;
        }
        items.push_back(MakeItem(PK_SEP, PF_NONE, RectF(x + 8, y + (h - 16) / 2, 1, 16)));
        x += kSepW;
        first = true;
    }
    void Label(const char *key) {
        float w = std::ceil(gfx::Measure(T(key), LabelSize(), kRegular).w) + 2;
        Add(PK_LABEL, PF_NONE, w).label = key;
    }
    void Slider(PopField f, float mn, float mx) {
        PopItem &it = Add(PK_SLIDER, f, SliderW());
        // 数值带单位（如「18 pt」）：数值列放宽，轨道相应缩短，总宽不变
        it.track = RectF(it.r.x, it.r.y, SliderW() - 50, h);
        it.min = mn;
        it.max = mx;
    }
    void Stepper(PopField f, float mn, float mx) {
        PopItem &it = Add(PK_STEPPER, f, kStepperW);
        it.track = RectF(it.r.x, it.r.y + 2, kStepperW, h - 4);
        it.min = mn;
        it.max = mx;
    }
    void Chip(PopField f, const std::wstring &text, int index, const char *key) {
        float w = std::ceil(gfx::Measure(text, LabelSize(), kRegular).w + 16);
        if (w < h) w = h;
        PopItem &it = Add(PK_CHIP, f, w, index);
        it.label = key;
    }
    void Dots() {
        for (int i = 0; i < 3; ++i) Add(PK_DOT, PF_LINEW, h, i);
    }
    float Width() const { return x - x0; }
};

float FieldValue(PopField f, const ls_style &st) {
    switch (f) {
    case PF_BLOCK: return st.block_size;
    case PF_BRUSH: return st.line_width;
    case PF_FONT: return st.font_size;
    case PF_STEPSIZE: return st.line_width;
    case PF_OPACITY: return st.opacity;
    case PF_NEXT: return (float)st.next_number;
    default: return 0;
    }
}

void SetField(PopField f, ls_style &st, float v) {
    switch (f) {
    case PF_BLOCK: st.block_size = v; break;
    case PF_BRUSH: st.line_width = v; break;
    case PF_FONT: st.font_size = v; break;
    case PF_STEPSIZE: st.line_width = v; break;
    case PF_OPACITY: st.opacity = v; break;
    case PF_NEXT: st.next_number = (int32_t)v; break;
    default: break;
    }
}

bool IsOn(const PopItem &it, const ls_style &st) {
    switch (it.field) {
    case PF_LINEW: return style::NearestLevel(st.line_width) == it.index;
    case PF_FILLED: return st.filled != 0;
    case PF_CORNER: return st.corner_radius > 0;
    case PF_DASHED: return st.variant == 1;
    case PF_BOLD: return st.bold != 0;
    case PF_VARIANT: return st.variant == it.index;
    case PF_ZOOM: {
        // 滚轮按 0.5 调（1.5–6）：不在档位上时亮最近的一档
        int best = 0;
        for (int i = 1; i < 4; ++i)
            if (std::fabs(kZooms[i] - st.zoom) < std::fabs(kZooms[best] - st.zoom)) best = i;
        return best == it.index;
    }
    default: return false;
    }
}

std::wstring ValueText(PopField f, float v) {
    if (f == PF_OPACITY) return FormatW(L"%d%%", (int)std::floor(v * 100.f + 0.5f));
    // 块大小、笔刷、字号、序号大小都是逻辑点
    return i18n::T("common.points", {{"n", FormatW(L"%d", (int)std::floor(v + 0.5f))}});
}

std::wstring ZoomText(int i) { return NumberText(kZooms[i], 1) + L"×"; }

// 用内核几何画的小预览（箭头、序号）：临时会话里真的画一个，缓存会话
struct Preview {
    int tool, variant;
    ls_color color;
    float size, lineWidth;
    ls_image base;
    ls_session *session;
};
std::vector<Preview> g_previews;

ls_session *PreviewSession(int tool, int variant, ls_color color, float size, float lineWidth) {
    for (auto &p : g_previews)
        if (p.tool == tool && p.variant == variant && std::memcmp(&p.color, &color, sizeof color) == 0 && p.size == size &&
            p.lineWidth == lineWidth)
            return p.session;
    Preview p;
    p.tool = tool;
    p.variant = variant;
    p.color = color;
    p.size = size;
    p.lineWidth = lineWidth;
    p.base = ls_image_alloc((int)size, (int)size);
    p.session = nullptr;
    if (!p.base.data) return nullptr;
    ls_session_config cfg = ls_session_default_config();
    cfg.width = size;
    cfg.height = size;
    cfg.scale = 1;
    cfg.base = p.base;
    cfg.detect_windows = 0;
    cfg.lock_selection = 1;
    cfg.has_initial_selection = 1;
    cfg.initial_selection = ls_rect{0, 0, size, size};
    ls_session *s = ls_session_create(&cfg);
    if (!s) {
        ls_image_free(&p.base);
        return nullptr;
    }
    ls_style st;
    ls_session_get_style(s, tool, &st);
    st.color = color;
    st.variant = variant;
    st.opacity = 1;
    st.line_width = lineWidth;
    st.next_number = 1;
    ls_session_set_style(s, tool, &st);
    ls_session_set_tool(s, tool);
    float m = size * 0.2f;
    if (tool == LS_TOOL_STEP) {
        ls_session_mouse_down(s, size / 2, size / 2, LS_BUTTON_LEFT, 0, 1);
        ls_session_mouse_up(s, size / 2, size / 2, LS_BUTTON_LEFT, 0);
    } else {
        ls_session_mouse_down(s, m, size - m, LS_BUTTON_LEFT, 0, 1);
        ls_session_mouse_move(s, size / 2, size / 2, 0);
        ls_session_mouse_move(s, size - m, m, 0);
        ls_session_mouse_up(s, size - m, m, LS_BUTTON_LEFT, 0);
    }
    p.session = s;
    g_previews.push_back(p);
    return s;
}

bool DrawKernelPreview(Canvas &c, int tool, int variant, RectF r, Color col, float lineWidth) {
    float size = r.h;
    ls_session *s = PreviewSession(tool, variant, style::ToLs(col), size, lineWidth);
    if (!s || ls_session_shape_count(s) <= 0) return false;
    D2D1_MATRIX_3X2_F old = c.GetTransform();
    c.SetTransform(MatMul(MatTranslate(r.cx() - size / 2, r.y), old));
    AnnotationContext ctx;
    int n = ls_session_shape_count(s);
    for (int i = 0; i < n; ++i) {
        ls_shape_view v;
        std::memset(&v, 0, sizeof v);
        ls_session_shape_at(s, i, &v);
        v.selected = 0;
        DrawShape(c, v, ctx);
    }
    c.SetTransform(old);
    return true;
}

void DrawVariant(Canvas &c, int tool, int v, RectF r, Color fg) {
    const auto &u = theme::Ui();
    switch (tool) {
    case LS_TOOL_ARROW:
        if (!DrawKernelPreview(c, tool, v, r, fg, 2))
            c.Icon(v == 3 ? "spline" : "move-up-right", r.cx(), r.cy(), kIconSize, fg);
        break;
    case LS_TOOL_STEP:
        if (!DrawKernelPreview(c, tool, v, r, fg, 18)) c.Icon("list-ordered", r.cx(), r.cy(), kIconSize, fg);
        break;
    case LS_TOOL_TEXT: {
        RectF ir(r.cx() - 9, r.cy() - 9, 18, 18);
        if (v == 0) {
            c.Icon("type", ir.cx(), ir.cy(), 18, fg);
        } else if (v == 1) {
            c.Icon("type-outline", ir.cx(), ir.cy(), 18, fg);
        } else {
            c.FillRound(ir.Inflate(1), 4, fg);
            c.Icon("type", ir.cx(), ir.cy(), 14, u.surface);
        }
        break;
    }
    case LS_TOOL_PEN: c.Icon(kPenIcons[v], r.cx(), r.cy(), kIconSize, fg); break;
    case LS_TOOL_MOSAIC: c.Icon(kMosaicIcons[v], r.cx(), r.cy(), kIconSize, fg); break;
    default: break;
    }
}

}  // namespace

PopLayout LayoutPopover(int tool, const ls_style &st, bool full) {
    const auto &p = theme::Tok().popover;
    float pad = full ? p.padding : 0;
    float rh = p.rowHeight;
    Row r1(pad, pad, rh);
    Row r2(pad, pad + rh + p.gap, rh);
    r1.lines = r2.lines = full;
    bool row2Extra = false;
    switch (tool) {
    case LS_TOOL_RECT:
    case LS_TOOL_ELLIPSE:
        r1.Dots();
        r1.Sep();
        r1.Add(PK_TOGGLE, PF_FILLED, rh).icon = tool == LS_TOOL_RECT ? "square" : "circle";
        if (tool == LS_TOOL_RECT) r1.Chip(PF_CORNER, T("style.cornerRadius"), 0, "style.cornerRadius");
        break;
    case LS_TOOL_LINE:
        r1.Dots();
        r1.Sep();
        r1.Add(PK_DASH, PF_DASHED, rh);
        break;
    case LS_TOOL_ARROW:
        for (int i = 0; i < 4; ++i) r1.Add(PK_VARIANT, PF_VARIANT, rh, i);
        r1.Sep();
        r1.Dots();
        break;
    case LS_TOOL_PEN:
        for (int i = 0; i < 2; ++i) r1.Add(PK_VARIANT, PF_VARIANT, rh, i);
        r1.Sep();
        r1.Dots();
        break;
    case LS_TOOL_MOSAIC:
        for (int i = 0; i < 4; ++i) r1.Add(PK_VARIANT, PF_VARIANT, rh, i);
        // 马赛克没有颜色：滑块放第 2 行
        r2.Label("style.blockSize");
        r2.Slider(PF_BLOCK, 2, 60);
        if (st.variant == 1 || st.variant == 3) {
            r2.Sep();
            r2.Label("style.brushSize");
            r2.Slider(PF_BRUSH, 4, 120);
        }
        row2Extra = true;
        break;
    case LS_TOOL_TEXT:
        for (int i = 0; i < 3; ++i) r1.Add(PK_VARIANT, PF_VARIANT, rh, i);
        r1.Sep();
        r1.Label("style.fontSize");
        r1.Slider(PF_FONT, 8, 200);
        r1.Sep();
        r1.Add(PK_TOGGLE, PF_BOLD, rh).icon = "bold";
        break;
    case LS_TOOL_STEP:
        for (int i = 0; i < 3; ++i) r1.Add(PK_VARIANT, PF_VARIANT, rh, i);
        r1.Sep();
        r1.Label("style.size");
        r1.Slider(PF_STEPSIZE, 16, 64);
        if (full) {
            r1.Sep();
            r1.Label("style.next");
            r1.Stepper(PF_NEXT, 1, 999);
        }
        break;
    case LS_TOOL_HIGHLIGHT:
        r1.Label("style.opacity");
        r1.Slider(PF_OPACITY, 0.1f, 0.95f);
        r1.Sep();
        r1.Chip(PF_CORNER, T("style.cornerRadius"), 0, "style.cornerRadius");
        break;
    case LS_TOOL_MAGNIFY:
        for (int i = 0; i < 4; ++i) r1.Chip(PF_ZOOM, ZoomText(i), i, nullptr);
        r1.Sep();
        r1.Dots();
        break;
    default: break;
    }
    PopLayout lay;
    lay.tool = tool;
    lay.items = r1.items;
    float w = r1.Width();
    if (full) w += 8 + 18;
    bool palette = full && tool != LS_TOOL_HIGHLIGHT && tool != LS_TOOL_MOSAIC;
    bool hasRow2 = palette || row2Extra;
    if (row2Extra) {
        for (auto &it : r2.items) lay.items.push_back(it);
        // 宽度按全部滑块可见时计算，切换样式时面板不跳动
        float all = 0;
        if (tool == LS_TOOL_MOSAIC) {
            float l1 = std::ceil(gfx::Measure(T("style.blockSize"), LabelSize(), kRegular).w) + 2;
            float l2 = std::ceil(gfx::Measure(T("style.brushSize"), LabelSize(), kRegular).w) + 2;
            all = l1 + kLabelGap + SliderW() + kSepW + l2 + kLabelGap + SliderW();
        }
        if (all > w) w = all;
    }
    if (palette) {
        float sw = p.swatch, g = p.swatchGap;
        float x = pad;
        float y2 = pad + rh + p.gap;
        for (int i = 0; i < 11; ++i) {
            PopItem it = MakeItem(i < 10 ? PK_SWATCH : PK_CUSTOM, PF_COLOR,
                                  RectF(x + i * (sw + g), y2 + (rh - sw) / 2, sw, sw), i);
            lay.items.push_back(it);
        }
        float pw = 11 * sw + 10 * g + 4;
        if (pw > w) w = pw;
    }
    if (full) {
        lay.w = std::ceil(w + 2 * pad);
        lay.h = std::ceil((hasRow2 ? rh * 2 + p.gap : rh) + 2 * pad);
        PopItem info = MakeItem(PK_INFO, PF_NONE, RectF(lay.w - pad - 18, pad + (rh - 18) / 2, 18, 18));
        lay.items.push_back(info);
    } else {
        lay.w = std::ceil(w);
        lay.h = hasRow2 ? rh * 2 + p.gap : rh;
    }
    return lay;
}

void DrawPopover(Canvas &c, const PopLayout &lay, float ox, float oy, const ls_style &st, int hover, bool panel) {
    const auto &u = theme::Ui();
    const auto &p = theme::Tok().popover;
    float rad = theme::Tok().toolbar.buttonRadius;
    if (panel) ui::DrawPanel(c, RectF(ox, oy, lay.w, lay.h), p.radius);
    std::vector<Color> pal = style::Palette();
    bool matched = false;
    for (auto &pc : pal) {
        ls_color a = style::ToLs(pc);
        if (a.r == st.color.r && a.g == st.color.g && a.b == st.color.b) matched = true;
    }
    for (size_t i = 0; i < lay.items.size(); ++i) {
        const PopItem &it = lay.items[i];
        RectF r = it.r.Offset(ox, oy);
        bool hv = (int)i == hover;
        auto buttonBg = [&](bool on) {
            if (on)
                c.FillRound(r, rad, u.surfaceSelected);
            else if (hv)
                c.FillRound(r, rad, u.surfaceHover);
        };
        switch (it.kind) {
        case PK_SEP: c.FillRect(r, u.divider); break;
        case PK_LABEL: {
            TextStyle ts;
            ts.size = LabelSize();
            ts.color = u.textSecondary;
            c.Text(T(it.label), r, ts);
            break;
        }
        case PK_DOT: {
            bool on = IsOn(it, st);
            buttonBg(on);
            float d = kDotRadius[it.index];
            c.FillEllipse(r.cx(), r.cy(), d, d, on ? u.iconActive : u.icon);
            break;
        }
        case PK_TOGGLE: {
            bool on = IsOn(it, st);
            buttonBg(on);
            Color fg = on ? u.iconActive : u.icon;
            c.Icon(it.icon, r.cx(), r.cy(), it.field == PF_FILLED ? 16 : kIconSize, fg, -1, it.field == PF_FILLED);
            break;
        }
        case PK_DASH: {
            bool on = IsOn(it, st);
            buttonBg(on);
            // 2pt 平头虚线，线段 4 间隔 3（逻辑点，同 Mac setLineDash [4, 3]）。D2D 的虚线长度按线宽倍数计，
            // 直接用 UiDashStroke（1pt 线的 4/3）会变成 8/6，这里逐段画
            float x0 = r.x + 6, y0 = r.b() - 6, x1 = r.r() - 6, y1 = r.y + 6;
            float len = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
            float ux = (x1 - x0) / len, uy = (y1 - y0) / len;
            for (float t = 0; t < len; t += 7.f) {
                float e = std::fmin(t + 4.f, len);
                c.Line(x0 + ux * t, y0 + uy * t, x0 + ux * e, y0 + uy * e, on ? u.iconActive : u.icon, 2);
            }
            break;
        }
        case PK_CHIP: {
            bool on = IsOn(it, st);
            buttonBg(on);
            TextStyle ts;
            ts.size = LabelSize();
            ts.color = on ? u.iconActive : u.icon;
            ts.h = HAlign::Center;
            c.Text(it.field == PF_ZOOM ? ZoomText(it.index) : T(it.label), r, ts);
            break;
        }
        case PK_VARIANT: {
            bool on = IsOn(it, st);
            buttonBg(on);
            DrawVariant(c, lay.tool, it.index, r, on ? u.iconActive : u.icon);
            break;
        }
        case PK_SLIDER: {
            float v = FieldValue(it.field, st);
            float t = it.max > it.min ? (v - it.min) / (it.max - it.min) : 0;
            ui::CtrlState cs;
            cs.hover = hv;
            ui::DrawSlider(c, it.track.Offset(ox, oy), t, cs);
            TextStyle ts;
            ts.size = LabelSize();
            ts.color = u.text;
            ts.tabular = true;
            ts.h = HAlign::Right;
            c.Text(ValueText(it.field, v), RectF(r.r() - kValueW, r.y, kValueW, r.h), ts);
            break;
        }
        case PK_STEPPER: {
            ui::CtrlState cs;
            ui::DrawStepper(c, it.track.Offset(ox, oy), FormatW(L"%d", st.next_number), 0, cs);
            break;
        }
        case PK_SWATCH: {
            Color col = pal[(size_t)it.index];
            ls_color a = style::ToLs(col);
            bool sel = a.r == st.color.r && a.g == st.color.g && a.b == st.color.b;
            ui::DrawSwatch(c, r.cx(), r.cy(), r.w, col, sel, hv);
            break;
        }
        case PK_CUSTOM: {
            if (!matched) {
                ui::DrawSwatch(c, r.cx(), r.cy(), r.w, style::FromLs(st.color), true, false);
            } else {
                if (hv) c.StrokeEllipse(r.cx(), r.cy(), r.w / 2 + 3, r.w / 2 + 3, u.border, 2);
                c.Icon("palette", r.cx(), r.cy(), r.w - 1, u.icon, 1.5f);
            }
            break;
        }
        case PK_INFO: {
            ui::CtrlState cs;
            cs.hover = hv;
            ui::DrawInfoIcon(c, r.cx(), r.cy(), 16, cs);
            break;
        }
        }
    }
}

int HitPopover(const PopLayout &lay, float x, float y) {
    for (size_t i = 0; i < lay.items.size(); ++i) {
        const PopItem &it = lay.items[i];
        switch (it.kind) {
        case PK_SEP:
        case PK_LABEL: continue;
        case PK_SLIDER:
            if (it.track.Inflate(4, 0).Contains(x, y)) return (int)i;
            continue;
        case PK_STEPPER:
            if (it.track.Contains(x, y)) return (int)i;
            continue;
        case PK_SWATCH:
        case PK_CUSTOM:
            if (it.r.Inflate(3).Contains(x, y)) return (int)i;
            continue;
        default:
            if (it.r.Contains(x, y)) return (int)i;
        }
    }
    return -1;
}

bool PopoverApply(const PopLayout &lay, int item, float x, float y, ls_style &st, bool dragging) {
    if (item < 0 || item >= (int)lay.items.size()) return false;
    const PopItem &it = lay.items[(size_t)item];
    ls_style before = st;
    if (dragging && it.kind != PK_SLIDER) return false;
    switch (it.kind) {
    case PK_DOT: st.line_width = style::kLineWidths[it.index]; break;
    case PK_TOGGLE:
        if (it.field == PF_FILLED) st.filled = st.filled ? 0 : 1;
        else if (it.field == PF_BOLD) st.bold = st.bold ? 0 : 1;
        break;
    case PK_DASH: st.variant = st.variant == 1 ? 0 : 1; break;
    case PK_CHIP:
        if (it.field == PF_CORNER) st.corner_radius = st.corner_radius > 0 ? 0 : kCornerOn;
        else if (it.field == PF_ZOOM) st.zoom = kZooms[it.index];
        break;
    case PK_VARIANT: st.variant = it.index; break;
    case PK_SLIDER: {
        float t = ui::SliderValueAt(it.track, x);
        float v = it.min + t * (it.max - it.min);
        if (it.field == PF_OPACITY)
            v = std::floor(v * 20.f + 0.5f) / 20.f;  // 0.05 一档
        else
            v = std::floor(v + 0.5f);
        SetField(it.field, st, v);
        break;
    }
    case PK_STEPPER: {
        int part = ui::StepperHit(it.track, x, y);
        int v = st.next_number + (part == 2 ? 1 : (part == 1 ? -1 : 0));
        st.next_number = Clamp(v, (int)it.min, (int)it.max);
        break;
    }
    case PK_SWATCH: {
        std::vector<Color> pal = style::Palette();
        st.color = style::ToLs(pal[(size_t)it.index]);
        break;
    }
    default: return false;
    }
    return std::memcmp(&before, &st, sizeof st) != 0;
}

void DrawStyleHint(Canvas &c, RectF popover, float screenW, float screenH) {
    const std::wstring &text = T("style.hint");
    RectF sz = ui::MeasureBubble(text, 300);
    float x = popover.r() - sz.w;
    float y = popover.b() + 6;
    if (y + sz.h > screenH - 4) y = popover.y - 6 - sz.h;
    if (x < 4) x = 4;
    if (x + sz.w > screenW - 4) x = screenW - 4 - sz.w;
    ui::DrawBubble(c, RectF(x, y, sz.w, sz.h), text);
}

}  // namespace ls
