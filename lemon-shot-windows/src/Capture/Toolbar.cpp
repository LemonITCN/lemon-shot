#include "Capture/Toolbar.h"

#include "App/Hotkeys.h"
#include "Capture/StyleSettings.h"
#include "Settings/I18n.h"
#include "UI/Controls.h"

namespace ls {

namespace {

// 组：标注 | 编辑 | 功能 | 输出
const int kGroupAnnotate[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
const int kGroupEdit[] = {TB_UNDO, TB_REDO};
const int kGroupFeature[] = {TB_LONGSHOT, TB_RECORD, TB_OCR, TB_TRANSLATE, TB_PIN};
const int kGroupOutput[] = {TB_SAVE, TB_CANCEL, TB_DONE};

struct Group {
    const int *ids;
    int count;
};

std::vector<Group> Groups(bool pinMode) {
    std::vector<Group> g;
    g.push_back({kGroupAnnotate, 10});
    g.push_back({kGroupEdit, 2});
    if (!pinMode) g.push_back({kGroupFeature, 5});
    g.push_back({kGroupOutput, 3});
    return g;
}

}  // namespace

RectF ToolbarLayout::ButtonRect(int id) const {
    for (auto &b : btns)
        if (b.id == id) return b.r;
    return RectF();
}

float ToolbarHeight() { return theme::Tok().toolbar.height; }

float ToolbarWidth(bool pinMode) {
    const auto &t = theme::Tok().toolbar;
    auto groups = Groups(pinMode);
    float w = t.paddingX * 2;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        w += groups[gi].count * t.button + (groups[gi].count - 1) * t.spacing;
        if (gi + 1 < groups.size()) w += t.separatorMargin * 2 + t.separatorWidth;
    }
    return w;
}

ToolbarLayout LayoutToolbar(float x, float y, bool pinMode) {
    const auto &t = theme::Tok().toolbar;
    ToolbarLayout lay;
    lay.rect = RectF(x, y, ToolbarWidth(pinMode), t.height);
    auto groups = Groups(pinMode);
    float cx = x + t.paddingX;
    float by = y + (t.height - t.button) / 2;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        for (int i = 0; i < groups[gi].count; ++i) {
            lay.btns.push_back({groups[gi].ids[i], RectF(cx, by, t.button, t.button)});
            cx += t.button;
            if (i + 1 < groups[gi].count) cx += t.spacing;
        }
        if (gi + 1 < groups.size()) {
            cx += t.separatorMargin;
            lay.seps.push_back(RectF(cx, y + (t.height - t.separatorHeight) / 2, t.separatorWidth, t.separatorHeight));
            cx += t.separatorWidth + t.separatorMargin;
        }
    }
    return lay;
}

const char *ToolbarIcon(int id) {
    if (id >= 1 && id <= 10) return style::ToolIcon(id);
    switch (id) {
    case TB_UNDO: return "undo-2";
    case TB_REDO: return "redo-2";
    case TB_LONGSHOT: return "scroll-text";
    case TB_RECORD: return "video";
    case TB_OCR: return "scan-text";
    case TB_TRANSLATE: return "languages";
    case TB_PIN: return "pin";
    case TB_SAVE: return "download";
    case TB_CANCEL: return "x";
    case TB_DONE: return "check";
    }
    return "";
}

