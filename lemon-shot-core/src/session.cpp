// 截图会话：选区与标注编辑的交互状态机。行为契约见 docs/session-behavior.md。
//
// 结构：每个输入函数先记下关键状态（Before），处理完再比较，得出 LS_EV_* 位掩码；
// 标注列表的每次提交前存一份快照进撤销栈；绘制数据（路径、马赛克 patch）在读取时按版本号懒重建。
#include "session_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace lsk;

namespace {

const float kDragThreshold = 3.0f;
const size_t kMaxUndo = 100;
const float kRatios[10] = {0, 1, 4.0f / 3, 3.0f / 2, 16.0f / 9, 16.0f / 10, 3.0f / 4, 2.0f / 3, 9.0f / 16, 21.0f / 9};

enum { MERGE_NONE = 0, MERGE_WHEEL_SHAPE, MERGE_WHEEL_STYLE, MERGE_NUDGE, MERGE_STYLE };

enum { HIT_OUTSIDE = 0, HIT_INSIDE, HIT_SEL_HANDLE, HIT_SEL_EDGE, HIT_SHAPE_HANDLE, HIT_SHAPE };

struct Hit {
    int kind;
    int handle;
    int32_t shape;
};

ls_color hex(uint32_t v) {
    ls_color c;
    c.r = (uint8_t)(v >> 16);
    c.g = (uint8_t)(v >> 8);
    c.b = (uint8_t)v;
    c.a = 255;
    return c;
}

// 默认样式取 settings.defaults.json 的 annotate 段（平台通常会在创建后用设置覆盖）
void default_styles(ls_style st[LS_TOOL_COUNT]) {
    std::memset(st, 0, sizeof(ls_style) * LS_TOOL_COUNT);
    for (int t = 0; t < LS_TOOL_COUNT; ++t) {
        st[t].color = hex(0xFF3B30);
        st[t].line_width = 3;
        st[t].opacity = 1;
        st[t].font_size = 18;
        st[t].block_size = 10;
        st[t].zoom = 2;
        st[t].next_number = 1;
    }
    st[LS_TOOL_ARROW].line_width = 4;
    st[LS_TOOL_MOSAIC].line_width = 24; // 画笔样式的笔刷直径
    st[LS_TOOL_STEP].line_width = 24;   // 圆直径
    st[LS_TOOL_HIGHLIGHT].opacity = 0.55f;
    st[LS_TOOL_HIGHLIGHT].corner_radius = 6;
    st[LS_TOOL_MAGNIFY].line_width = 3;
}

void default_bindings(int32_t b[LS_BIND_MAX]) {
    for (int i = 0; i < LS_BIND_MAX; ++i) b[i] = 0;
    for (int t = LS_TOOL_RECT; t <= LS_TOOL_MAGNIFY; ++t) b[t] = t == LS_TOOL_MAGNIFY ? '0' : '0' + t;
    b[LS_BIND_LONGSHOT] = 'S';
    b[LS_BIND_OCR] = 'O';
    b[LS_BIND_TRANSLATE] = 'T';
    b[LS_BIND_PIN] = 'P';
    b[LS_BIND_RECORD] = 'V';
    b[LS_BIND_LAST_AREA] = 'D';
    b[LS_BIND_FULLSCREEN] = 'F';
    b[LS_BIND_COPY_RGB] = 'R';
    b[LS_BIND_COPY_HEX] = 'H';
    b[LS_BIND_COPY_CUSTOM] = 'C';
    b[LS_BIND_TOGGLE_TOOLBAR] = LS_KEY_TAB;
}

float round2(float v) { return std::floor(v * 100 + 0.5f) / 100; }

// ---------------------------------------------------------------- 像素对齐与屏幕夹取

float snapv(const ls_session *s, float v) { return std::floor(v * s->scale + 0.5f) / s->scale; }

ls_rect snap_rect(const ls_session *s, ls_rect r) {
    const float x0 = snapv(s, r.x), y0 = snapv(s, r.y), x1 = snapv(s, r.x + r.w), y1 = snapv(s, r.y + r.h);
    return rc(x0, y0, x1 - x0, y1 - y0);
}

ls_rect clamp_rect(const ls_session *s, ls_rect r) {
    const float x0 = clampf(r.x, 0, s->W), y0 = clampf(r.y, 0, s->H);
    const float x1 = clampf(r.x + r.w, 0, s->W), y1 = clampf(r.y + r.h, 0, s->H);
    return rc(x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0));
}

ls_point clamp_pt(const ls_session *s, ls_point p) { return pt(clampf(p.x, 0, s->W), clampf(p.y, 0, s->H)); }

ls_rect full_screen(const ls_session *s) { return snap_rect(s, rc(0, 0, s->W, s->H)); }

bool rect_eq(ls_rect a, ls_rect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

// ---------------------------------------------------------------- 标注列表与撤销

lsk::Shape *find_shape(ls_session *s, int32_t id) {
    if (!id) return nullptr;
    for (Shape &sh : s->shapes)
        if (sh.id == id) return &sh;
    return nullptr;
}

const lsk::Shape *find_shape(const ls_session *s, int32_t id) {
    if (!id) return nullptr;
    for (const Shape &sh : s->shapes)
        if (sh.id == id) return &sh;
    return nullptr;
}

void touch(ls_session *s, Shape &sh) {
    sh.rev = ++s->rev_counter;
    ++s->shapes_version;
}

Snapshot make_snapshot(const ls_session *s) {
    Snapshot sn;
    sn.shapes = s->shapes;
    sn.step_next = s->styles[LS_TOOL_STEP].next_number;
    return sn;
}

void push_undo(ls_session *s, Snapshot &&sn) {
    s->undo_stack.push_back(std::move(sn));
    if (s->undo_stack.size() > kMaxUndo) s->undo_stack.erase(s->undo_stack.begin());
    s->redo_stack.clear();
}

// 在修改标注之前调用
void commit(ls_session *s) {
    s->merge_kind = MERGE_NONE;
    push_undo(s, make_snapshot(s));
}

// 连续同类修改（滚轮、方向键、拖样式滑块）合并成一条撤销记录；aux 区分改的是哪类字段
void commit_merged(ls_session *s, int kind, int32_t id, int32_t aux) {
    const int32_t key = id * 64 + (aux & 63);
    if (s->merge_kind == kind && s->merge_id == key && !s->undo_stack.empty()) return;
    push_undo(s, make_snapshot(s));
    s->merge_kind = kind;
    s->merge_id = key;
}

void break_merge(ls_session *s) {
    s->merge_kind = MERGE_NONE;
    s->wheel_kind = 0;
    s->wheel_id = 0;
    s->wheel_acc = 0;
}

void gc_geoms(const ls_session *s) {
    for (auto it = s->geoms.begin(); it != s->geoms.end();) {
        if (!find_shape(s, it->first))
            it = s->geoms.erase(it);
        else
            ++it;
    }
}

void remove_shape(ls_session *s, int32_t id) {
    for (size_t i = 0; i < s->shapes.size(); ++i) {
        if (s->shapes[i].id == id) {
            s->shapes.erase(s->shapes.begin() + (std::ptrdiff_t)i);
            ++s->shapes_version;
            break;
        }
    }
    if (s->selected_id == id) s->selected_id = 0;
    if (s->hovered_id == id) s->hovered_id = 0;
    if (s->editing_id == id) s->editing_id = 0;
    gc_geoms(s);
}

bool same_step_ids(const std::vector<Shape> &a, const std::vector<Shape> &b) {
    std::vector<int32_t> ia, ib;
    for (const Shape &s : a)
        if (s.type == LS_TOOL_STEP) ia.push_back(s.id);
    for (const Shape &s : b)
        if (s.type == LS_TOOL_STEP) ib.push_back(s.id);
    return ia == ib;
}

void restore(ls_session *s, Snapshot &&sn) {
    // 序号增删时连带恢复「下一个」，避免撤销后数字跳号；其他撤销不动用户手调的数字
    if (!same_step_ids(s->shapes, sn.shapes) && s->styles[LS_TOOL_STEP].next_number != sn.step_next) {
        s->styles[LS_TOOL_STEP].next_number = sn.step_next;
        ++s->style_version;
    }
    s->shapes = std::move(sn.shapes);
    ++s->shapes_version;
    if (!find_shape(s, s->selected_id)) s->selected_id = 0;
    if (!find_shape(s, s->hovered_id)) s->hovered_id = 0;
    gc_geoms(s);
}

// ---------------------------------------------------------------- 文字

void measure_text(const ls_session *s, const char *text, float fs, int32_t bold, float *w, float *h) {
    *w = *h = 0;
    if (s->measurer) s->measurer(s->measurer_ctx, text, fs, bold, w, h);
    if (s->measurer && *h > 0) return;
    // 没有测量回调时粗略估计：ASCII 0.6 字宽，其他 1 字宽，行高 1.25
    float line = 0, maxw = 0;
    int lines = 1;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '\n') {
            maxw = std::max(maxw, line);
            line = 0;
            ++lines;
        } else if (*p < 0x80) {
            line += 0.6f * fs;
        } else if ((*p & 0xC0) != 0x80) {
            line += fs;
        }
    }
    *w = std::max(maxw, line);
    *h = lines * 1.25f * fs;
}

void measure_shape(const ls_session *s, Shape &sh) {
    float w = 0, h = 0;
    const float fs = sh.style.font_size;
    if (sh.text.empty()) {
        float mw = 0;
        measure_text(s, "M", fs, sh.style.bold, &mw, &h);
        w = fs; // 空文字按一个字宽、一行高
    } else {
        measure_text(s, sh.text.c_str(), fs, sh.style.bold, &w, &h);
    }
    sh.text_rect = rc(sh.p0.x, sh.p0.y, w, h);
}

