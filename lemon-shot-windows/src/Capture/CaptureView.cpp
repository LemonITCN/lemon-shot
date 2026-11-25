#include "Capture/CaptureView.h"

#include <cmath>
#include <cstring>

#include "Capture/StyleSettings.h"
#include "Record/SetupPanel.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Controls.h"
#include "Util/Util.h"

namespace ls {

namespace {

const Tokens &K() { return theme::Tok(); }
const CaptureColors &Cap() { return theme::Cap(); }

float TipFontSize() { return K().fontSize.caption + 1; }

void FillOutside(Canvas &c, RectF bounds, RectF hole, Color col) {
    hole = hole.Intersect(bounds);
    if (hole.Empty()) {
        c.FillRect(bounds, col);
        return;
    }
    c.FillRect(RectF(bounds.x, bounds.y, bounds.w, hole.y - bounds.y), col);
    c.FillRect(RectF(bounds.x, hole.b(), bounds.w, bounds.b() - hole.b()), col);
    c.FillRect(RectF(bounds.x, hole.y, hole.x - bounds.x, hole.h), col);
    c.FillRect(RectF(hole.r(), hole.y, bounds.r() - hole.r(), hole.h), col);
}

// 描边画在矩形内侧（与 Mac 端 Draw.stroke 一致）
void StrokeInside(Canvas &c, RectF r, Color col, float w) { c.StrokeRect(r.Inflate(-w / 2), col, w); }

void DrawSelectionChrome(Canvas &c, RectF sel, bool handles) {
    float bw = K().capture.selectionBorderWidth;
    c.StrokeRect(sel.Inflate(bw / 2), Cap().selectionBorder, bw);
    if (!handles) return;
    float hs = K().capture.handleSize;
    float xs[3] = {sel.x, sel.cx(), sel.r()}, ys[3] = {sel.y, sel.cy(), sel.b()};
    for (int iy = 0; iy < 3; ++iy) {
        for (int ix = 0; ix < 3; ++ix) {
            if (ix == 1 && iy == 1) continue;
            RectF h(xs[ix] - hs / 2, ys[iy] - hs / 2, hs, hs);
            c.FillRect(h, Cap().handleFill);
            StrokeInside(c, h, Cap().handleBorder, 1);
        }
    }
}

void DrawMagnifier(Canvas &c, const CaptureFrame &f, const ls_session_state &st) {
    const auto &m = K().capture.magnifier;
    float size = m.size, pixels = m.pixels, cell = size / pixels;
    ls_rect pr = ls_session_place_magnifier(f.session, size, size + m.infoHeight, m.offset);
    RectF rect(pr);
    RectF top(rect.x, rect.y, size, size);
    RectF info(rect.x, rect.y + size, size, rect.h - size);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    D2D1_ROUNDED_RECT rr = {rect.D2(), m.radius, m.radius};
    gfx::D2D()->CreateRoundedRectangleGeometry(&rr, clip.Put());
    c.PushGeometryMask(clip.Get());
    c.FillRect(top, Color{0, 0, 0, 1});
    float px = std::floor(st.mouse_x * f.scale), py = std::floor(st.mouse_y * f.scale);
    float half = std::floor(pixels / 2);
    if (f.frozen && f.frozen->Valid()) {
        RectF src(px - half, py - half, pixels, pixels);
        RectF img(0, 0, (float)f.frozen->Width(), (float)f.frozen->Height());
        RectF in = src.Intersect(img);
        if (!in.Empty()) {
            RectF dst(top.x + (in.x - src.x) * cell, top.y + (in.y - src.y) * cell, in.w * cell, in.h * cell);
            f.frozen->Draw(c, dst, in, true);
        }
    }
    // 像素网格
    for (int i = 1; i < (int)pixels; ++i) {
        float x = top.x + i * cell, y = top.y + i * cell;
        c.Line(x, top.y, x, top.b(), Cap().magnifierGrid, 1);
        c.Line(top.x, y, top.r(), y, Cap().magnifierGrid, 1);
    }
    float c0 = half * cell;
    // 十字中线：穿过中心像素中线的 1pt 细线、避开中心框（同 Mac CaptureRender.drawMagnifier）
    Color cross = WithAlpha(Cap().magnifierCenter, 0.55f);
    float mid = c0 + cell / 2 - 0.5f;
    c.FillRect(RectF(top.x, top.y + mid, c0 - 1, 1), cross);
    c.FillRect(RectF(top.x + c0 + cell + 1, top.y + mid, size - c0 - cell - 1, 1), cross);
    c.FillRect(RectF(top.x + mid, top.y, 1, c0 - 1), cross);
    c.FillRect(RectF(top.x + mid, top.y + c0 + cell + 1, 1, size - c0 - cell - 1), cross);
    StrokeInside(c, RectF(top.x + c0, top.y + c0, cell, cell).Inflate(1), Cap().magnifierCenter, 2);
    // 信息区
    c.FillRect(info, Cap().tipBg);
    ls_color col = ls_session_pixel_at(f.session, st.mouse_x, st.mouse_y);
    // 读数与复制出去的值一致：按 color.space 换算（色块和颜色名仍按屏幕上的颜色）
    uint8_t vr = col.r, vg = col.g, vb = col.b;
    if (f.opt.colorTarget >= 0) {
        float in[3] = {col.r / 255.f, col.g / 255.f, col.b / 255.f}, out[3];
        ls_color_convert(LS_SPACE_SRGB, f.opt.colorTarget, in, out);
        vr = (uint8_t)Clamp((int)std::lround(out[0] * 255), 0, 255);
        vg = (uint8_t)Clamp((int)std::lround(out[1] * 255), 0, 255);
        vb = (uint8_t)Clamp((int)std::lround(out[2] * 255), 0, 255);
    }
    char hex[16] = {0};
    if (ls_color_hex(vr, vg, vb, f.opt.hexPrefix ? 1 : 0, f.opt.hexUpper ? 1 : 0, hex, sizeof hex) <= 0)
        snprintf(hex, sizeof hex, f.opt.hexUpper ? "#%02X%02X%02X" : "#%02x%02x%02x", vr, vg, vb);
    char name[64] = {0};
    ls_color_name(col.r, col.g, col.b, i18n::Lang(), name, sizeof name);
    Color white = Cap().tipText;
    float lineH = 14, x = info.x + 8, w = info.w - 16, y = info.y + 4;
    TextStyle ts;
    ts.size = K().fontSize.caption;
    ts.color = white;
    ts.tabular = true;
    c.Text(FormatW(L"%d, %d", (int)px, (int)py), RectF(x, y, w, lineH), ts);
    y += lineH;
    RectF sw(x, y + 2, 10, 10);
    c.FillRound(sw, 2, Color{col.r / 255.f, col.g / 255.f, col.b / 255.f, 1});
    c.StrokeRound(sw.Inflate(-0.5f), 1.5f, Color{1, 1, 1, 0.6f}, 1);
    c.Text(Widen(std::string(hex)), RectF(x + 16, y, w - 16, lineH), ts);
    y += lineH;
    ts.tabular = false;
    c.Text(Widen(std::string(name)), RectF(x, y, w, lineH), ts);
    y += lineH;
    if (f.opt.showTips) {
        // 提示行按宽度自动缩小字号（从 caption-1 起每次 0.5，最小 7），保证完整显示（同 Mac）
        const std::wstring &hint = T("capture.magnifier.copy");
        float fs = K().fontSize.caption - 1;
        while (fs > 7 && gfx::Measure(hint, fs, kRegular).w > w) fs -= 0.5f;
        ts.size = fs;
        ts.color = WithAlpha(white, 0.75f);
        c.Text(hint, RectF(x, y, w, lineH), ts);
    }
    c.PopLayer();
    c.StrokeRound(rect.Inflate(-0.5f), m.radius - 0.5f, Cap().magnifierBorder, 1);
}

void DrawTips(Canvas &c, const CaptureFrame &f, const ls_session_state &st) {
    std::vector<std::wstring> lines;
    if (f.opt.recordMode)
        lines.push_back(T(st.has_selection && st.phase == LS_PHASE_SELECTED ? "record.tip.adjust" : "record.selectRegion"));
    else if (st.has_selection && st.phase == LS_PHASE_SELECTED)
        lines.push_back(T("capture.tip.adjust"));
    else {
        lines.push_back(T("capture.tip.select"));
        lines.push_back(T("capture.tip.keys"));
    }
    float fs = TipFontSize();
    float w = 0;
    for (auto &l : lines) {
        float lw = gfx::Measure(l, fs, kRegular).w;
        if (lw > w) w = lw;
    }
    float lh = gfx::Measure(L"Ag", fs, kRegular).h + 4;
    float bw = std::ceil(w + 28), bh = std::ceil(lh * lines.size() + 12);
    RectF r(16, f.height - 16 - bh, bw, bh);
    c.FillRound(r, bh / 2 < 14 ? bh / 2 : 14, Cap().tipBg);
    TextStyle ts;
    ts.size = fs;
    ts.color = f.opt.tipsColor;
    float y = r.y + 6;
    for (auto &l : lines) {
        c.Text(l, RectF(r.x + 14, y, r.w - 28 + 2, lh), ts);
        y += lh;
    }
}

}  // namespace

CaptureOptions CaptureOptions::FromSettings() {
    CaptureOptions o;
    o.showMagnifier = settings::Bool("capture.showMagnifier");
    o.showCrosshair = settings::Bool("capture.showCrosshair");
    o.showTips = settings::Bool("capture.showTips");
    o.tipsColor = ParseColor(settings::Str("capture.tipsColor"), Color{0.92f, 0.35f, 0.32f, 1});
    o.hexPrefix = settings::Bool("color.hexPrefix");
    o.hexUpper = settings::Bool("color.hexUppercase");
    {
        std::string space = settings::Str("color.space");
        o.colorTarget = space == "p3" ? LS_SPACE_P3 : space == "adobe" ? LS_SPACE_ADOBE : space == "generic" ? LS_SPACE_GENERIC : -1;
    }
    return o;
}

std::wstring RatioName(int index) {
    static const wchar_t *names[] = {L"", L"1:1", L"4:3", L"3:2", L"16:9", L"16:10", L"3:4", L"2:3", L"9:16", L"21:9"};
    return index > 0 && index < 10 ? names[index] : L"";
}

std::wstring SizeTipText(const ls_session *s, float scale) {
    ls_session_state st;
    ls_session_get_state(s, &st);
    int w, h;
    if (st.has_selection) {
        int x, y;
        ls_session_selection_pixels(s, &x, &y, &w, &h);
    } else if (st.has_hover) {
        w = (int)std::floor(st.hover.w * scale + 0.5f);
        h = (int)std::floor(st.hover.h * scale + 0.5f);
    } else {
        return std::wstring();
    }
    std::wstring t = FormatW(L"%d × %d", w, h);
    if (st.window_count > 1)
        t += L"　" + i18n::T("capture.tip.windows", {{"n", FormatW(L"%d", st.window_count)}});
    else if (st.ratio_index > 0 && st.ratio_index < 10)
        t += L"　" + i18n::T("capture.tip.ratio", {{"ratio", RatioName(st.ratio_index)}});
    return t;
}

void LayoutCaptureUi(const CaptureFrame &f, CaptureUi &ui) {
    ls_session_state st;
    ls_session_get_state(f.session, &st);
    const auto &cap = K().capture;
    ui.hasToolbar = false;
    ui.hasPopover = false;
    ui.hasSizeTip = false;
    ui.hasRecordPanel = false;
    if (f.opt.colorPicker) return;
    // 尺寸提示
    std::wstring tip = f.opt.pinMode ? std::wstring() : SizeTipText(f.session, f.scale);
    if (!tip.empty() && (st.has_selection || (st.has_hover && st.mouse_inside))) {
        float tw;
        if (ui.sizeEditing && st.has_selection) {
            float xw = gfx::Measure(L"×", TipFontSize(), kMedium).w;
            tw = cap.tipPaddingX * 2 + ui.sizeEditFieldW * 2 + xw + 12;
        } else {
            tw = std::ceil(gfx::Measure(tip, TipFontSize(), kMedium).w + 2 * cap.tipPaddingX);
        }
        ls_rect r = ls_session_place_size_tip(f.session, tw, cap.tipHeight, cap.tipGap);
        ui.sizeTip = RectF(r);
        ui.hasSizeTip = true;
        float fx = ui.sizeTip.x + cap.tipPaddingX;
        ui.sizeEditW = RectF(fx, ui.sizeTip.y + 3, ui.sizeEditFieldW, cap.tipHeight - 6);
        ui.sizeEditH = RectF(ui.sizeTip.r() - cap.tipPaddingX - ui.sizeEditFieldW, ui.sizeTip.y + 3, ui.sizeEditFieldW,
                             cap.tipHeight - 6);
    }
    // 录屏模式：录屏设置面板代替工具栏，摆放规则相同
    if (f.opt.recordMode) {
        if (ui.recordPanel && st.phase == LS_PHASE_SELECTED && st.has_selection && st.toolbar_visible && !st.dragging_selection) {
            SizeF ps = ui.recordPanel->Size();
            ls_rect tr = ls_session_place_toolbar(f.session, ps.w, ps.h, cap.toolbarGap, cap.toolbarScreenMargin);
            ui.recordPanel->SetRect(RectF(tr), tr.y >= st.selection.y + st.selection.h);
            ui.hasRecordPanel = true;
        }
        return;
    }
    // 工具栏：SELECTED、可见、不在拖动选区
    if (st.phase == LS_PHASE_SELECTED && st.has_selection && st.toolbar_visible && !st.dragging_selection) {
        ls_rect tr = ls_session_place_toolbar(f.session, ToolbarWidth(f.opt.pinMode), ToolbarHeight(), cap.toolbarGap,
                                              cap.toolbarScreenMargin);
        ui.toolbar = LayoutToolbar(tr.x, tr.y, f.opt.pinMode);
        ui.hasToolbar = true;
        ui.toolbarBelow = tr.y >= st.selection.y + st.selection.h;
        // 样式面板：悬停的工具优先，否则当前工具
        int tool = ui.hoverPopoverTool ? ui.hoverPopoverTool : st.tool;
        if (st.window_count > 0) tool = 0;
        if (tool >= 1 && tool <= 10) {
            ls_style style;
            ls_session_get_style(f.session, tool, &style);
            ui.popover = LayoutPopover(tool, style, true);
            RectF btn = ui.toolbar.ButtonRect(tool);
            // 面板与工具栏间距用 capture.toolbarGap（同 Mac Session.placePopover）
            ls_rect pr = ls_session_place_popover(f.session, ui.toolbar.rect.Ls(), btn.cx(), ui.popover.w, ui.popover.h,
                                                  cap.toolbarGap, cap.toolbarScreenMargin);
            ui.popoverRect = RectF(pr);
            ui.popoverTool = tool;
            ui.hasPopover = true;
        }
    }
}

void RenderCapture(Canvas &c, const CaptureFrame &f, CaptureUi &ui) {
    LayoutCaptureUi(f, ui);
    ls_session_state st;
    ls_session_get_state(f.session, &st);
    RectF bounds(0, 0, f.width, f.height);
    // 1. 冻结画面
    if (f.frozen && f.frozen->Valid())
        f.frozen->Draw(c, bounds, RectF(0, 0, (float)f.frozen->Width(), (float)f.frozen->Height()), true);
    else
        c.FillRect(bounds, Color{0, 0, 0, 1});
    if (f.opt.colorPicker) {
        if (st.mouse_inside) {
            if (f.opt.showCrosshair) {
                c.FillRect(RectF(0, std::floor(st.mouse_y), f.width, 1), Cap().crosshair);
                c.FillRect(RectF(std::floor(st.mouse_x), 0, 1, f.height), Cap().crosshair);
            }
            DrawMagnifier(c, f, st);
        }
        return;
    }
    // 2. 遮罩 / 悬停
    RectF sel(st.selection);
    if (f.opt.pinMode) {
        // 贴图标注：图片以外是透明区（窗口区域已裁掉），不画遮罩
    } else if (st.has_selection) {
        FillOutside(c, bounds, sel, Cap().mask);
    } else if (st.has_hover) {
        RectF h(st.hover);
        FillOutside(c, bounds, h, Cap().mask);
        c.FillRect(h.Intersect(bounds), Cap().windowHoverFill);
        StrokeInside(c, h.Intersect(bounds), Cap().windowHover, K().capture.windowHoverBorderWidth);
    } else {
        c.FillRect(bounds, Cap().mask);
    }
    // 3. 标注（裁剪到选区）
    if (st.has_selection && st.phase == LS_PHASE_SELECTED) {
        if (f.patches) f.patches->BeginFrame();
        AnnotationContext ctx;
        ctx.base = f.frozen;
        ctx.scale = f.scale;
        ctx.patches = f.patches;
        DrawAnnotations(c, f.session, ctx, sel);
        if (f.patches) f.patches->EndFrame();
    }
    // 4. 选区边框与控制点；选中标注的控制点
    if (st.has_selection) {
        bool handles = st.phase == LS_PHASE_SELECTED && !st.dragging_shape;
        if (!f.opt.pinMode) DrawSelectionChrome(c, sel, handles);
        if (st.phase == LS_PHASE_SELECTED && st.selected_shape) DrawShapeHandles(c, f.session);
    }
    if (f.opt.pinMode) {
        if (ui.hasToolbar) {
            DrawToolbar(c, ui.toolbar, st, ui.hoverBtn, ui.pressedBtn);
            if (ui.hasPopover) {
                ls_style style;
                ls_session_get_style(f.session, ui.popoverTool, &style);
                DrawPopover(c, ui.popover, ui.popoverRect.x, ui.popoverRect.y, style, ui.popHover, true);
                if (ui.popHover >= 0 && ui.popHover < (int)ui.popover.items.size() &&
                    ui.popover.items[(size_t)ui.popHover].kind == PK_INFO)
                    DrawStyleHint(c, ui.popoverRect, f.width, f.height);
            }
            if (ui.tooltipBtn && !(ui.tooltipBtn >= 1 && ui.tooltipBtn <= 10)) {
                RectF b = ui.toolbar.ButtonRect(ui.tooltipBtn);
                if (!b.Empty()) DrawToolbarTooltip(c, b, ToolbarTooltip(ui.tooltipBtn), true, f.width, f.height);
            }
        }
        return;
    }
    // 5. 十字辅助线
    if (f.opt.showCrosshair && st.phase == LS_PHASE_SELECTING && st.mouse_inside) {
        c.FillRect(RectF(0, std::floor(st.mouse_y), f.width, 1), Cap().crosshair);
        c.FillRect(RectF(std::floor(st.mouse_x), 0, 1, f.height), Cap().crosshair);
    }
    // 6. 尺寸提示
    if (ui.hasSizeTip) {
        const auto &cap = K().capture;
        c.FillRound(ui.sizeTip, cap.tipRadius, Cap().tipBg);
        TextStyle ts;
        ts.size = TipFontSize();
        ts.weight = kMedium;
        ts.color = Cap().tipText;
        ts.tabular = true;
        if (ui.sizeEditing) {
            // 两个输入框由原生控件画，这里只画底框和「×」
            c.FillRound(ui.sizeEditW, 3, Color{1, 1, 1, 0.12f});
            c.FillRound(ui.sizeEditH, 3, Color{1, 1, 1, 0.12f});
            ts.h = HAlign::Center;
            c.Text(L"×", RectF(ui.sizeEditW.r(), ui.sizeTip.y, ui.sizeEditH.x - ui.sizeEditW.r(), ui.sizeTip.h), ts);
        } else {
            c.Text(SizeTipText(f.session, f.scale), RectF(ui.sizeTip.x + cap.tipPaddingX, ui.sizeTip.y,
                                                          ui.sizeTip.w - cap.tipPaddingX * 2 + 2, ui.sizeTip.h),
                   ts);
        }
    }
    // 7. 放大镜
    if (f.opt.showMagnifier && !f.opt.recordMode && st.show_magnifier && st.mouse_inside) DrawMagnifier(c, f, st);
    // 8. 左下角提示
    if (f.opt.showTips) DrawTips(c, f, st);
    // 9. 工具栏、样式面板、提示气泡（录屏模式为录屏设置面板）
    if (ui.hasRecordPanel && ui.recordPanel) ui.recordPanel->Draw(c, f.width, f.height);
    if (ui.hasToolbar) {
        DrawToolbar(c, ui.toolbar, st, ui.hoverBtn, ui.pressedBtn);
        if (ui.hasPopover) {
            ls_style style;
            ls_session_get_style(f.session, ui.popoverTool, &style);
            DrawPopover(c, ui.popover, ui.popoverRect.x, ui.popoverRect.y, style, ui.popHover, true);
            if (ui.popHover >= 0 && ui.popHover < (int)ui.popover.items.size() &&
                ui.popover.items[(size_t)ui.popHover].kind == PK_INFO)
                DrawStyleHint(c, ui.popoverRect, f.width, f.height);
        }
        if (ui.tooltipBtn && !(ui.tooltipBtn >= 1 && ui.tooltipBtn <= 10)) {
            RectF b = ui.toolbar.ButtonRect(ui.tooltipBtn);
            if (!b.Empty()) DrawToolbarTooltip(c, b, ToolbarTooltip(ui.tooltipBtn), ui.toolbarBelow, f.width, f.height);
        }
    }
}

}  // namespace ls