std::wstring ToolbarTooltip(int id) {
    static const char *toolKeys[] = {"",          "toolRect", "toolEllipse", "toolLine",      "toolArrow",  "toolPen",
                                     "toolMosaic", "toolText", "toolStep",    "toolHighlight", "toolMagnify"};
    std::wstring name, key;
    if (id >= 1 && id <= 10) {
        name = T(style::ToolNameKey(id));
        key = hotkeys::CaptureKeyDisplay(toolKeys[id]);
    } else {
        switch (id) {
        case TB_UNDO: name = T("action.undo"); key = L"Ctrl+Z"; break;
        case TB_REDO: name = T("action.redo"); key = L"Ctrl+Y"; break;
        case TB_LONGSHOT: name = T("action.longshot"); key = hotkeys::CaptureKeyDisplay("longshot"); break;
        case TB_RECORD: name = T("action.record"); key = hotkeys::CaptureKeyDisplay("record"); break;
        case TB_OCR: name = T("action.ocr"); key = hotkeys::CaptureKeyDisplay("ocr"); break;
        case TB_TRANSLATE: name = T("action.translate"); key = hotkeys::CaptureKeyDisplay("translate"); break;
        case TB_PIN: name = T("action.pin"); key = hotkeys::CaptureKeyDisplay("pin"); break;
        case TB_SAVE: name = T("action.save"); key = L"Ctrl+S"; break;
        case TB_CANCEL: name = T("action.cancel"); key = L"Esc"; break;
        case TB_DONE: name = T("action.done"); key = L"Enter"; break;
        }
    }
    if (key.empty()) return name;
    return name + L"　" + key;
}

bool ToolbarEnabled(int id, const ls_session_state &st) {
    bool multi = st.window_count > 0;
    if (id >= 1 && id <= 10) return !multi;
    if (id == TB_LONGSHOT || id == TB_RECORD) return !multi;
    if (id == TB_UNDO) return st.can_undo != 0;
    if (id == TB_REDO) return st.can_redo != 0;
    return true;
}

void DrawToolbar(Canvas &c, const ToolbarLayout &lay, const ls_session_state &st, int hover, int pressed) {
    const auto &t = theme::Tok().toolbar;
    const auto &u = theme::Ui();
    c.Shadow(lay.rect, t.radius, t.shadowBlur, t.shadowOffsetY, u.shadow);
    c.FillRound(lay.rect, t.radius, u.toolbarBg);
    c.StrokeRound(lay.rect.Inflate(-0.5f), t.radius - 0.5f, u.toolbarBorder, 1);
    for (auto &s : lay.seps) c.FillRect(s, u.divider);
    for (auto &b : lay.btns) {
        bool enabled = ToolbarEnabled(b.id, st);
        bool current = b.id >= 1 && b.id <= 10 && st.tool == b.id;
        if (current)
            c.FillRound(b.r, t.buttonRadius, u.surfaceSelected);
        else if (enabled && pressed == b.id)
            c.FillRound(b.r, t.buttonRadius, u.surfacePressed);
        else if (enabled && hover == b.id)
            c.FillRound(b.r, t.buttonRadius, u.surfaceHover);
        Color ic = u.icon;
        if (!enabled) ic = u.textTertiary;
        else if (current) ic = u.iconActive;
        else if (b.id == TB_DONE) ic = u.accent;
        c.Icon(ToolbarIcon(b.id), b.r.cx(), b.r.cy(), t.iconSize, ic);
    }
}

int HitToolbar(const ToolbarLayout &lay, float x, float y) {
    if (!lay.rect.Contains(x, y)) return TB_NONE;
    for (auto &b : lay.btns)
        if (b.r.Inflate(theme::Tok().toolbar.spacing / 2, 4).Contains(x, y)) return b.id;
    return TB_NONE;
}

RectF ToolbarTooltipRect(RectF btn, const std::wstring &text, bool below, float screenW, float screenH) {
    RectF sz = ui::MeasureBubble(text, 320);
    float gap = theme::Tok().capture.tipGap;
    float x = btn.cx() - sz.w / 2;
    float y = below ? btn.b() + gap + 4 : btn.y - gap - 4 - sz.h;
    if (below && y + sz.h > screenH) y = btn.y - gap - 4 - sz.h;
    if (!below && y < 0) y = btn.b() + gap + 4;
    if (x + sz.w > screenW - 4) x = screenW - 4 - sz.w;
    if (x < 4) x = 4;
    return RectF(x, y, sz.w, sz.h);
}

void DrawToolbarTooltip(Canvas &c, RectF btn, const std::wstring &text, bool below, float screenW, float screenH) {
    if (text.empty()) return;
    ui::DrawBubble(c, ToolbarTooltipRect(btn, text, below, screenW, screenH), text);
}

}  // namespace ls