int end_text_internal(ls_session *s) {
    if (!s->editing_id) return 0;
    Shape *sh = find_shape(s, s->editing_id);
    const int32_t id = s->editing_id;
    s->editing_id = 0;
    if (sh) {
        if (sh->text.empty()) {
            remove_shape(s, id);
            if (!s->text_is_new) push_undo(s, std::move(s->text_snap));
        } else if (s->text_is_new || s->text_dirty || sh->text != s->text_orig) {
            push_undo(s, std::move(s->text_snap));
        }
        ++s->shapes_version;
    }
    s->text_snap = Snapshot();
    s->merge_kind = MERGE_NONE;
    return LS_EV_TEXT_END;
}

// ---------------------------------------------------------------- 命中

bool sel_locked(const ls_session *s) { return s->cfg.lock_selection || !s->multi.empty(); }

Hit hit_test(const ls_session *s, float x, float y) {
    Hit h{HIT_OUTSIDE, -1, 0};
    if (!s->has_sel) return h;
    const ls_rect r = s->sel;
    if (!sel_locked(s)) {
        ls_point hp[8];
        rect_handle_points(r, hp);
        const float half = s->cfg.handle_hit * 0.5f;
        float bestd = 1e30f;
        for (int i = 0; i < 8; ++i) {
            if (std::fabs(x - hp[i].x) <= half && std::fabs(y - hp[i].y) <= half) {
                const float d = dist(pt(x, y), hp[i]);
                if (d < bestd) {
                    bestd = d;
                    h.kind = HIT_SEL_HANDLE;
                    h.handle = i;
                }
            }
        }
        if (h.kind == HIT_SEL_HANDLE) return h;
        const float e = s->cfg.edge_hit * 0.5f;
        const bool in_x = x >= r.x - e && x <= r.x + r.w + e;
        const bool in_y = y >= r.y - e && y <= r.y + r.h + e;
        struct {
            float d;
            int handle;
            bool ok;
        } edges[4] = {{std::fabs(y - r.y), 1, in_x},
                      {std::fabs(x - (r.x + r.w)), 3, in_y},
                      {std::fabs(y - (r.y + r.h)), 5, in_x},
                      {std::fabs(x - r.x), 7, in_y}};
        bestd = 1e30f;
        for (auto &ed : edges) {
            if (ed.ok && ed.d <= e && ed.d < bestd) {
                bestd = ed.d;
                h.kind = HIT_SEL_EDGE;
                h.handle = ed.handle;
            }
        }
        if (h.kind == HIT_SEL_EDGE) return h;
    }
    const bool in_sel = rect_contains(r, x, y);
    if (s->selected_id && s->selected_id != s->editing_id) {
        if (const Shape *sh = find_shape(s, s->selected_id)) {
            ls_point hp[9];
            const int n = shape_handles(*sh, hp);
            const float half = s->cfg.handle_hit * 0.5f;
            for (int i = 0; i < n; ++i) {
                if (std::fabs(x - hp[i].x) <= half && std::fabs(y - hp[i].y) <= half) {
                    h.kind = HIT_SHAPE_HANDLE;
                    h.handle = i;
                    h.shape = sh->id;
                    return h;
                }
            }
        }
    }
    if (in_sel) {
        for (size_t i = s->shapes.size(); i-- > 0;) {
            const Shape &sh = s->shapes[i];
            if (sh.id == s->editing_id) continue;
            if (hit_shape(sh, pt(x, y), s->tool != LS_TOOL_NONE)) {
                h.kind = HIT_SHAPE;
                h.shape = sh.id;
                return h;
            }
        }
        h.kind = HIT_INSIDE;
    }
    return h;
}

int32_t shape_under(const ls_session *s, float x, float y) {
    if (!s->has_sel || !rect_contains(s->sel, x, y)) return 0;
    for (size_t i = s->shapes.size(); i-- > 0;) {
        const Shape &sh = s->shapes[i];
        if (sh.id == s->editing_id) continue;
        if (hit_shape(sh, pt(x, y), s->tool != LS_TOOL_NONE)) return sh.id;
    }
    return 0;
}

int compute_cursor(const ls_session *s) {
    if (s->pressed && s->drag != LSK_DRAG_NONE) return s->press_cursor;
    if (s->phase == LS_PHASE_SELECTING) return LS_CURSOR_CROSSHAIR;
    const Hit h = hit_test(s, s->mx, s->my);
    switch (h.kind) {
    case HIT_SEL_HANDLE:
    case HIT_SEL_EDGE: return rect_handle_cursor(h.handle);
    case HIT_SHAPE_HANDLE: {
        const Shape *sh = find_shape(s, h.shape);
        return sh ? shape_handle_cursor(*sh, h.handle) : LS_CURSOR_ARROW;
    }
    case HIT_SHAPE: return LS_CURSOR_POINTER;
    case HIT_INSIDE:
        if (!s->multi.empty()) return LS_CURSOR_ARROW;
        if (s->tool == LS_TOOL_NONE) return s->cfg.lock_selection ? LS_CURSOR_ARROW : LS_CURSOR_MOVE;
        return s->tool == LS_TOOL_TEXT ? LS_CURSOR_IBEAM : LS_CURSOR_CROSSHAIR;
    default: return (s->shapes.empty() && !s->cfg.lock_selection) ? LS_CURSOR_CROSSHAIR : LS_CURSOR_ARROW;
    }
}

// ---------------------------------------------------------------- 窗口悬停与多选

bool window_at(const ls_session *s, float x, float y, ls_rect *out, int64_t *id) {
    if (!s->cfg.detect_windows) return false;
    for (const ls_window &w : s->windows) {
        if (!rect_contains(w.rect, x, y)) continue;
        const ls_rect r = snap_rect(s, clamp_rect(s, w.rect));
        if (r.w <= 0 || r.h <= 0) continue;
        *out = r;
        *id = w.id;
        return true;
    }
    return false;
}

void update_hover(ls_session *s) {
    if (s->phase != LS_PHASE_SELECTING || !s->inside || (s->pressed && s->drag != LSK_DRAG_PENDING)) {
        s->has_hover = false;
        return;
    }
    if (s->pressed) return; // 按下未拖动：保持按下时的悬停（单击要用）
    ls_rect r;
    int64_t id = 0;
    if (window_at(s, s->mx, s->my, &r, &id)) {
        s->hover = r;
        s->hover_is_window = true;
        s->hover_id = id;
    } else {
        s->hover = full_screen(s);
        s->hover_is_window = false;
        s->hover_id = 0;
    }
    s->has_hover = true;
}

void refresh(ls_session *s) {
    update_hover(s);
    if (s->phase == LS_PHASE_SELECTED && s->inside && !s->pressed)
        s->hovered_id = shape_under(s, s->mx, s->my);
    else if (!s->inside || s->phase != LS_PHASE_SELECTED)
        s->hovered_id = 0;
    s->cursor = compute_cursor(s);
}

void enter_selected(ls_session *s, ls_rect r) {
    s->sel = r;
    s->has_sel = true;
    s->phase = LS_PHASE_SELECTED;
    s->has_hover = false;
    s->sel_window = 0;
    ++s->sel_version;
}

// 选区 = 悬停高亮（单击或回车）；悬停的是窗口时记下它，导出时按窗口截图加圆角阴影
void select_hover(ls_session *s) {
    const bool window = s->has_hover && s->hover_is_window;
    const int64_t id = s->hover_id;
    s->multi.clear();
    enter_selected(s, s->has_hover ? s->hover : full_screen(s));
    if (window) {
        s->sel_window = id;
        s->sel_window_rect = s->sel;
    }
}

void reset_to_selecting(ls_session *s) {
    s->has_sel = false;
    s->sel = rc(0, 0, 0, 0);
    s->phase = LS_PHASE_SELECTING;
    s->multi.clear();
    s->sel_window = 0;
    ++s->sel_version;
}

void recompute_multi(ls_session *s) {
    bool any = false;
    ls_rect u = rc(0, 0, 0, 0);
    for (int64_t id : s->multi) {
        for (const ls_window &w : s->windows) {
            if (w.id != id) continue;
            const ls_rect r = snap_rect(s, clamp_rect(s, w.rect));
            u = any ? rect_union(u, r) : r;
            any = true;
            break;
        }
    }
    if (s->multi.empty() || !any) {
        s->multi.clear();
        reset_to_selecting(s);
    } else {
        enter_selected(s, u);
    }
}

void toggle_window(ls_session *s, int64_t id) {
    auto it = std::find(s->multi.begin(), s->multi.end(), id);
    if (it != s->multi.end())
        s->multi.erase(it);
    else
        s->multi.push_back(id);
    recompute_multi(s);
}

void set_selection_internal(ls_session *s, ls_rect r) {
    r.w = std::min(std::max(r.w, 0.0f), s->W);
    r.h = std::min(std::max(r.h, 0.0f), s->H);
    r.x = clampf(r.x, 0, s->W - r.w);
    r.y = clampf(r.y, 0, s->H - r.h);
    r = snap_rect(s, r);
    const float px = 1 / s->scale;
    if (r.w < px) r.w = px;
    if (r.h < px) r.h = px;
    s->multi.clear();
    enter_selected(s, r);
}

// ---------------------------------------------------------------- 选区拖动

// 以 F 为固定角、宽度跟随 C 的比例矩形；超出屏幕时两边一起缩小。use_max：正方形边长取 max(|dx|,|dy|)
ls_rect ratio_rect(const ls_session *s, ls_point F, ls_point C, float ratio, bool use_max) {
    const float dx = C.x - F.x, dy = C.y - F.y;
    const float sx = dx < 0 ? -1.0f : 1.0f, sy = dy < 0 ? -1.0f : 1.0f;
    float w = std::fabs(dx), h = w / ratio;
    if (use_max) w = h = std::max(std::fabs(dx), std::fabs(dy));
    const float maxw = sx > 0 ? s->W - F.x : F.x, maxh = sy > 0 ? s->H - F.y : F.y;
    if (w > maxw) {
        w = maxw;
        h = w / ratio;
    }
    if (h > maxh) {
        h = maxh;
        w = h * ratio;
    }
    return rc(sx > 0 ? F.x : F.x - w, sy > 0 ? F.y : F.y - h, w, h);
}

ls_rect new_sel_rect(const ls_session *s, ls_point cur) {
    const ls_point a = clamp_pt(s, s->press_pt), b = clamp_pt(s, cur);
    const float ratio = kRatios[s->ratio_index];
    if (ratio > 0) return snap_rect(s, ratio_rect(s, a, b, ratio, false));
    if (s->mods & LS_MOD_SHIFT) return snap_rect(s, ratio_rect(s, a, b, 1, true));
    return snap_rect(s, rect_from(a, b));
}

ls_rect resize_sel_rect(const ls_session *s, ls_point cur) {
    const ls_rect o = s->press_sel;
    const int hd = s->press_handle;
    const float dx = cur.x - s->press_pt.x, dy = cur.y - s->press_pt.y;
    float ratio = kRatios[s->ratio_index];
    if (ratio <= 0 && (s->mods & LS_MOD_SHIFT) && o.w > 0 && o.h > 0) ratio = o.w / o.h;
    if (ratio <= 0) return snap_rect(s, clamp_rect(s, drag_rect_handle(o, hd, dx, dy)));
    ls_point hp[8];
    rect_handle_points(o, hp);
    if (hd % 2 == 0) {
        // 角：对角固定
        const ls_point F = hp[(hd + 4) % 8];
        const ls_point C = clamp_pt(s, pt(hp[hd].x + dx, hp[hd].y + dy));
        return snap_rect(s, ratio_rect(s, F, C, ratio, false));
    }
    if (hd == 3 || hd == 7) {
        const float fx = hd == 3 ? o.x : o.x + o.w;
        const float cx = clampf((hd == 3 ? o.x + o.w : o.x) + dx, 0, s->W);
        const float sx = cx - fx < 0 ? -1.0f : 1.0f;
        float w = std::fabs(cx - fx), h = w / ratio;
        const float maxw = sx > 0 ? s->W - fx : fx;
        if (w > maxw) {
            w = maxw;
            h = w / ratio;
        }
        if (h > s->H) {
            h = s->H;
            w = h * ratio;
        }
        const float y = clampf(o.y + o.h / 2 - h / 2, 0, s->H - h);
        return snap_rect(s, rc(sx > 0 ? fx : fx - w, y, w, h));
    }
    const float fy = hd == 5 ? o.y : o.y + o.h;
    const float cy = clampf((hd == 5 ? o.y + o.h : o.y) + dy, 0, s->H);
    const float sy = cy - fy < 0 ? -1.0f : 1.0f;
    float h = std::fabs(cy - fy), w = h * ratio;
    const float maxh = sy > 0 ? s->H - fy : fy;
    if (h > maxh) {
        h = maxh;
        w = h * ratio;
    }
    if (w > s->W) {
        w = s->W;
        h = w / ratio;
    }
    const float x = clampf(o.x + o.w / 2 - w / 2, 0, s->W - w);
    return snap_rect(s, rc(x, sy > 0 ? fy : fy - h, w, h));
}

ls_rect move_sel_rect(const ls_session *s, ls_point cur) {
    const ls_rect o = s->press_sel;
    const float x = clampf(o.x + cur.x - s->press_pt.x, 0, s->W - o.w);
    const float y = clampf(o.y + cur.y - s->press_pt.y, 0, s->H - o.h);
    return snap_rect(s, rc(x, y, o.w, o.h));
}

// 拉伸结束后保证不小于最小尺寸（从固定边往外补）
ls_rect enforce_min(const ls_session *s, ls_rect r) {
    const float m = s->cfg.min_selection;
    if (r.w < m) {
        r.w = std::min(m, s->W);
        if (r.x + r.w > s->W) r.x = s->W - r.w;
    }
    if (r.h < m) {
        r.h = std::min(m, s->H);
        if (r.y + r.h > s->H) r.y = s->H - r.h;
    }
    return snap_rect(s, r);
}

// ---------------------------------------------------------------- 标注拖动

ls_point snap45(ls_point from, ls_point to) {
    const float dx = to.x - from.x, dy = to.y - from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0) return to;
    const double step = kPi / 4;
    const double a = std::floor(std::atan2(dy, dx) / step + 0.5) * step;
    float c = (float)std::cos(a), sn = (float)std::sin(a);
    if (std::fabs(c) < 1e-6f) c = 0;
    if (std::fabs(sn) < 1e-6f) sn = 0;
    return pt(from.x + len * c, from.y + len * sn);
}

void translate_shape(Shape &sh, const Shape &o, float dx, float dy) {
    sh.p0 = pt(o.p0.x + dx, o.p0.y + dy);
    sh.p1 = pt(o.p1.x + dx, o.p1.y + dy);
    sh.ctrl = pt(o.ctrl.x + dx, o.ctrl.y + dy);
    sh.pts.resize(o.pts.size());
    for (size_t i = 0; i < o.pts.size(); ++i) sh.pts[i] = pt(o.pts[i].x + dx, o.pts[i].y + dy);
    sh.text_rect = rc(o.text_rect.x + dx, o.text_rect.y + dy, o.text_rect.w, o.text_rect.h);
}

void update_new_shape(ls_session *s, ls_point p) {
    Shape *sh = find_shape(s, s->drawing_id);
    if (!sh) return;
    const bool shift = (s->mods & LS_MOD_SHIFT) != 0;
    if (is_rect_shape(*sh)) {
        ls_point q = p;
        if (shift) {
            const float dx = p.x - sh->p0.x, dy = p.y - sh->p0.y;
            const float side = std::max(std::fabs(dx), std::fabs(dy));
            q = pt(sh->p0.x + (dx < 0 ? -side : side), sh->p0.y + (dy < 0 ? -side : side));
        }
        sh->p1 = q;
    } else if (is_line_shape(*sh)) {
        sh->p1 = shift ? snap45(sh->p0, p) : p;
        if (sh->type == LS_TOOL_ARROW && sh->style.variant == 3) default_ctrl(*sh);
    } else if (is_brush_shape(*sh)) {
        if (shift) {
            // 按住 Shift：从按下 Shift 时的点拉直线
            if (s->shift_anchor < 0) s->shift_anchor = (int)sh->pts.size() - 1;
            sh->pts.resize((size_t)s->shift_anchor + 1);
            sh->pts.push_back(p);
        } else {
            s->shift_anchor = -1; // 松开 Shift 后从直线终点继续
            if (sh->pts.empty() || dist(p, sh->pts.back()) >= 1.0f) sh->pts.push_back(p);
        }
    } else if (sh->type == LS_TOOL_MAGNIFY) {
        sh->p1 = p;
    }
    touch(s, *sh);
}

void update_reshape(ls_session *s, ls_point p) {
    Shape *sh = find_shape(s, s->press_shape);
    if (!sh) return;
    const Shape &o = s->press_orig;
    const float dx = p.x - s->press_pt.x, dy = p.y - s->press_pt.y;
    const bool shift = (s->mods & LS_MOD_SHIFT) != 0;
    if (is_rect_shape(o)) {
        const ls_rect r = drag_rect_handle(rect_from(o.p0, o.p1), s->press_handle, dx, dy);
        sh->p0 = pt(r.x, r.y);
        sh->p1 = pt(r.x + r.w, r.y + r.h);
    } else if (is_line_shape(o)) {
        if (s->press_handle == 0) {
            const ls_point q = pt(o.p0.x + dx, o.p0.y + dy);
            sh->p0 = shift ? snap45(o.p1, q) : q;
        } else if (s->press_handle == 1) {
            const ls_point q = pt(o.p1.x + dx, o.p1.y + dy);
            sh->p1 = shift ? snap45(o.p0, q) : q;
        } else {
            const ls_point m0 = curve_mid(o);
            const ls_point m = pt(m0.x + dx, m0.y + dy);
            // 让曲线经过手柄：ctrl = 2m - (p0+p1)/2
            sh->ctrl = pt(2 * m.x - (o.p0.x + o.p1.x) / 2, 2 * m.y - (o.p0.y + o.p1.y) / 2);
        }
    } else if (o.type == LS_TOOL_MAGNIFY) {
        const float r = std::max(12.0f, dist(o.p0, p));
        sh->p1 = pt(o.p0.x + r, o.p0.y);
    }
    touch(s, *sh);
}

void update_drag(ls_session *s) {
    const ls_point p = pt(s->mx, s->my);
    switch (s->drag) {
    case LSK_DRAG_NEW_SEL: {
        const ls_rect r = new_sel_rect(s, p);
        if (!rect_eq(r, s->sel)) {
            s->sel = r;
            ++s->sel_version;
        }
        break;
    }
    case LSK_DRAG_RESIZE_SEL: {
        const ls_rect r = resize_sel_rect(s, p);
        if (!rect_eq(r, s->sel)) {
            s->sel = r;
            ++s->sel_version;
        }
        break;
    }
    case LSK_DRAG_MOVE_SEL: {
        const ls_rect r = move_sel_rect(s, p);
        if (!rect_eq(r, s->sel)) {
            s->sel = r;
            ++s->sel_version;
        }
        break;
    }
    case LSK_DRAG_NEW_SHAPE: update_new_shape(s, p); break;
    case LSK_DRAG_MOVE_SHAPE: {
        Shape *sh = find_shape(s, s->press_shape);
        if (sh) {
            translate_shape(*sh, s->press_orig, p.x - s->press_pt.x, p.y - s->press_pt.y);
            touch(s, *sh);
        }
        break;
    }
    case LSK_DRAG_RESHAPE: update_reshape(s, p); break;
    default: break;
    }
}

void create_shape_at_press(ls_session *s) {
    Shape sh;
    sh.id = s->next_id++;
    sh.type = s->tool;
    sh.style = s->styles[s->tool];
    sh.p0 = sh.p1 = s->press_pt;
    if (is_brush_shape(sh)) sh.pts.push_back(s->press_pt);
    if (sh.type == LS_TOOL_ARROW && sh.style.variant == 3) default_ctrl(sh);
    touch(s, sh);
    s->shapes.push_back(sh);
    s->selected_id = sh.id;
    s->drawing_id = sh.id;
    s->shift_anchor = -1;
}

void start_drag(ls_session *s) {
    switch (s->press_target) {
    case LSK_T_NEW_SEL:
        s->drag = LSK_DRAG_NEW_SEL;
        s->multi.clear();
        s->sel_window = 0;
        s->selected_id = 0;
        s->phase = LS_PHASE_SELECTING;
        s->has_sel = true;
        s->has_hover = false;
        s->sel = snap_rect(s, rc(clamp_pt(s, s->press_pt).x, clamp_pt(s, s->press_pt).y, 0, 0));
        ++s->sel_version;
        break;
    case LSK_T_RESIZE_SEL: s->drag = LSK_DRAG_RESIZE_SEL; break;
    case LSK_T_MOVE_SEL: s->drag = LSK_DRAG_MOVE_SEL; break;
    case LSK_T_NEW_SHAPE:
        // 文字、序号只认单击（松开时在按下点放置）
        if (s->tool == LS_TOOL_TEXT || s->tool == LS_TOOL_STEP) return;
        s->drag_snap = make_snapshot(s);
        create_shape_at_press(s);
        s->drag = LSK_DRAG_NEW_SHAPE;
        break;
    case LSK_T_MOVE_SHAPE:
        s->drag_snap = make_snapshot(s);
        s->drag = LSK_DRAG_MOVE_SHAPE;
        break;
    case LSK_T_RESHAPE:
        s->drag_snap = make_snapshot(s);
        s->drag = LSK_DRAG_RESHAPE;
        break;
    default: s->drag = LSK_DRAG_IGNORE; break;
    }
}

bool shape_valid(const Shape &sh) {
    if (is_rect_shape(sh)) {
        const ls_rect r = rect_from(sh.p0, sh.p1);
        return r.w >= 2 && r.h >= 2;
    }
    if (is_line_shape(sh)) return dist(sh.p0, sh.p1) >= 2;
    if (is_brush_shape(sh)) return sh.pts.size() >= 2;
    return true;
}

bool shape_changed(const Shape &a, const Shape &b) {
    if (std::memcmp(&a.p0, &b.p0, sizeof a.p0) || std::memcmp(&a.p1, &b.p1, sizeof a.p1) ||
        std::memcmp(&a.ctrl, &b.ctrl, sizeof a.ctrl) || a.pts.size() != b.pts.size())
        return true;
    for (size_t i = 0; i < a.pts.size(); ++i)
        if (a.pts[i].x != b.pts[i].x || a.pts[i].y != b.pts[i].y) return true;
    return false;
}

void finish_drag(ls_session *s) {
    switch (s->drag) {
    case LSK_DRAG_NEW_SEL: {
        const float m = s->cfg.min_selection;
        if (s->sel.w < m || s->sel.h < m) {
            reset_to_selecting(s);
        } else {
            enter_selected(s, s->sel);
        }
        break;
    }
    case LSK_DRAG_RESIZE_SEL: {
        const ls_rect r = enforce_min(s, s->sel);
        if (!rect_eq(r, s->sel)) {
            s->sel = r;
            ++s->sel_version;
        }
        break;
    }
    case LSK_DRAG_NEW_SHAPE: {
        Shape *sh = find_shape(s, s->drawing_id);
        if (sh) {
            if (!shape_valid(*sh)) {
                remove_shape(s, sh->id);
            } else {
                if (sh->type == LS_TOOL_MAGNIFY && dist(sh->p0, sh->p1) < 12) {
                    const float d = dist(sh->p0, sh->p1);
                    sh->p1 = d > 1e-3f ? pt(sh->p0.x + (sh->p1.x - sh->p0.x) * 12 / d, sh->p0.y + (sh->p1.y - sh->p0.y) * 12 / d)
                                       : pt(sh->p0.x + 12, sh->p0.y);
                }
                touch(s, *sh);
                s->merge_kind = MERGE_NONE;
                push_undo(s, std::move(s->drag_snap));
                s->selected_id = sh->id;
            }
        }
        s->drawing_id = 0;
        break;
    }
    case LSK_DRAG_MOVE_SHAPE:
    case LSK_DRAG_RESHAPE: {
        Shape *sh = find_shape(s, s->press_shape);
        if (sh && shape_changed(*sh, s->press_orig)) {
            s->merge_kind = MERGE_NONE;
            push_undo(s, std::move(s->drag_snap));
        }
        break;
    }
    default: break;
    }
    s->drag_snap = Snapshot();
}

// 右键或其他原因中止当前拖动：恢复到按下前
void abort_drag(ls_session *s) {
    switch (s->drag) {
    case LSK_DRAG_NEW_SHAPE:
        if (s->drawing_id) remove_shape(s, s->drawing_id);
        s->drawing_id = 0;
        break;
    case LSK_DRAG_MOVE_SHAPE:
    case LSK_DRAG_RESHAPE:
        if (Shape *sh = find_shape(s, s->press_shape)) {
            *sh = s->press_orig;
            touch(s, *sh);
        }
        break;
    case LSK_DRAG_RESIZE_SEL:
    case LSK_DRAG_MOVE_SEL:
        s->sel = s->press_sel;
        ++s->sel_version;
        break;
    case LSK_DRAG_NEW_SEL: reset_to_selecting(s); break;
    default: break;
    }
    s->drag_snap = Snapshot();
    s->drag = LSK_DRAG_IGNORE;
}

// ---------------------------------------------------------------- 单击放置

void place_text(ls_session *s, ls_point p) {
    Shape sh;
    sh.id = s->next_id++;
    sh.type = LS_TOOL_TEXT;
    sh.style = s->styles[LS_TOOL_TEXT];
    sh.p0 = pt(p.x - 4, p.y - sh.style.font_size * 0.7f);
    sh.p1 = sh.p0;
    s->text_snap = make_snapshot(s);
    s->text_is_new = true;
    s->text_dirty = false;
    s->text_orig.clear();
    measure_shape(s, sh);
    touch(s, sh);
    s->shapes.push_back(sh);
    s->selected_id = sh.id;
    s->editing_id = sh.id;
}

void begin_edit(ls_session *s, Shape &sh) {
    s->text_snap = make_snapshot(s);
    s->text_is_new = false;
    s->text_dirty = false;
    s->text_orig = sh.text;
    s->selected_id = sh.id;
    s->editing_id = sh.id;
    ++s->shapes_version;
}

void place_step(ls_session *s, ls_point p) {
    commit(s);
    Shape sh;
    sh.id = s->next_id++;
    sh.type = LS_TOOL_STEP;
    sh.style = s->styles[LS_TOOL_STEP];
    sh.p0 = sh.p1 = p;
    sh.number = s->styles[LS_TOOL_STEP].next_number;
    touch(s, sh);
    s->shapes.push_back(sh);
    s->selected_id = sh.id;
    if (s->styles[LS_TOOL_STEP].next_number < 999) {
        ++s->styles[LS_TOOL_STEP].next_number;
        ++s->style_version;
    }
}

void place_magnify(ls_session *s, ls_point p) {
    commit(s);
    Shape sh;
    sh.id = s->next_id++;
    sh.type = LS_TOOL_MAGNIFY;
    sh.style = s->styles[LS_TOOL_MAGNIFY];
    sh.p0 = p;
    sh.p1 = pt(p.x + 60, p.y);
    touch(s, sh);
    s->shapes.push_back(sh);
    s->selected_id = sh.id;
}

int handle_click(ls_session *s) {
    const ls_point p = s->press_pt;
    const bool shift = (s->press_mods & LS_MOD_SHIFT) != 0;
    if (s->phase == LS_PHASE_SELECTING) {
        if (shift) {
            if (s->has_hover && s->hover_is_window) toggle_window(s, s->hover_id);
            return 0;
        }
        select_hover(s);
        return 0;
    }
    if (!s->multi.empty() && shift) {
        ls_rect r;
        int64_t id = 0;
        if (window_at(s, p.x, p.y, &r, &id)) toggle_window(s, id);
        return 0;
    }
    switch (s->press_target) {
    case LSK_T_NEW_SHAPE:
        if (s->tool == LS_TOOL_TEXT) {
            place_text(s, p);
            return LS_EV_TEXT_BEGIN;
        }
        if (s->tool == LS_TOOL_STEP) {
            place_step(s, p);
            return 0;
        }
        if (s->tool == LS_TOOL_MAGNIFY) {
            place_magnify(s, p);
            return 0;
        }
        s->selected_id = 0;
        return 0;
    case LSK_T_MOVE_SHAPE:
    case LSK_T_RESHAPE: return 0;
    default: s->selected_id = 0; return 0;
    }
}

// ---------------------------------------------------------------- 样式

void clamp_style(ls_style &st) {
    st.opacity = clampf(st.opacity, 0.05f, 1.0f);
    if (!(st.line_width >= 0.5f)) st.line_width = 0.5f;
    if (!(st.font_size >= 1)) st.font_size = 1;
    if (!(st.block_size >= 1)) st.block_size = 1;
    if (!(st.zoom >= 1)) st.zoom = 1;
    if (st.corner_radius < 0) st.corner_radius = 0;
    st.next_number = st.next_number < 1 ? 1 : (st.next_number > 999 ? 999 : st.next_number);
}

int32_t style_diff_mask(const ls_style &a, const ls_style &b) {
    int32_t m = 0;
    if (std::memcmp(&a.color, &b.color, sizeof a.color)) m |= 1;
    if (a.line_width != b.line_width) m |= 2;
    if (a.opacity != b.opacity) m |= 4;
    if (a.filled != b.filled || a.variant != b.variant || a.bold != b.bold) m |= 8;
    if (a.corner_radius != b.corner_radius) m |= 16;
    if (a.font_size != b.font_size || a.block_size != b.block_size || a.zoom != b.zoom) m |= 32;
    return m;
}

// 把工具样式套到已有标注上（保留标注自己的数字、马赛克几何类型）。返回新样式（未写入）
ls_style style_for_shape(const Shape &sh, const ls_style &st) {
    ls_style ns = st;
    ns.next_number = sh.style.next_number;
    if (sh.type == LS_TOOL_MOSAIC) {
        const bool was_brush = sh.style.variant == 1 || sh.style.variant == 3;
        const bool now_brush = st.variant == 1 || st.variant == 3;
        if (was_brush != now_brush) ns.variant = sh.style.variant;
    }
    return ns;
}

void apply_style(ls_session *s, Shape &sh, const ls_style &ns) {
    const bool to_curve = sh.type == LS_TOOL_ARROW && ns.variant == 3 && sh.style.variant != 3;
    sh.style = ns;
    if (to_curve) default_ctrl(sh);
    if (sh.type == LS_TOOL_TEXT) measure_shape(s, sh);
    touch(s, sh);
}

// 改工具样式，同时作用于选中的同类标注
int set_style_internal(ls_session *s, int tool, const ls_style &st, int merge) {
    int ev = 0;
    s->styles[tool] = st;
    ++s->style_version;
    Shape *sh = find_shape(s, s->selected_id);
    if (sh && sh->type == tool && !(s->pressed && s->drag != LSK_DRAG_PENDING)) {
        const ls_style ns = style_for_shape(*sh, st);
        const int32_t mask = style_diff_mask(ns, sh->style);
        if (mask) {
            if (sh->id == s->editing_id) {
                s->text_dirty = true; // 编辑中的文字：结束编辑时一起记一条撤销
                ev |= LS_EV_TEXT_BEGIN;
            } else {
                commit_merged(s, merge, sh->id, mask);
            }
            apply_style(s, *sh, ns);
        }
    }
    return ev;
}

void wheel_style(ls_session *s, int steps) {
    const int tool = s->tool;
    ls_style st = s->styles[tool];
    switch (tool) {
    case LS_TOOL_RECT:
    case LS_TOOL_ELLIPSE:
    case LS_TOOL_LINE:
    case LS_TOOL_ARROW:
    case LS_TOOL_PEN: st.line_width = clampf(std::floor(st.line_width + 0.5f) + (float)steps, 1, 30); break;
    case LS_TOOL_MOSAIC:
        if (st.variant == 1 || st.variant == 3)
            st.line_width = clampf(std::floor(st.line_width + 0.5f) + 2.0f * steps, 4, 120);
        else
            st.block_size = clampf(std::floor(st.block_size + 0.5f) + (float)steps, 2, 60);
        break;
    case LS_TOOL_TEXT: st.font_size = clampf(std::floor(st.font_size + 0.5f) + (float)steps, 8, 200); break;
    case LS_TOOL_STEP: st.next_number = std::min(999, std::max(1, st.next_number + steps)); break;
    case LS_TOOL_MAGNIFY: st.zoom = clampf(std::floor(st.zoom * 2 + 0.5f) / 2 + 0.5f * steps, 1.5f, 6); break;
    case LS_TOOL_HIGHLIGHT: st.opacity = clampf(round2(st.opacity + 0.05f * steps), 0.1f, 0.95f); break;
    default: return;
    }
    if (std::memcmp(&st, &s->styles[tool], sizeof st) == 0) return;
    set_style_internal(s, tool, st, MERGE_WHEEL_STYLE);
}

// ---------------------------------------------------------------- 事件掩码

struct Before {
    int cursor, phase, tool, drag, ratio_index;
    bool has_sel, has_hover, toolbar, inside, magnifier, can_undo, can_redo;
    ls_rect sel, hover;
    size_t wcount, nactions;
    uint64_t style_version, shapes_version, sel_version;
    int32_t selected, hovered, editing;
    float mx, my;
};

bool magnifier_on(const ls_session *s) {
    return (s->phase == LS_PHASE_SELECTING && s->inside && s->drag != LSK_DRAG_NEW_SEL) || s->drag == LSK_DRAG_NEW_SEL ||
           s->drag == LSK_DRAG_RESIZE_SEL;
}

Before take(const ls_session *s) {
    Before b;
    b.cursor = s->cursor;
    b.phase = s->phase;
    b.tool = s->tool;
    b.drag = s->drag;
    b.ratio_index = s->ratio_index;
    b.has_sel = s->has_sel;
    b.has_hover = s->has_hover;
    b.toolbar = s->toolbar_visible;
    b.inside = s->inside;
    b.magnifier = magnifier_on(s);
    b.can_undo = !s->undo_stack.empty();
    b.can_redo = !s->redo_stack.empty();
    b.sel = s->sel;
    b.hover = s->hover;
    b.wcount = s->multi.size();
    b.nactions = s->actions.size();
    b.style_version = s->style_version;
    b.shapes_version = s->shapes_version;
    b.sel_version = s->sel_version;
    b.selected = s->selected_id;
    b.hovered = s->hovered_id;
    b.editing = s->editing_id;
    b.mx = s->mx;
    b.my = s->my;
    return b;
}

int32_t changes(ls_session *s, const Before &b, int extra) {
    refresh(s);
    int32_t ev = extra;
    if (s->cursor != b.cursor) ev |= LS_EV_CURSOR;
    if (s->phase != b.phase || s->has_sel != b.has_sel || !rect_eq(s->sel, b.sel) || s->multi.size() != b.wcount)
        ev |= LS_EV_SELECTION | LS_EV_REDRAW;
    if (s->tool != b.tool || s->style_version != b.style_version) ev |= LS_EV_TOOL | LS_EV_REDRAW;
    if (!s->undo_stack.empty() != b.can_undo || !s->redo_stack.empty() != b.can_redo) ev |= LS_EV_HISTORY;
    if (s->actions.size() > b.nactions) ev |= LS_EV_ACTION;
    const bool mag = magnifier_on(s);
    if (s->has_hover != b.has_hover || (s->has_hover && !rect_eq(s->hover, b.hover)) || s->shapes_version != b.shapes_version ||
        s->selected_id != b.selected || s->hovered_id != b.hovered || s->editing_id != b.editing ||
        s->toolbar_visible != b.toolbar || s->drag != b.drag || s->ratio_index != b.ratio_index || mag != b.magnifier ||
        s->inside != b.inside)
        ev |= LS_EV_REDRAW;
    if ((s->mx != b.mx || s->my != b.my) && (mag || b.magnifier || s->phase == LS_PHASE_SELECTING)) ev |= LS_EV_REDRAW;
    return ev;
}

void push_action(ls_session *s, int a) { s->actions.push_back(a); }

// ---------------------------------------------------------------- 键盘

void nudge(ls_session *s, int key, int mods) {
    const float step = ((mods & LS_MOD_ALT) ? 10.0f : 1.0f) / s->scale;
    float dx = 0, dy = 0;
    switch (key) {
    case LS_KEY_LEFT: dx = -step; break;
    case LS_KEY_RIGHT: dx = step; break;
    case LS_KEY_UP: dy = -step; break;
    default: dy = step; break;
    }
    Shape *sh = find_shape(s, s->selected_id);
    if (sh && sh->id != s->editing_id) {
        commit_merged(s, MERGE_NUDGE, sh->id, 0);
        const Shape o = *sh;
        translate_shape(*sh, o, dx, dy);
        touch(s, *sh);
        return;
    }
    if (!s->has_sel || sel_locked(s)) return;
    ls_rect r = s->sel;
    if (mods & LS_MOD_SHIFT) {
        // 右/下边伸缩
        const float m = std::max(1 / s->scale, s->cfg.min_selection);
        r.w = clampf(r.w + dx, std::min(m, s->W - r.x), s->W - r.x);
        r.h = clampf(r.h + dy, std::min(m, s->H - r.y), s->H - r.y);
    } else {
        r.x = clampf(r.x + dx, 0, s->W - r.w);
        r.y = clampf(r.y + dy, 0, s->H - r.h);
    }
    r = snap_rect(s, r);
    if (!rect_eq(r, s->sel)) {
        s->sel = r;
        ++s->sel_version;
    }
}

int set_tool_internal(ls_session *s, int tool) {
    if (tool < 0 || tool >= LS_TOOL_COUNT) return 0;
    if (tool != LS_TOOL_NONE && !s->multi.empty()) return 0; // 多选窗口模式下标注工具无效
    int ev = 0;
    if (s->editing_id) ev |= end_text_internal(s);
    s->tool = s->tool == tool ? LS_TOOL_NONE : tool;
    s->wheel_kind = 0;
    s->wheel_acc = 0;
    return ev;
}

int handle_binding(ls_session *s, int b) {
    const bool selected = s->phase == LS_PHASE_SELECTED;
    const bool locked = s->cfg.lock_selection != 0;
    const bool multi = !s->multi.empty();
    switch (b) {
    case LS_BIND_COPY_RGB: push_action(s, LS_ACTION_COPY_RGB); return 0;
    case LS_BIND_COPY_HEX: push_action(s, LS_ACTION_COPY_HEX); return 0;
    case LS_BIND_COPY_CUSTOM: push_action(s, LS_ACTION_COPY_CUSTOM); return 0;
    case LS_BIND_FULLSCREEN:
        if (locked || !s->shapes.empty() || s->pressed) return 0;
        s->multi.clear();
        enter_selected(s, full_screen(s));
        return 0;
    case LS_BIND_LAST_AREA:
        if (locked || !s->shapes.empty() || s->pressed) return 0;
        // 上次区域不在本屏（夹取后太小）按没有处理
        if (s->has_last && clamp_rect(s, s->last_area).w >= s->cfg.min_selection &&
            clamp_rect(s, s->last_area).h >= s->cfg.min_selection) {
            set_selection_internal(s, clamp_rect(s, s->last_area));
        } else {
            push_action(s, LS_ACTION_LAST_AREA);
        }
        return 0;
    default: break;
    }
    if (!selected) return 0;
    if (b >= LS_TOOL_RECT && b <= LS_TOOL_MAGNIFY) {
        if (s->pressed) return 0;
        return set_tool_internal(s, b);
    }
    switch (b) {
    case LS_BIND_LONGSHOT:
        if (!multi && !locked) push_action(s, LS_ACTION_LONGSHOT);
        break;
    case LS_BIND_RECORD:
        if (!multi && !locked) push_action(s, LS_ACTION_RECORD);
        break;
    case LS_BIND_OCR: push_action(s, LS_ACTION_OCR); break;
    case LS_BIND_TRANSLATE: push_action(s, LS_ACTION_TRANSLATE); break;
    case LS_BIND_PIN: push_action(s, LS_ACTION_PIN); break;
    case LS_BIND_TOGGLE_TOOLBAR: s->toolbar_visible = !s->toolbar_visible; break;
    default: break;
    }
    return 0;
}

int undo_internal(ls_session *s, bool redo) {
    if (s->pressed && s->drag != LSK_DRAG_PENDING && s->drag != LSK_DRAG_IGNORE) return 0;
    int ev = 0;
    if (s->editing_id) ev |= end_text_internal(s);
    std::vector<Snapshot> &from = redo ? s->redo_stack : s->undo_stack;
    std::vector<Snapshot> &to = redo ? s->undo_stack : s->redo_stack;
    if (from.empty()) return ev;
    Snapshot target = std::move(from.back());
    from.pop_back();
    to.push_back(make_snapshot(s));
    if (to.size() > kMaxUndo) to.erase(to.begin());
    restore(s, std::move(target));
    break_merge(s);
    return ev;
}

}  // namespace

// ================================================================== 对外接口

extern "C" {

ls_session_config ls_session_default_config(void) {
    ls_session_config c;
    std::memset(&c, 0, sizeof c);
    c.scale = 1;
    c.detect_windows = 1;
    c.handle_hit = 14;
    c.edge_hit = 6;
    c.min_selection = 4;
    return c;
}

ls_session *ls_session_create(const ls_session_config *config) {
    ls_session *s = new ls_session();
    s->cfg = config ? *config : ls_session_default_config();
    s->W = s->cfg.width > 0 ? s->cfg.width : 0;
    s->H = s->cfg.height > 0 ? s->cfg.height : 0;
    s->scale = s->cfg.scale > 0 ? s->cfg.scale : 1;
    if (s->cfg.handle_hit <= 0) s->cfg.handle_hit = 14;
    if (s->cfg.edge_hit <= 0) s->cfg.edge_hit = 6;
    if (s->cfg.min_selection <= 0) s->cfg.min_selection = 4;
    default_styles(s->styles);
    default_bindings(s->bindings);
    s->tiles.base = &s->cfg.base;
    if (s->cfg.lock_selection) {
        const ls_rect r = s->cfg.has_initial_selection ? s->cfg.initial_selection : rc(0, 0, s->W, s->H);
        enter_selected(s, snap_rect(s, clamp_rect(s, r)));
    } else if (s->cfg.has_initial_selection) {
        enter_selected(s, snap_rect(s, clamp_rect(s, s->cfg.initial_selection)));
    }
    s->cursor = s->phase == LS_PHASE_SELECTING ? LS_CURSOR_CROSSHAIR : LS_CURSOR_ARROW;
    return s;
}

void ls_session_destroy(ls_session *s) { delete s; }

void ls_session_set_windows(ls_session *s, const ls_window *windows, int32_t count) {
    if (!s) return;
    s->windows.assign(windows, windows + (count > 0 && windows ? count : 0));
    refresh(s);
}

void ls_session_set_last_area(ls_session *s, ls_rect rect) {
    if (!s) return;
    s->has_last = rect.w > 0 && rect.h > 0;
    s->last_area = rect;
}

void ls_session_bind_key(ls_session *s, ls_binding binding, int32_t key) {
    if (!s || binding <= 0 || binding >= LS_BIND_MAX) return;
    s->bindings[binding] = key;
}

int32_t ls_session_mouse_down(ls_session *s, float x, float y, int32_t button, int32_t mods, int32_t click_count) {
    if (!s) return 0;
    const Before b = take(s);
    int extra = 0;
    s->mx = x;
    s->my = y;
    s->inside = true;
    s->mods = mods;

    if (button == LS_BUTTON_RIGHT) {
        if (s->editing_id) {
            extra |= end_text_internal(s);
            return changes(s, b, extra);
        }
        if (s->phase == LS_PHASE_SELECTING) {
            push_action(s, LS_ACTION_CANCEL);
            return changes(s, b, extra);
        }
        if (s->pressed) {
            // 正在画：放弃这一笔；正在拖选区或标注：恢复原状
            if (s->drag != LSK_DRAG_PENDING) abort_drag(s);
            s->drag = LSK_DRAG_IGNORE;
            return changes(s, b, extra);
        }
        if (s->cfg.right_click_exits || s->cfg.lock_selection) {
            push_action(s, LS_ACTION_CANCEL);
            return changes(s, b, extra);
        }
        // 清空选区和全部标注，回到 SELECTING
        s->shapes.clear();
        ++s->shapes_version;
        gc_geoms(s);
        s->undo_stack.clear();
        s->redo_stack.clear();
        s->selected_id = s->hovered_id = 0;
        s->tool = LS_TOOL_NONE;
        s->ratio_index = 0;
        break_merge(s);
        reset_to_selecting(s);
        return changes(s, b, extra);
    }
    if (button != LS_BUTTON_LEFT || s->pressed) return changes(s, b, extra);

    break_merge(s);
    s->cursor = compute_cursor(s); // 按下点的光标（按下前可能没有移动事件）
    s->pressed = true;
    s->press_pt = pt(x, y);
    s->press_mods = mods;
    s->press_cursor = s->cursor;
    s->press_handle = -1;
    s->press_shape = 0;
    s->drag = LSK_DRAG_PENDING;

    if (s->editing_id) {
        // 正在编辑文字时点别处：只结束编辑
        extra |= end_text_internal(s);
        s->drag = LSK_DRAG_IGNORE;
        return changes(s, b, extra);
    }
    if (s->phase == LS_PHASE_SELECTING) {
        // 按下前没有移动事件时，按下点的悬停也要算对（单击要用它）
        s->pressed = false;
        update_hover(s);
        s->pressed = true;
        s->press_target = LSK_T_NEW_SEL;
        s->ratio_index = 0;
        return changes(s, b, extra);
    }

    const Hit h = hit_test(s, x, y);
    if (click_count >= 2) {
        if (h.kind == HIT_SHAPE) {
            Shape *sh = find_shape(s, h.shape);
            if (sh && sh->type == LS_TOOL_TEXT) {
                begin_edit(s, *sh);
                s->drag = LSK_DRAG_IGNORE;
                return changes(s, b, extra | LS_EV_TEXT_BEGIN);
            }
        }
        if (s->tool == LS_TOOL_NONE && h.kind == HIT_INSIDE) {
            push_action(s, LS_ACTION_DOUBLE_CLICK);
            s->drag = LSK_DRAG_IGNORE;
            return changes(s, b, extra);
        }
    }
    switch (h.kind) {
    case HIT_SEL_HANDLE:
    case HIT_SEL_EDGE:
        s->press_target = LSK_T_RESIZE_SEL;
        s->press_handle = h.handle;
        s->press_sel = s->sel;
        s->ratio_index = 0;
        break;
    case HIT_SHAPE_HANDLE:
        s->press_target = LSK_T_RESHAPE;
        s->press_handle = h.handle;
        s->press_shape = h.shape;
        if (const Shape *sh = find_shape(s, h.shape)) s->press_orig = *sh;
        break;
    case HIT_SHAPE:
        s->press_target = LSK_T_MOVE_SHAPE;
        s->press_shape = h.shape;
        s->selected_id = h.shape;
        if (const Shape *sh = find_shape(s, h.shape)) s->press_orig = *sh;
        break;
    case HIT_INSIDE:
        if (!s->multi.empty())
            s->press_target = LSK_T_CLICK;
        else if (s->tool == LS_TOOL_NONE)
            s->press_target = s->cfg.lock_selection ? LSK_T_CLICK : LSK_T_MOVE_SEL;
        else
            s->press_target = LSK_T_NEW_SHAPE;
        s->press_sel = s->sel;
        break;
    default:
        s->press_target = (s->shapes.empty() && !s->cfg.lock_selection) ? LSK_T_NEW_SEL : LSK_T_CLICK;
        if (s->press_target == LSK_T_NEW_SEL) s->ratio_index = 0;
        break;
    }
    return changes(s, b, extra);
}

int32_t ls_session_mouse_move(ls_session *s, float x, float y, int32_t mods) {
    if (!s) return 0;
    const Before b = take(s);
    s->mx = x;
    s->my = y;
    s->inside = true;
    s->mods = mods;
    if (s->pressed) {
        if (s->drag == LSK_DRAG_PENDING && dist(pt(x, y), s->press_pt) > kDragThreshold) start_drag(s);
        if (s->drag != LSK_DRAG_PENDING && s->drag != LSK_DRAG_IGNORE) update_drag(s);
    }
    return changes(s, b, 0);
}

int32_t ls_session_mouse_up(ls_session *s, float x, float y, int32_t button, int32_t mods) {
    if (!s) return 0;
    const Before b = take(s);
    int extra = 0;
    s->mx = x;
    s->my = y;
    s->mods = mods;
    if (button != LS_BUTTON_LEFT || !s->pressed) return changes(s, b, 0);
    if (s->drag != LSK_DRAG_PENDING && s->drag != LSK_DRAG_IGNORE) update_drag(s);
    s->pressed = false;
    if (s->drag == LSK_DRAG_PENDING)
        extra |= handle_click(s);
    else
        finish_drag(s);
    s->drag = LSK_DRAG_NONE;
    return changes(s, b, extra);
}

int32_t ls_session_scroll(ls_session *s, float x, float y, float dy, int32_t mods) {
    if (!s) return 0;
    const Before b = take(s);
    s->mx = x;
    s->my = y;
    s->inside = true;
    s->mods = mods;
    if (s->phase != LS_PHASE_SELECTED || s->editing_id || s->pressed || dy == 0 || !s->multi.empty()) return changes(s, b, 0);
    const int32_t sid = shape_under(s, x, y);
    int kind;
    int32_t target;
    if (sid) {
        kind = 1;
        target = sid;
    } else if (s->tool != LS_TOOL_NONE) {
        kind = 2;
        target = s->tool;
    } else {
        return changes(s, b, 0);
    }
    if (kind != s->wheel_kind || target != s->wheel_id) {
        s->wheel_kind = kind;
        s->wheel_id = target;
        s->wheel_acc = 0;
        s->merge_kind = MERGE_NONE;
    }
    s->wheel_acc += dy;
    const int steps = (int)s->wheel_acc; // 向零取整，余数留到下一次
    s->wheel_acc -= (float)steps;
    if (!steps) return changes(s, b, 0);
    int extra = 0;
    if (kind == 1) {
        Shape *sh = find_shape(s, sid);
        if (sh) {
            const bool hl = sh->type == LS_TOOL_HIGHLIGHT;
            const float v = clampf(round2(sh->style.opacity + 0.05f * steps), hl ? 0.1f : 0.05f, hl ? 0.95f : 1.0f);
            if (v != sh->style.opacity) {
                commit_merged(s, MERGE_WHEEL_SHAPE, sh->id, 0);
                sh->style.opacity = v;
                touch(s, *sh);
            }
        }
    } else {
        wheel_style(s, steps);
    }
    return changes(s, b, extra);
}

int32_t ls_session_key_down(ls_session *s, int32_t key, int32_t mods) {
    if (!s) return 0;
    const Before b = take(s);
    int extra = 0;
    s->mods = mods;
    if (s->editing_id) {
        // 文字由原生输入框处理；万一转发过来，Esc 只结束编辑
        if (key == LS_KEY_ESCAPE) extra |= end_text_internal(s);
        return changes(s, b, extra);
    }
    const bool primary = (mods & LS_MOD_PRIMARY) != 0;
    const bool plain = (mods & (LS_MOD_CTRL | LS_MOD_ALT | LS_MOD_CMD | LS_MOD_PRIMARY)) == 0;
    const bool selected = s->phase == LS_PHASE_SELECTED;
    const bool sel_drag = s->pressed && (s->press_target == LSK_T_NEW_SEL || s->press_target == LSK_T_RESIZE_SEL) &&
                          s->drag != LSK_DRAG_IGNORE;
    if (key != LS_KEY_LEFT && key != LS_KEY_RIGHT && key != LS_KEY_UP && key != LS_KEY_DOWN) s->merge_kind = MERGE_NONE;

    if (key == LS_KEY_ESCAPE) {
        push_action(s, LS_ACTION_CANCEL);
    } else if (key == LS_KEY_ENTER) {
        if (!selected && !s->pressed) select_hover(s);
        if (s->phase == LS_PHASE_SELECTED) push_action(s, LS_ACTION_DONE);
    } else if (sel_drag && key >= '0' && key <= '9') {
        // 拖出或拉伸选区时数字键锁比例
        s->ratio_index = key - '0';
        if (s->drag != LSK_DRAG_PENDING) update_drag(s);
    } else if (primary) {
        if (!selected) return changes(s, b, extra);
        if (key == 'C') {
            push_action(s, LS_ACTION_COPY);
        } else if (key == 'S') {
            push_action(s, LS_ACTION_SAVE);
        } else if (key == 'Z') {
            extra |= undo_internal(s, (mods & LS_MOD_SHIFT) != 0);
        } else if (key == 'Y') {
            extra |= undo_internal(s, true);
        }
    } else if (selected && (key == LS_KEY_DELETE || key == LS_KEY_BACKSPACE)) {
        if (s->selected_id && !s->pressed) {
            commit(s);
            remove_shape(s, s->selected_id);
        }
    } else if (selected && (key == LS_KEY_LEFT || key == LS_KEY_RIGHT || key == LS_KEY_UP || key == LS_KEY_DOWN)) {
        if (!s->pressed) nudge(s, key, mods);
    } else if (plain || (mods & ~LS_MOD_SHIFT) == 0) {
        for (int bnd = 1; bnd < LS_BIND_MAX; ++bnd) {
            if (s->bindings[bnd] == key && key != 0) {
                extra |= handle_binding(s, bnd);
                break;
            }
        }
    }
    return changes(s, b, extra);
}

int32_t ls_session_key_up(ls_session *s, int32_t key, int32_t mods) {
    if (!s) return 0;
    (void)key;
    const Before b = take(s);
    s->mods = mods;
    if (s->pressed && s->drag != LSK_DRAG_PENDING && s->drag != LSK_DRAG_IGNORE) update_drag(s);
    return changes(s, b, 0);
}

int32_t ls_session_modifiers(ls_session *s, int32_t mods) {
    if (!s) return 0;
    const Before b = take(s);
    s->mods = mods;
    if (s->pressed && s->drag != LSK_DRAG_PENDING && s->drag != LSK_DRAG_IGNORE) update_drag(s);
    return changes(s, b, 0);
}

int32_t ls_session_mouse_exit(ls_session *s) {
    if (!s) return 0;
    const Before b = take(s);
    s->inside = false;
    s->has_hover = false;
    s->hovered_id = 0;
    return changes(s, b, 0);
}

int32_t ls_session_set_tool(ls_session *s, int32_t tool) {
    if (!s) return 0;
    const Before b = take(s);
    const int extra = set_tool_internal(s, tool);
    return changes(s, b, extra);
}

int32_t ls_session_get_tool(const ls_session *s) { return s ? s->tool : 0; }

void ls_session_get_style(const ls_session *s, int32_t tool, ls_style *out) {
    if (!out) return;
    if (!s || tool < 0 || tool >= LS_TOOL_COUNT) {
        std::memset(out, 0, sizeof *out);
        return;
    }
    *out = s->styles[tool];
}

int32_t ls_session_set_style(ls_session *s, int32_t tool, const ls_style *style) {
    if (!s || !style || tool <= LS_TOOL_NONE || tool >= LS_TOOL_COUNT) return 0;
    const Before b = take(s);
    ls_style st = *style;
    clamp_style(st);
    const int extra = set_style_internal(s, tool, st, MERGE_STYLE);
    return changes(s, b, extra);
}

int32_t ls_session_undo(ls_session *s) {
    if (!s) return 0;
    const Before b = take(s);
    const int extra = undo_internal(s, false);
    return changes(s, b, extra);
}

int32_t ls_session_redo(ls_session *s) {
    if (!s) return 0;
    const Before b = take(s);
    const int extra = undo_internal(s, true);
    return changes(s, b, extra);
}

int32_t ls_session_delete_selected(ls_session *s) {
    if (!s) return 0;
    const Before b = take(s);
    int extra = 0;
    if (s->selected_id && !(s->pressed && s->drag != LSK_DRAG_PENDING)) {
        if (s->editing_id == s->selected_id) {
            // 删掉正在编辑的文字：先结束编辑（会提交），再删
            extra |= end_text_internal(s);
        }
        if (find_shape(s, s->selected_id)) {
            commit(s);
            remove_shape(s, s->selected_id);
        }
    }
    return changes(s, b, extra);
}

int32_t ls_session_set_selection(ls_session *s, ls_rect rect) {
    if (!s || s->cfg.lock_selection || s->pressed) return 0;
    const Before b = take(s);
    set_selection_internal(s, rect);
    return changes(s, b, 0);
}

int32_t ls_session_select_fullscreen(ls_session *s) {
    if (!s) return 0;
    return ls_session_set_selection(s, rc(0, 0, s->W, s->H));
}

int32_t ls_session_set_toolbar_visible(ls_session *s, int32_t visible) {
    if (!s) return 0;
    const Before b = take(s);
    s->toolbar_visible = visible != 0;
    return changes(s, b, 0);
}

ls_action ls_session_take_action(ls_session *s) {
    if (!s || s->actions.empty()) return LS_ACTION_NONE;
    const int a = s->actions.front();
    s->actions.pop_front();
    return (ls_action)a;
}

void ls_session_get_state(const ls_session *s, ls_session_state *out) {
    if (!out) return;
    std::memset(out, 0, sizeof *out);
    if (!s) return;
    out->phase = s->phase;
    out->has_selection = s->has_sel;
    out->selection = s->sel;
    out->has_hover = s->has_hover && s->phase == LS_PHASE_SELECTING;
    out->hover = s->hover;
    out->dragging_selection = s->drag == LSK_DRAG_NEW_SEL || s->drag == LSK_DRAG_RESIZE_SEL || s->drag == LSK_DRAG_MOVE_SEL;
    out->dragging_shape = s->drag == LSK_DRAG_NEW_SHAPE || s->drag == LSK_DRAG_MOVE_SHAPE || s->drag == LSK_DRAG_RESHAPE;
    out->mouse_x = s->mx;
    out->mouse_y = s->my;
    out->mouse_inside = s->inside;
    out->cursor = s->cursor;
    out->tool = s->tool;
    out->show_magnifier = magnifier_on(s);
    out->ratio_index = s->ratio_index;
    out->ratio = kRatios[s->ratio_index];
    out->toolbar_visible = s->toolbar_visible;
    out->can_undo = !s->undo_stack.empty();
    out->can_redo = !s->redo_stack.empty();
    out->selected_shape = s->selected_id;
    out->editing_text = s->editing_id;
    out->window_count = (int32_t)s->multi.size();
    out->shape_count = (int32_t)s->shapes.size();
    if (s->sel_window && s->has_sel && s->phase == LS_PHASE_SELECTED && s->multi.empty() && rect_eq(s->sel, s->sel_window_rect))
        out->selection_window = s->sel_window;
}

void ls_session_selection_pixels(const ls_session *s, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (s && s->has_sel) {
        x0 = (int32_t)std::floor(s->sel.x * s->scale + 0.5f);
        y0 = (int32_t)std::floor(s->sel.y * s->scale + 0.5f);
        x1 = (int32_t)std::floor((s->sel.x + s->sel.w) * s->scale + 0.5f);
        y1 = (int32_t)std::floor((s->sel.y + s->sel.h) * s->scale + 0.5f);
    }
    if (x) *x = x0;
    if (y) *y = y0;
    if (w) *w = x1 - x0;
    if (h) *h = y1 - y0;
}

int32_t ls_session_selection_grip_at(const ls_session *s, float x, float y) {
    if (!s || s->phase != LS_PHASE_SELECTED || !s->has_sel || sel_locked(s)) return -1;
    const Hit h = hit_test(s, x, y);
    return h.kind == HIT_SEL_HANDLE || h.kind == HIT_SEL_EDGE ? h.handle : -1;
}

int32_t ls_session_selected_windows(const ls_session *s, int64_t *ids, int32_t capacity) {
    if (!s) return 0;
    const int32_t n = (int32_t)s->multi.size();
    for (int32_t i = 0; i < n && i < capacity && ids; ++i) ids[i] = s->multi[(size_t)i];
    return n;
}

ls_color ls_session_pixel_at(const ls_session *s, float x, float y) {
    ls_color c{0, 0, 0, 0};
    if (!s || !s->cfg.base.data || s->cfg.base.width <= 0 || s->cfg.base.height <= 0) return c;
    const int px = clampi((int)std::floor(x * s->scale), 0, s->cfg.base.width - 1);
    const int py = clampi((int)std::floor(y * s->scale), 0, s->cfg.base.height - 1);
    const uint8_t *p = row_ptr(&s->cfg.base, py) + px * 4;
    const uint32_t a = p[3];
    c.a = (uint8_t)a;
    if (a == 0) return c;
    auto un = [a](uint32_t v) { return (uint8_t)std::min(255u, a == 255 ? v : (v * 255 + a / 2) / a); };
    c.b = un(p[0]);
    c.g = un(p[1]);
    c.r = un(p[2]);
    return c;
}

// ---------------------------------------------------------------- 摆放

ls_rect ls_session_place_toolbar(const ls_session *s, float w, float h, float gap, float margin) {
    if (!s) return rc(0, 0, w, h);
    const ls_rect r = s->has_sel ? s->sel : rc(0, 0, s->W, s->H);
    float x = r.x + r.w - w;
    float y = r.y + r.h + gap;
    if (y + h > s->H - margin) {
        y = r.y - gap - h;
        if (y < margin) y = r.y + r.h - h - gap;
    }
    x = std::max(margin, std::min(x, s->W - w - margin));
    return rc(x, y, w, h);
}

ls_rect ls_session_place_size_tip(const ls_session *s, float w, float h, float gap) {
    if (!s) return rc(0, 0, w, h);
    float x, y;
    if (s->has_sel) {
        x = s->sel.x;
        y = s->sel.y - h - gap;
        if (y < gap) {
            x = s->sel.x + gap;
            y = s->sel.y + gap;
        }
    } else {
        // 没有选区：光标右上方，放不下时放右下方
        x = s->mx + gap;
        y = s->my - h - gap;
        if (y < 0) y = s->my + gap;
    }
    x = std::max(0.0f, std::min(x, s->W - w));
    return rc(x, y, w, h);
}

ls_rect ls_session_place_magnifier(const ls_session *s, float w, float h, float offset) {
    if (!s) return rc(0, 0, w, h);
    float x = s->mx + offset, y = s->my + offset;
    if (x + w > s->W) x = s->mx - offset - w;
    if (y + h > s->H) y = s->my - offset - h;
    return rc(x, y, w, h);
}

ls_rect ls_session_place_popover(const ls_session *s, ls_rect toolbar, float anchor_x, float w, float h, float gap, float margin) {
    if (!s) return rc(anchor_x - 16, toolbar.y + toolbar.h + gap, w, h);
    float x = std::max(margin, std::min(anchor_x - 16, s->W - w - margin));
    const bool above = s->has_sel && toolbar.y + toolbar.h <= s->sel.y + 0.5f;
    float y;
    if (!above) {
        y = toolbar.y + toolbar.h + gap;
        if (y + h > s->H - margin) y = toolbar.y - gap - h;
    } else {
        y = toolbar.y - gap - h;
        if (y < margin) y = toolbar.y + toolbar.h + gap;
    }
    return rc(x, y, w, h);
}

// ---------------------------------------------------------------- 绘制数据

static bool has_highlight(const ls_session *s) {
    if (!s->has_sel) return false;
    for (const Shape &sh : s->shapes)
        if (sh.type == LS_TOOL_HIGHLIGHT) return true;
    return false;
}

int32_t ls_session_shape_count(const ls_session *s) {
    if (!s) return 0;
    return (int32_t)s->shapes.size() + (has_highlight(s) ? 1 : 0);
}

void ls_session_shape_at(const ls_session *s, int32_t index, ls_shape_view *out) {
    if (!out) return;
    std::memset(out, 0, sizeof *out);
    if (!s || index < 0) return;
    if (has_highlight(s)) {
        if (index == 0) {
            if (s->mask_shapes_version != s->shapes_version || s->mask_sel_version != s->sel_version) {
                build_mask_geom(s->sel, s->shapes, s->mask_geom);
                s->mask_shapes_version = s->shapes_version;
                s->mask_sel_version = s->sel_version;
            }
            out->type = LS_SHAPE_MASK;
            out->style = s->mask_geom.vstyle;
            out->bounds = s->mask_geom.bounds;
            out->p0 = pt(s->sel.x, s->sel.y);
            out->p1 = pt(s->sel.x + s->sel.w, s->sel.y + s->sel.h);
            out->fill = &s->mask_geom.fill;
            out->fill_color = s->mask_geom.fill_color;
            return;
        }
        --index;
    }
    if ((size_t)index >= s->shapes.size()) return;
    const Shape &sh = s->shapes[(size_t)index];
    std::unique_ptr<ShapeGeom> &slot = s->geoms[sh.id];
    if (!slot) slot.reset(new ShapeGeom());
    ShapeGeom &g = *slot;
    if (!g.valid || g.rev != sh.rev) {
        build_geom(sh, g);
        if (sh.type == LS_TOOL_MOSAIC) build_patch(sh, g, s->tiles, s->scale, s->drawing_id == sh.id);
        g.rev = sh.rev;
        g.valid = true;
    }
    out->id = sh.id;
    out->type = sh.type;
    out->selected = sh.id == s->selected_id;
    out->hovered = sh.id == s->hovered_id;
    out->editing = sh.id == s->editing_id;
    out->style = g.vstyle;
    out->bounds = g.bounds;
    out->p0 = sh.p0;
    out->p1 = sh.p1;
    if (!sh.pts.empty()) {
        out->p0 = sh.pts.front();
        out->p1 = sh.pts.back();
    }
    out->ctrl = sh.ctrl;
    out->fill = g.has_fill ? &g.fill : nullptr;
    out->stroke = g.has_stroke ? &g.stroke : nullptr;
    out->dashed = g.dashed;
    out->fill_color = g.fill_color;
    if (sh.type == LS_TOOL_TEXT) out->text = sh.text.c_str();
    if (sh.type == LS_TOOL_STEP) out->text = g.text.c_str();
    out->text_rect = g.text_rect;
    out->text_color = g.text_color;
    out->text_background = g.text_background;
    out->text_outline = g.text_outline;
    out->text_size = g.text_size;
    if (sh.type == LS_TOOL_MOSAIC && g.patch.data) {
        out->patch = &g.patch;
        out->patch_rect = g.patch_rect;
    }
    out->magnify_src = g.magnify_src;
    out->radius = g.radius;
}

int32_t ls_session_shape_handles(const ls_session *s, ls_point *out, int32_t capacity) {
    if (!s || !s->selected_id || s->selected_id == s->editing_id) return 0;
    const Shape *sh = find_shape(s, s->selected_id);
    if (!sh) return 0;
    ls_point hp[9];
    const int n = shape_handles(*sh, hp);
    for (int i = 0; i < n && i < capacity && out; ++i) out[i] = hp[i];
    return n;
}

// ---------------------------------------------------------------- 文字

void ls_session_set_text_measurer(ls_session *s, ls_measure_text_fn fn, void *ctx) {
    if (!s) return;
    s->measurer = fn;
    s->measurer_ctx = ctx;
    // 已有文字按新测量重排
    for (Shape &sh : s->shapes) {
        if (sh.type != LS_TOOL_TEXT) continue;
        measure_shape(s, sh);
        touch(s, sh);
    }
}

int32_t ls_session_text_edit(const ls_session *s, ls_text_edit *out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof *out);
    if (!s || !s->editing_id) return 0;
    const Shape *sh = find_shape(s, s->editing_id);
    if (!sh) return 0;
    out->id = sh->id;
    out->rect = sh->text_rect;
    out->text = sh->text.c_str();
    out->font_size = sh->style.font_size;
    out->bold = sh->style.bold;
    text_colors(*sh, &out->color, &out->background, &out->outline);
    return 1;
}

int32_t ls_session_set_text(ls_session *s, const char *utf8) {
    if (!s || !s->editing_id) return 0;
    const Before b = take(s);
    Shape *sh = find_shape(s, s->editing_id);
    if (sh) {
        const char *t = utf8 ? utf8 : "";
        if (sh->text != t) {
            sh->text = t;
            measure_shape(s, *sh);
            touch(s, *sh);
        }
    }
    return changes(s, b, 0);
}

int32_t ls_session_end_text(ls_session *s) {
    if (!s || !s->editing_id) return 0;
    const Before b = take(s);
    const int extra = end_text_internal(s);
    return changes(s, b, extra);
}

}  // extern "C"
