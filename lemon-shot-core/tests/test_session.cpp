// 截图会话：逐条对照 docs/session-behavior.md
#include "test.h"

#include <string>
#include <vector>

namespace {

const int SHIFT = LS_MOD_SHIFT, ALT = LS_MOD_ALT, PRIMARY = LS_MOD_PRIMARY | LS_MOD_CMD;

// 测试用测量：每字节 10pt 宽，每行 20pt 高
void measure(void *, const char *t, float, int32_t, float *w, float *h) {
    int lines = 1, cur = 0, maxw = 0;
    for (const char *p = t; *p; ++p) {
        if (*p == '\n') {
            ++lines;
            cur = 0;
        } else if (++cur > maxw) {
            maxw = cur;
        }
    }
    *w = (float)(maxw * 10);
    *h = (float)(lines * 20);
}

struct Opt {
    float scale = 2;
    int lock = 0;
    bool init = false;
    ls_rect init_rect{0, 0, 0, 0};
    int rce = 0;
    int detect = 1;
    bool measurer = true;
};

struct Fx {
    ls_image base{nullptr, 0, 0, 0};
    ls_session *s = nullptr;
    explicit Fx(const Opt &o = Opt()) {
        ls_session_config c = ls_session_default_config();
        c.width = 1000;
        c.height = 800;
        c.scale = o.scale;
        base = ls_image_alloc((int)(1000 * o.scale + 0.5f), (int)(800 * o.scale + 0.5f));
        for (int y = 0; y < base.height; ++y)
            for (int x = 0; x < base.width; ++x) {
                uint8_t *p = base.data + (size_t)y * base.stride + x * 4;
                p[0] = (uint8_t)(x & 255);
                p[1] = (uint8_t)(y & 255);
                p[2] = (uint8_t)((x + y) & 255);
                p[3] = 255;
            }
        c.base = base;
        c.lock_selection = o.lock;
        c.has_initial_selection = o.init;
        c.initial_selection = o.init_rect;
        c.right_click_exits = o.rce;
        c.detect_windows = o.detect;
        s = ls_session_create(&c);
        if (o.measurer) ls_session_set_text_measurer(s, measure, nullptr);
    }
    ~Fx() {
        ls_session_destroy(s);
        ls_image_free(&base);
    }
    ls_session_state st() const {
        ls_session_state x;
        ls_session_get_state(s, &x);
        return x;
    }
    int down(float x, float y, int mods = 0, int clicks = 1, int button = LS_BUTTON_LEFT) {
        return ls_session_mouse_down(s, x, y, button, mods, clicks);
    }
    int move(float x, float y, int mods = 0) { return ls_session_mouse_move(s, x, y, mods); }
    int up(float x, float y, int mods = 0, int button = LS_BUTTON_LEFT) { return ls_session_mouse_up(s, x, y, button, mods); }
    int click(float x, float y, int mods = 0) { return down(x, y, mods) | up(x, y, mods); }
    int drag(float x0, float y0, float x1, float y1, int mods = 0) {
        int ev = move(x0, y0, mods);
        ev |= down(x0, y0, mods);
        ev |= move((x0 + x1) / 2, (y0 + y1) / 2, mods);
        ev |= move(x1, y1, mods);
        ev |= up(x1, y1, mods);
        return ev;
    }
    int key(int k, int mods = 0) { return ls_session_key_down(s, k, mods) | ls_session_key_up(s, k, mods); }
    std::vector<int> actions() {
        std::vector<int> v;
        for (ls_action a; (a = ls_session_take_action(s)) != LS_ACTION_NONE;) v.push_back(a);
        return v;
    }
    int select(float x, float y, float w, float h) { return ls_session_set_selection(s, ls_rect{x, y, w, h}); }
    ls_shape_view view(int i) const {
        ls_shape_view v;
        ls_session_shape_at(s, i, &v);
        return v;
    }
    ls_style style(int tool) const {
        ls_style st;
        ls_session_get_style(s, tool, &st);
        return st;
    }
    int set_style(int tool, void (*fn)(ls_style &)) {
        ls_style st = style(tool);
        fn(st);
        return ls_session_set_style(s, tool, &st);
    }
    int count() const { return ls_session_shape_count(s); }
};

bool rect_is(ls_rect r, float x, float y, float w, float h, float eps = 1e-3f) {
    const bool ok = std::fabs(r.x - x) <= eps && std::fabs(r.y - y) <= eps && std::fabs(r.w - w) <= eps && std::fabs(r.h - h) <= eps;
    if (!ok) std::printf("    矩形 (%g, %g, %g, %g) 期望 (%g, %g, %g, %g)\n", r.x, r.y, r.w, r.h, x, y, w, h);
    return ok;
}

bool aligned(float v, float scale) {
    const float k = v * scale;
    return std::fabs(k - std::floor(k + 0.5f)) < 1e-3f;
}

int count_verbs(const ls_path *p, ls_path_verb verb) {
    if (!p) return 0;
    int n = 0;
    ls_point pts[3];
    for (int i = 0; i < ls_path_count(p); ++i)
        if (ls_path_get(p, i, pts) == verb) ++n;
    return n;
}

bool has(const std::vector<int> &v, int a) {
    for (int x : v)
        if (x == a) return true;
    return false;
}

void set_windows(Fx &f, std::vector<ls_window> w) { ls_session_set_windows(f.s, w.data(), (int32_t)w.size()); }

}  // namespace

// ================================================================ 选区

TEST(session_drag_selection_pixel_aligned) {
    Fx f;
    int ev = f.move(100, 100);
    ls_session_state st = f.st();
    CHECK_EQ(st.phase, LS_PHASE_SELECTING);
    CHECK_EQ(st.has_hover, 1);
    CHECK(rect_is(st.hover, 0, 0, 1000, 800));
    CHECK_EQ(st.cursor, LS_CURSOR_CROSSHAIR);
    CHECK_EQ(st.show_magnifier, 1);
    CHECK(ev & LS_EV_REDRAW);
    f.down(100.3f, 100.2f);
    f.move(200, 175);
    st = f.st();
    CHECK_EQ(st.dragging_selection, 1);
    CHECK_EQ(st.has_selection, 1);
    CHECK_EQ(st.has_hover, 0);
    CHECK_EQ(st.show_magnifier, 1);
    f.move(300.7f, 250.1f);
    ev = f.up(300.7f, 250.1f);
    st = f.st();
    CHECK(ev & LS_EV_SELECTION);
    CHECK_EQ(st.phase, LS_PHASE_SELECTED);
    CHECK_EQ(st.dragging_selection, 0);
    CHECK(rect_is(st.selection, 100.5f, 100, 200, 150));
    CHECK(aligned(st.selection.x, 2) && aligned(st.selection.y, 2) && aligned(st.selection.w, 2) && aligned(st.selection.h, 2));
    int32_t x, y, w, h;
    ls_session_selection_pixels(f.s, &x, &y, &w, &h);
    CHECK_EQ(x, 201);
    CHECK_EQ(y, 200);
    CHECK_EQ(w, 400);
    CHECK_EQ(h, 300);
    CHECK_EQ(f.st().show_magnifier, 0);
}

TEST(session_drag_threshold_and_click_selects_hover) {
    Fx f;
    set_windows(f, {{7, {100, 100, 300, 200}}});
    f.move(150, 150);
    CHECK(rect_is(f.st().hover, 100, 100, 300, 200));
    f.down(150, 150);
    f.move(152, 151); // 2.2pt，没超过 3pt
    CHECK_EQ(f.st().dragging_selection, 0);
    f.up(152, 151);
    ls_session_state st = f.st();
    CHECK_EQ(st.phase, LS_PHASE_SELECTED);
    CHECK(rect_is(st.selection, 100, 100, 300, 200));
    CHECK_EQ(st.window_count, 0);
}

TEST(session_too_small_selection_discarded) {
    Fx f;
    f.drag(100, 100, 103.5f, 140);
    ls_session_state st = f.st();
    CHECK_EQ(st.phase, LS_PHASE_SELECTING);
    CHECK_EQ(st.has_selection, 0);
    CHECK_EQ(st.has_hover, 1);
}

TEST(session_window_hover) {
    Fx f;
    set_windows(f, {{4, {10.3f, 10.3f, 20.2f, 20.2f}}, {1, {50, 50, 300, 200}}, {2, {100, 100, 400, 300}}, {3, {900, 700, 300, 300}}});
    f.move(120, 120);
    CHECK(rect_is(f.st().hover, 50, 50, 300, 200)); // 最上层
    f.move(400, 350);
    CHECK(rect_is(f.st().hover, 100, 100, 400, 300));
    f.move(950, 750);
    CHECK(rect_is(f.st().hover, 900, 700, 100, 100)); // 夹到屏内
    f.move(800, 100);
    CHECK(rect_is(f.st().hover, 0, 0, 1000, 800)); // 都不包含：整屏
    f.move(20, 20);
    CHECK(rect_is(f.st().hover, 10.5f, 10.5f, 20, 20)); // 对齐到像素
    int ev = ls_session_mouse_exit(f.s);
    CHECK(ev & LS_EV_REDRAW);
    CHECK_EQ(f.st().has_hover, 0);
    CHECK_EQ(f.st().mouse_inside, 0);
    // 关闭窗口识别
    Opt o;
    o.detect = 0;
    Fx g(o);
    set_windows(g, {{1, {50, 50, 300, 200}}});
    g.move(120, 120);
    CHECK(rect_is(g.st().hover, 0, 0, 1000, 800));
}

TEST(session_shift_multi_window) {
    Fx f;
    set_windows(f, {{1, {50, 50, 300, 200}}, {2, {500, 300, 200, 200}}});
    f.move(100, 100);
    f.click(100, 100, SHIFT);
    ls_session_state st = f.st();
    CHECK_EQ(st.phase, LS_PHASE_SELECTED);
    CHECK_EQ(st.window_count, 1);
    CHECK(rect_is(st.selection, 50, 50, 300, 200));
    f.move(550, 350, SHIFT);
    f.click(550, 350, SHIFT);
    st = f.st();
    CHECK_EQ(st.window_count, 2);
    CHECK(rect_is(st.selection, 50, 50, 650, 450));
    int64_t ids[4] = {0, 0, 0, 0};
    CHECK_EQ(ls_session_selected_windows(f.s, ids, 4), 2);
    CHECK_EQ(ids[0], 1);
    CHECK_EQ(ids[1], 2);
    // 多选模式：标注工具、长截图、录屏无效；选区不能拉伸
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    CHECK_EQ(f.st().tool, LS_TOOL_NONE);
    f.key('2');
    CHECK_EQ(f.st().tool, LS_TOOL_NONE);
    f.key('S');
    f.key('V');
    f.key('O');
    std::vector<int> a = f.actions();
    CHECK_EQ(a.size(), 1);
    CHECK(has(a, LS_ACTION_OCR));
    f.move(700, 400);
    CHECK(f.st().cursor != LS_CURSOR_RESIZE_EW);
    // 再点一次移出集合
    f.click(100, 100, SHIFT);
    st = f.st();
    CHECK_EQ(st.window_count, 1);
    CHECK(rect_is(st.selection, 500, 300, 200, 200));
    f.click(550, 350, SHIFT);
    st = f.st();
    CHECK_EQ(st.window_count, 0);
    CHECK_EQ(st.phase, LS_PHASE_SELECTING);
    CHECK_EQ(st.has_selection, 0);
    // 整屏悬停不能多选
    f.move(900, 100);
    f.click(900, 100, SHIFT);
    CHECK_EQ(f.st().phase, LS_PHASE_SELECTING);
    // 多选后在选区外拖动：丢弃多选，重新拉出普通选区
    f.click(100, 100, SHIFT);
    f.drag(800, 600, 900, 700);
    st = f.st();
    CHECK_EQ(st.window_count, 0);
    CHECK(rect_is(st.selection, 800, 600, 100, 100));
}

TEST(session_ratio_lock_new_selection) {
    Opt o;
    o.scale = 1;
    Fx f(o);
    f.move(100, 100);
    f.down(100, 100);
    f.move(200, 150);
    f.key('4'); // 16:9
    ls_session_state st = f.st();
    CHECK_EQ(st.ratio_index, 4);
    CHECK_NEAR(st.ratio, 16.0 / 9, 1e-5);
    CHECK(rect_is(st.selection, 100, 100, 100, 56));
    CHECK_EQ(f.st().tool, LS_TOOL_NONE); // 数字键没有切工具
    f.move(500, 120);
    CHECK(rect_is(f.st().selection, 100, 100, 400, 225));
    f.move(40, 60); // 往左上
    CHECK(rect_is(f.st().selection, 40, 66, 60, 34));
    f.up(40, 60);
    CHECK_EQ(f.st().ratio_index, 4); // 松开后保留显示
    // 下一次拖动开始时清零
    f.down(700, 600);
    CHECK_EQ(f.st().ratio_index, 0);
    f.up(700, 600);
    // 贴近屏幕底边：两边一起缩小
    Fx g(o);
    g.move(100, 700);
    g.down(100, 700);
    g.move(150, 720);
    g.key('4');
    g.move(500, 750);
    CHECK(rect_is(g.st().selection, 100, 700, 178, 100));
    g.up(500, 750);
    // Shift 1:1
    Fx h(o);
    h.drag(100, 100, 300, 150, SHIFT);
    CHECK(rect_is(h.st().selection, 100, 100, 200, 200));
    // 拖动中途按下 Shift
    Fx k(o);
    k.move(100, 100);
    k.down(100, 100);
    k.move(300, 150);
    CHECK(rect_is(k.st().selection, 100, 100, 200, 50));
    int ev = ls_session_modifiers(k.s, SHIFT);
    CHECK(ev & LS_EV_SELECTION);
    CHECK(rect_is(k.st().selection, 100, 100, 200, 200));
    ls_session_modifiers(k.s, 0);
    CHECK(rect_is(k.st().selection, 100, 100, 200, 50));
    k.up(300, 150);
}

TEST(session_resize_handles_cursors_flip) {
    Opt o;
    o.scale = 1;
    Fx f(o);
    f.select(200, 150, 400, 300);
    CHECK_EQ(f.st().phase, LS_PHASE_SELECTED);
    f.move(600, 450);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_NWSE);
    f.move(400, 150);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_NS);
    f.move(200, 300);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_EW);
    f.move(600, 150);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_NESW);
    f.move(400, 300);
    CHECK_EQ(f.st().cursor, LS_CURSOR_MOVE);
    f.move(900, 700);
    CHECK_EQ(f.st().cursor, LS_CURSOR_CROSSHAIR);
    // 控制点优先于边：左上角 handle_hit=14 内
    f.move(205, 155);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_NWSE);
    f.move(300, 152);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_NS);
    f.move(300, 156);
    CHECK_EQ(f.st().cursor, LS_CURSOR_MOVE);
    // 拖右下角
    int ev = f.drag(600, 450, 700, 500);
    CHECK(ev & LS_EV_SELECTION);
    CHECK(rect_is(f.st().selection, 200, 150, 500, 350));
    // 拖动中保持开始时的光标
    f.move(200, 300);
    f.down(200, 300);
    f.move(500, 300);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_EW);
    CHECK_EQ(f.st().dragging_selection, 1);
    CHECK_EQ(f.st().show_magnifier, 1);
    // 拉过对边翻转
    f.move(800, 300);
    f.up(800, 300);
    CHECK(rect_is(f.st().selection, 700, 150, 100, 350));
    // 拉出屏幕：夹在屏内
    f.select(100, 100, 400, 200);
    f.drag(500, 300, 2000, 2000);
    CHECK(rect_is(f.st().selection, 100, 100, 900, 700));
    // Shift 保持宽高比
    f.select(100, 100, 400, 200);
    f.drag(500, 300, 600, 320, SHIFT);
    CHECK(rect_is(f.st().selection, 100, 100, 500, 250));
    // 拉伸时数字键锁比例
    f.select(100, 100, 400, 200);
    f.move(500, 300);
    f.down(500, 300);
    f.move(520, 310);
    f.key('1');
    CHECK(rect_is(f.st().selection, 100, 100, 420, 420));
    f.up(520, 310);
    // 边拖动 + 比例：另一个方向以中心为准
    f.select(100, 100, 400, 200);
    f.move(500, 200);
    f.down(500, 200);
    f.move(510, 200);
    f.key('1');
    CHECK(rect_is(f.st().selection, 100, 0, 410, 410)); // 以原中心为准，夹到屏内
    f.up(510, 200);
    // 拉伸结束时不小于最小尺寸
    f.select(100, 100, 400, 200);
    f.drag(500, 200, 101, 200);
    CHECK(f.st().selection.w >= 4);
}

TEST(session_move_selection) {
    Opt o;
    o.scale = 1;
    Fx f(o);
    f.select(200, 150, 400, 300);
    int ev = f.drag(300, 300, 350, 320);
    CHECK(ev & LS_EV_SELECTION);
    CHECK(rect_is(f.st().selection, 250, 170, 400, 300));
    f.drag(300, 300, 2000, 2000);
    CHECK(rect_is(f.st().selection, 600, 500, 400, 300));
}

TEST(session_arrow_keys) {
    Fx f; // scale 2：1 像素 = 0.5pt
    f.select(100, 100, 200, 100);
    int ev = f.key(LS_KEY_RIGHT);
    CHECK(ev & LS_EV_SELECTION);
    CHECK(rect_is(f.st().selection, 100.5f, 100, 200, 100));
    f.key(LS_KEY_RIGHT, ALT);
    CHECK(rect_is(f.st().selection, 105.5f, 100, 200, 100));
    f.key(LS_KEY_RIGHT, SHIFT);
    CHECK(rect_is(f.st().selection, 105.5f, 100, 200.5f, 100));
    f.key(LS_KEY_UP, SHIFT);
    CHECK(rect_is(f.st().selection, 105.5f, 100, 200.5f, 99.5f));
    f.key(LS_KEY_DOWN, SHIFT | ALT);
    CHECK(rect_is(f.st().selection, 105.5f, 100, 200.5f, 104.5f));
    f.key(LS_KEY_DOWN);
    CHECK(rect_is(f.st().selection, 105.5f, 100.5f, 200.5f, 104.5f));
    f.key(LS_KEY_LEFT);
    f.key(LS_KEY_UP);
    CHECK(rect_is(f.st().selection, 105, 100, 200.5f, 104.5f));
    // 贴边不再移动
    f.select(800, 700, 200, 100);
    f.key(LS_KEY_RIGHT);
    CHECK(rect_is(f.st().selection, 800, 700, 200, 100));
    // 选中标注时方向键移动标注
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.select(100, 100, 600, 500);
    f.drag(200, 200, 300, 260);
    f.key(LS_KEY_LEFT);
    f.key(LS_KEY_LEFT, ALT);
    ls_shape_view v = f.view(0);
    CHECK_NEAR(v.p0.x, 194.5, 1e-4);
    CHECK(rect_is(f.st().selection, 100, 100, 600, 500));
    // 连续方向键合并为一次撤销
    ls_session_undo(f.s);
    CHECK_NEAR(f.view(0).p0.x, 200, 1e-4);
}

TEST(session_lock_selection) {
    Opt o;
    o.scale = 1;
    o.lock = 1;
    o.init = true;
    o.init_rect = ls_rect{0, 0, 400, 300};
    Fx f(o);
    ls_session_state st = f.st();
    CHECK_EQ(st.phase, LS_PHASE_SELECTED);
    CHECK(rect_is(st.selection, 0, 0, 400, 300));
    f.move(400, 150);
    CHECK(f.st().cursor != LS_CURSOR_RESIZE_EW);
    f.move(200, 150);
    CHECK_EQ(f.st().cursor, LS_CURSOR_ARROW);
    f.drag(399, 150, 500, 150);
    f.drag(200, 150, 250, 200);
    CHECK(rect_is(f.st().selection, 0, 0, 400, 300));
    f.key(LS_KEY_RIGHT);
    CHECK(rect_is(f.st().selection, 0, 0, 400, 300));
    CHECK_EQ(f.select(10, 10, 10, 10), 0);
    f.key('F');
    f.key('S');
    f.key('V');
    CHECK(rect_is(f.st().selection, 0, 0, 400, 300));
    CHECK(f.actions().empty());
    // 标注照常，方向键只移动标注
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.drag(100, 100, 200, 200);
    CHECK_EQ(f.count(), 1);
    f.key(LS_KEY_RIGHT);
    CHECK_NEAR(f.view(0).p0.x, 101, 1e-4);
    // 右键直接退出
    f.down(50, 50, 0, 1, LS_BUTTON_RIGHT);
    CHECK(has(f.actions(), LS_ACTION_CANCEL));
}

// ================================================================ 标注

TEST(session_rect_create_and_constraints) {
    Fx f;
    f.select(100, 100, 600, 500);
    int ev = ls_session_set_tool(f.s, LS_TOOL_RECT);
    CHECK(ev & LS_EV_TOOL);
    f.move(250, 250);
    CHECK_EQ(f.st().cursor, LS_CURSOR_CROSSHAIR);
    ev = f.drag(200, 200, 300, 260);
    CHECK(ev & LS_EV_REDRAW);
    CHECK(ev & LS_EV_HISTORY);
    ls_session_state st = f.st();
    CHECK_EQ(st.shape_count, 1);
    CHECK_EQ(f.count(), 1);
    CHECK(st.selected_shape != 0);
    CHECK_EQ(st.can_undo, 1);
    ls_shape_view v = f.view(0);
    CHECK_EQ(v.type, LS_TOOL_RECT);
    CHECK_EQ(v.id, st.selected_shape);
    CHECK_EQ(v.selected, 1);
    CHECK(v.stroke != nullptr);
    CHECK(v.fill == nullptr);
    CHECK_EQ(v.style.line_width, 3);
    CHECK_EQ(v.fill_color.r, 0xFF);
    CHECK_NEAR(v.p0.x, 200, 1e-4);
    CHECK_NEAR(v.p1.y, 260, 1e-4);
    CHECK(v.bounds.x <= 198.5f && v.bounds.x + v.bounds.w >= 301.5f);
    CHECK_EQ(count_verbs(v.stroke, LS_PATH_LINE), 3);
    CHECK_EQ(ls_session_shape_handles(f.s, nullptr, 0), 8);
    // 圆角
    f.set_style(LS_TOOL_RECT, [](ls_style &s) { s.corner_radius = 8; });
    CHECK_EQ(count_verbs(f.view(0).stroke, LS_PATH_CUBIC), 4);
    // Shift 正方形
    f.drag(400, 200, 450, 300, SHIFT);
    v = f.view(1);
    CHECK_NEAR(v.p1.x, 500, 1e-4);
    CHECK_NEAR(v.p1.y, 300, 1e-4);
    // 小于 2pt 作废
    f.drag(200, 400, 201, 410);
    CHECK_EQ(f.count(), 2);
    // 单击不新建，取消选中
    f.click(600, 500);
    CHECK_EQ(f.count(), 2);
    CHECK_EQ(f.st().selected_shape, 0);
    // 选区外：有标注时无效
    f.move(900, 700);
    CHECK_EQ(f.st().cursor, LS_CURSOR_ARROW);
    f.drag(800, 650, 900, 700);
    CHECK_EQ(f.count(), 2);
    CHECK(rect_is(f.st().selection, 100, 100, 600, 500));
}

TEST(session_tool_outside_no_shapes_reselects) {
    Fx f;
    f.select(100, 100, 300, 300);
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.drag(600, 500, 700, 600);
    CHECK_EQ(f.count(), 0);
    CHECK(rect_is(f.st().selection, 600, 500, 100, 100));
}

TEST(session_ellipse_filled) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_ELLIPSE);
    f.set_style(LS_TOOL_ELLIPSE, [](ls_style &s) { s.filled = 1; });
    f.drag(200, 200, 300, 260);
    ls_shape_view v = f.view(0);
    CHECK(v.fill != nullptr);
    CHECK(v.stroke == nullptr);
    CHECK_EQ(count_verbs(v.fill, LS_PATH_CUBIC), 4);
    // 实心：区域内命中
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.click(600, 500);
    f.move(250, 230);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
    CHECK_EQ(f.view(0).hovered, 1);
}

TEST(session_line_snap_dashed_hit) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_LINE);
    f.set_style(LS_TOOL_LINE, [](ls_style &s) { s.variant = 1; });
    f.drag(200, 200, 300, 210, SHIFT);
    ls_shape_view v = f.view(0);
    CHECK_EQ(v.dashed, 1);
    CHECK_NEAR(v.p1.y, 200, 1e-3);
    CHECK_NEAR(v.p1.x, 200 + std::sqrt(100.0 * 100 + 10 * 10), 1e-3);
    CHECK_EQ(count_verbs(v.stroke, LS_PATH_MOVE), 1);
    CHECK_EQ(ls_session_shape_handles(f.s, nullptr, 0), 2);
    // 45° 吸附
    f.drag(200, 400, 300, 310, SHIFT);
    v = f.view(1);
    CHECK_NEAR(v.p1.x - v.p0.x, -(v.p1.y - v.p0.y), 1e-3);
    // 拖过阈值但最终长度小于 2：作废
    f.move(500, 500);
    f.down(500, 500);
    f.move(505, 500);
    f.move(501, 500);
    f.up(501, 500);
    CHECK_EQ(f.count(), 2);
    // 命中：距离 ≤ max(线宽/2+4, 6)
    f.click(650, 550);
    f.move(250, 205);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
    f.move(250, 215);
    CHECK(f.st().cursor != LS_CURSOR_POINTER);
}

TEST(session_arrow_variants) {
    Fx f;
    f.select(100, 100, 800, 600);
    ls_session_set_tool(f.s, LS_TOOL_ARROW);
    // 样式 0：实心锥形箭杆 + 三角头
    f.drag(200, 300, 400, 300);
    ls_shape_view v = f.view(0);
    CHECK(v.fill != nullptr);
    CHECK(v.stroke == nullptr);
    CHECK_EQ(ls_path_count(v.fill), 8);
    ls_point pts[3];
    ls_path_get(v.fill, 3, pts);
    CHECK_NEAR(pts[0].x, 400, 1e-3); // 尖端在终点
    ls_path_get(v.fill, 2, pts);     // 头根部外侧：头长 22，半宽 0.45*22
    CHECK_NEAR(pts[0].x, 378, 1e-3);
    CHECK_NEAR(std::fabs(pts[0].y - 300), 9.9, 1e-3);
    ls_path_get(v.fill, 0, pts); // 尾端半宽 0.175w
    CHECK_NEAR(std::fabs(pts[0].y - 300), 0.7, 1e-3);
    // 样式 1、2
    f.set_style(LS_TOOL_ARROW, [](ls_style &s) { s.variant = 1; });
    CHECK_EQ(count_verbs(f.view(0).stroke, LS_PATH_MOVE), 2); // 选中的箭头一起改了
    CHECK(f.view(0).fill == nullptr);
    f.set_style(LS_TOOL_ARROW, [](ls_style &s) { s.variant = 2; });
    CHECK_EQ(count_verbs(f.view(0).stroke, LS_PATH_MOVE), 3);
    // 样式 3：曲线，控制点默认在中点左侧 20% 长度
    f.click(800, 600);
    f.set_style(LS_TOOL_ARROW, [](ls_style &s) { s.variant = 3; });
    f.drag(200, 500, 400, 500);
    v = f.view(1);
    CHECK_NEAR(v.ctrl.x, 300, 1e-3);
    CHECK_NEAR(v.ctrl.y, 460, 1e-3);
    CHECK(v.stroke != nullptr);
    CHECK(v.fill != nullptr);
    CHECK_EQ(count_verbs(v.stroke, LS_PATH_CUBIC), 1);
    ls_path_get(v.fill, 0, pts);
    CHECK_NEAR(pts[0].x, 400, 1e-3);
    ls_point hp[9];
    CHECK_EQ(ls_session_shape_handles(f.s, hp, 9), 3);
    CHECK_NEAR(hp[2].x, 300, 1e-3);
    CHECK_NEAR(hp[2].y, 480, 1e-3);
    // 拖控制点：曲线经过手柄
    f.move(300, 480);
    CHECK_EQ(f.st().cursor, LS_CURSOR_MOVE);
    f.drag(300, 480, 300, 450);
    ls_session_shape_handles(f.s, hp, 9);
    CHECK_NEAR(hp[2].y, 450, 1e-3);
    CHECK_NEAR(f.view(1).ctrl.y, 400, 1e-3);
    // 拖端点
    f.drag(400, 500, 420, 520);
    CHECK_NEAR(f.view(1).p1.x, 420, 1e-3);
    CHECK_NEAR(f.view(1).p1.y, 520, 1e-3);
}

TEST(session_pen_smooth_highlighter_shift) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_PEN);
    f.move(200, 200);
    f.down(200, 200);
    f.move(210, 205);
    f.move(210.5f, 205.2f); // 距离不足 1pt 不记录
    f.move(220, 215);
    f.move(230, 230);
    f.move(240, 250);
    f.up(240, 250);
    ls_shape_view v = f.view(0);
    CHECK(v.stroke != nullptr);
    CHECK_EQ(count_verbs(v.stroke, LS_PATH_MOVE), 1);
    CHECK_EQ(count_verbs(v.stroke, LS_PATH_CUBIC), 3); // 5 个点：中点平滑 3 段 + 末段直线
    CHECK_EQ(count_verbs(v.stroke, LS_PATH_LINE), 1);
    CHECK_EQ(ls_session_shape_handles(f.s, nullptr, 0), 0);
    // 荧光笔：线宽 ×3，不透明度 ×0.4
    f.set_style(LS_TOOL_PEN, [](ls_style &s) { s.variant = 1; });
    v = f.view(0);
    CHECK_NEAR(v.style.line_width, 9, 1e-4);
    CHECK_NEAR(v.style.opacity, 0.4, 1e-4);
    // Shift 画直线
    f.move(300, 300);
    f.down(300, 300);
    f.move(320, 320, SHIFT);
    f.move(340, 330, SHIFT);
    f.up(340, 330, SHIFT);
    v = f.view(1);
    CHECK_EQ(count_verbs(v.stroke, LS_PATH_LINE), 1);
    CHECK_EQ(count_verbs(v.stroke, LS_PATH_CUBIC), 0);
    CHECK_NEAR(v.p1.x, 340, 1e-4);
}

namespace {

// 冻结画面里某个像素块的平均（与内核像素化同样的取整）
void block_avg(const ls_image &im, int x0, int y0, int b, uint8_t out[4]) {
    uint32_t s[4] = {0, 0, 0, 0};
    for (int y = y0; y < y0 + b; ++y)
        for (int x = x0; x < x0 + b; ++x)
            for (int c = 0; c < 4; ++c) s[c] += im.data[(size_t)y * im.stride + x * 4 + c];
    const uint32_t n = (uint32_t)(b * b);
    for (int c = 0; c < 4; ++c) out[c] = (uint8_t)((s[c] + n / 2) / n);
}

const uint8_t *patch_px(const ls_shape_view &v, int x, int y) { return v.patch->data + (size_t)y * v.patch->stride + x * 4; }

}  // namespace

TEST(session_mosaic_rect_pixelate_and_move) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_MOSAIC);
    f.set_style(LS_TOOL_MOSAIC, [](ls_style &s) {
        s.variant = 0;
        s.block_size = 5; // 10 像素
    });
    f.drag(150, 150, 200, 180);
    ls_shape_view v = f.view(0);
    CHECK(v.patch != nullptr);
    if (!v.patch) return;
    CHECK(rect_is(v.patch_rect, 150, 150, 50, 30));
    CHECK_EQ(v.patch->width, 100);
    CHECK_EQ(v.patch->height, 60);
    uint8_t e[4];
    block_avg(f.base, 300, 300, 10, e);
    CHECK(std::memcmp(patch_px(v, 0, 0), e, 4) == 0);
    CHECK(std::memcmp(patch_px(v, 9, 9), e, 4) == 0);
    block_avg(f.base, 310, 300, 10, e);
    CHECK(std::memcmp(patch_px(v, 10, 0), e, 4) == 0);
    // 与 ls_pixelate 结果一致
    ls_image ref;
    ls_pixelate(&f.base, 300, 300, 100, 60, 10, &ref);
    bool same = true;
    for (int y = 0; y < 60; ++y) same = same && std::memcmp(patch_px(v, 0, y), ref.data + (size_t)y * ref.stride, 400) == 0;
    CHECK(same);
    ls_image_free(&ref);
    // 指针在下一次修改前稳定
    ls_shape_view again = f.view(0);
    CHECK(again.patch == v.patch);
    // 移动：块网格对齐整张图，按新位置重新取
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.drag(175, 165, 177.5f, 167.5f);
    v = f.view(0);
    CHECK(rect_is(v.patch_rect, 152.5f, 152.5f, 50, 30));
    block_avg(f.base, 300, 300, 10, e); // (305,305) 仍在 [300,310) 块里
    CHECK(std::memcmp(patch_px(v, 0, 0), e, 4) == 0);
    block_avg(f.base, 310, 310, 10, e);
    CHECK(std::memcmp(patch_px(v, 5, 5), e, 4) == 0);
    // 模糊样式：与 ls_blur 同区域结果一致
    ls_session_set_tool(f.s, LS_TOOL_MOSAIC);
    f.set_style(LS_TOOL_MOSAIC, [](ls_style &s) {
        s.variant = 2;
        s.block_size = 4;
    });
    f.drag(300, 300, 340, 330);
    v = f.view(1);
    CHECK(v.patch != nullptr);
    if (!v.patch) return;
    ls_blur(&f.base, 600, 600, 80, 60, 8, &ref);
    same = true;
    for (int y = 0; y < 60; ++y) same = same && std::memcmp(patch_px(v, 0, y), ref.data + (size_t)y * ref.stride, 320) == 0;
    CHECK(same);
    ls_image_free(&ref);
    // 已有矩形马赛克的 variant 0→2 可以互换，但不会变成画笔样式
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.click(320, 315);
    f.set_style(LS_TOOL_MOSAIC, [](ls_style &s) { s.variant = 1; });
    CHECK_EQ(f.view(1).style.variant, 2);
}

TEST(session_mosaic_brush_mask_incremental) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_MOSAIC);
    f.set_style(LS_TOOL_MOSAIC, [](ls_style &s) {
        s.variant = 1;
        s.line_width = 20; // 笔刷直径 20pt = 40 像素
        s.block_size = 4;
    });
    f.move(300, 300.25f);
    f.down(300, 300.25f);
    f.move(305, 300.25f);
    f.move(340, 300.25f);
    ls_shape_view v = f.view(0);
    CHECK(v.patch != nullptr);
    if (!v.patch) return;
    CHECK(aligned(v.patch_rect.x, 2) && aligned(v.patch_rect.y, 2));
    const int ox = (int)(v.patch_rect.x * 2 + 0.5f), oy = (int)(v.patch_rect.y * 2 + 0.5f);
    // 线在像素 y=600.5；中心不透明，半径 20 像素处半透明，外面全透明
    CHECK_EQ(patch_px(v, 640 - ox, 600 - oy)[3], 255);
    CHECK_EQ(patch_px(v, 640 - ox, 619 - oy)[3], 255);
    CHECK_NEAR(patch_px(v, 640 - ox, 580 - oy)[3], 128, 1);
    CHECK_EQ(patch_px(v, 640 - ox, 622 - oy)[3], 0);
    // 继续画（增量更新），然后用撤销/重做强制整笔重建，比较同一像素
    // 平台每次移动都会重画：每步都读一次，走增量更新（中途超出预留边距时整笔重建）
    for (int i = 1; i <= 40; ++i) {
        f.move(340 + i * 3.0f, 300.25f + (float)(i % 7) * 4);
        f.view(0);
    }
    f.up(460, 324.25f);
    v = f.view(0);
    struct Sample {
        int x, y;
        uint8_t px[4];
    };
    std::vector<Sample> samples;
    const int ox1 = (int)(v.patch_rect.x * 2 + 0.5f), oy1 = (int)(v.patch_rect.y * 2 + 0.5f);
    for (int y = 560; y < 700; y += 7)
        for (int x = 580; x < 960; x += 11) {
            Sample s{x, y, {0, 0, 0, 0}};
            if (x - ox1 < 0 || y - oy1 < 0 || x - ox1 >= v.patch->width || y - oy1 >= v.patch->height) continue;
            std::memcpy(s.px, patch_px(v, x - ox1, y - oy1), 4);
            samples.push_back(s);
        }
    CHECK(samples.size() > 50);
    ls_session_undo(f.s);
    CHECK_EQ(f.count(), 0);
    ls_session_redo(f.s);
    v = f.view(0);
    const int ox2 = (int)(v.patch_rect.x * 2 + 0.5f), oy2 = (int)(v.patch_rect.y * 2 + 0.5f);
    int diff = 0;
    for (const Sample &s : samples) {
        uint8_t z[4] = {0, 0, 0, 0};
        const bool in = s.x - ox2 >= 0 && s.y - oy2 >= 0 && s.x - ox2 < v.patch->width && s.y - oy2 < v.patch->height;
        const uint8_t *p = in ? patch_px(v, s.x - ox2, s.y - oy2) : z;
        if (std::memcmp(p, s.px, 4) != 0) {
            ++diff;
            std::printf("    像素 (%d,%d) 增量 %d,%d,%d,%d 重建 %d,%d,%d,%d\n", s.x, s.y, s.px[0], s.px[1], s.px[2], s.px[3], p[0], p[1], p[2], p[3]);
        }
    }
    CHECK_EQ(diff, 0);
    // 命中：笔刷半径内
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.click(650, 550);
    f.move(320, 305);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
}

TEST(session_text_edit_flow) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_TEXT);
    f.move(300, 300);
    CHECK_EQ(f.st().cursor, LS_CURSOR_IBEAM);
    int ev = f.click(300, 300);
    CHECK(ev & LS_EV_TEXT_BEGIN);
    ls_session_state st = f.st();
    CHECK(st.editing_text != 0);
    ls_text_edit te;
    CHECK_EQ(ls_session_text_edit(f.s, &te), 1);
    CHECK(rect_is(te.rect, 296, 300 - 18 * 0.7f, 18, 20)); // 空文字：一个字宽、一行高
    CHECK_EQ(te.font_size, 18);
    CHECK_STR(te.text, "");
    ev = ls_session_set_text(f.s, "Hi");
    CHECK(ev & LS_EV_REDRAW);
    ls_shape_view v = f.view(0);
    CHECK_STR(v.text, "Hi");
    CHECK_EQ(v.editing, 1);
    CHECK(rect_is(v.text_rect, 296, 300 - 18 * 0.7f, 20, 20));
    CHECK(rect_is(v.bounds, 292 - 1, 300 - 18 * 0.7f - 4 - 1, 28 + 2, 28 + 2));
    ls_session_set_text(f.s, "Hi\nthere");
    CHECK(rect_is(f.view(0).text_rect, 296, 300 - 18 * 0.7f, 50, 40));
    CHECK_EQ(f.st().can_undo, 0); // 编辑中不记撤销
    ev = ls_session_end_text(f.s);
    CHECK(ev & LS_EV_TEXT_END);
    CHECK(ev & LS_EV_HISTORY);
    CHECK_EQ(f.st().editing_text, 0);
    CHECK_EQ(f.count(), 1);
    CHECK_EQ(ls_session_text_edit(f.s, &te), 0);
    ls_session_undo(f.s);
    CHECK_EQ(f.count(), 0);
    ls_session_redo(f.s);
    CHECK_EQ(f.count(), 1);
    // 空文字结束：删除，不留撤销记录
    const int can_redo = f.st().can_redo;
    f.click(400, 450);
    CHECK(f.st().editing_text != 0);
    ev = ls_session_end_text(f.s);
    CHECK_EQ(f.count(), 1);
    CHECK_EQ(f.st().can_redo, can_redo);
    // 编辑中点别处：只结束编辑，不新建
    f.click(400, 450);
    ls_session_set_text(f.s, "x");
    ev = f.click(500, 500);
    CHECK(ev & LS_EV_TEXT_END);
    CHECK_EQ(f.count(), 2);
    CHECK_EQ(f.st().editing_text, 0);
    // 双击已有文字进入编辑；清空后结束 = 删除（可撤销）
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.move(300, 295);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
    f.down(300, 295);
    f.up(300, 295);
    ev = f.down(300, 295, 0, 2);
    f.up(300, 295);
    CHECK(ev & LS_EV_TEXT_BEGIN);
    CHECK_EQ(ls_session_text_edit(f.s, &te), 1);
    CHECK_STR(te.text, "Hi\nthere");
    CHECK_EQ(f.view(0).editing, 1);
    ls_session_set_text(f.s, "");
    ls_session_end_text(f.s);
    CHECK_EQ(f.count(), 1);
    ls_session_undo(f.s);
    CHECK_EQ(f.count(), 2);
    CHECK_STR(f.view(0).text, "Hi\nthere");
}

TEST(session_text_styles_and_fallback_measure) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_TEXT);
    f.set_style(LS_TOOL_TEXT, [](ls_style &s) { s.variant = 1; });
    f.click(200, 200);
    ls_session_set_text(f.s, "A");
    ls_text_edit te;
    ls_session_text_edit(f.s, &te);
    CHECK_EQ(te.outline.a, 255);
    CHECK_EQ(te.outline.r, 255); // 红色亮度 < 0.5：白色描边
    CHECK_EQ(te.background.a, 0);
    ls_session_end_text(f.s);
    ls_shape_view v = f.view(0);
    CHECK_EQ(v.text_outline.r, 255);
    CHECK_EQ(v.text_color.r, 0xFF);
    CHECK_EQ(v.text_color.g, 0x3B);
    CHECK_EQ(v.text_size, 18);
    // 底色样式：底色 = 颜色，文字 = 对比色
    f.set_style(LS_TOOL_TEXT, [](ls_style &s) {
        s.variant = 2;
        s.color = ls_color{0xFF, 0xCC, 0x00, 0xFF};
    });
    f.click(200, 300);
    ls_session_set_text(f.s, "B");
    ls_session_end_text(f.s);
    v = f.view(1);
    CHECK_EQ(v.text_background.r, 0xFF);
    CHECK_EQ(v.text_background.g, 0xCC);
    CHECK_EQ(v.text_color.r, 0); // 黄色太亮：黑字
    // 编辑中改字号：重新测量并通知平台更新输入框
    f.click(200, 400);
    ls_session_set_text(f.s, "abc");
    int ev = f.set_style(LS_TOOL_TEXT, [](ls_style &s) { s.font_size = 30; });
    CHECK(ev & LS_EV_TEXT_BEGIN);
    ls_session_text_edit(f.s, &te);
    CHECK_EQ(te.font_size, 30);
    ls_session_end_text(f.s);
    // 没有测量回调时按字数估计
    Opt o;
    o.measurer = false;
    Fx g(o);
    g.select(100, 100, 600, 500);
    ls_session_set_tool(g.s, LS_TOOL_TEXT);
    g.click(200, 200);
    ls_session_set_text(g.s, "ab\n中");
    ls_session_text_edit(g.s, &te);
    CHECK_NEAR(te.rect.w, 21.6, 1e-3); // max(2*0.6*18, 18)
    CHECK_NEAR(te.rect.h, 45, 1e-3);   // 2 行 × 1.25 × 18
}

TEST(session_step_numbers) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_STEP);
    int ev = f.click(200, 200);
    CHECK(ev & LS_EV_TOOL);
    f.click(250, 200);
    f.click(300, 200);
    CHECK_EQ(f.count(), 3);
    CHECK_STR(f.view(0).text, "1");
    CHECK_STR(f.view(2).text, "3");
    CHECK_EQ(f.style(LS_TOOL_STEP).next_number, 4);
    ls_shape_view v = f.view(0);
    CHECK_NEAR(v.text_size, 0.55 * 24, 1e-4);
    CHECK(rect_is(v.text_rect, 188, 188, 24, 24));
    CHECK(v.fill != nullptr);
    CHECK(v.stroke == nullptr);
    CHECK_EQ(v.fill_color.r, 0xFF);
    CHECK_EQ(v.text_color.r, 255);
    CHECK_EQ(v.text_color.g, 255); // 红底白字
    CHECK_EQ(ls_session_shape_handles(f.s, nullptr, 0), 0);
    // 撤销连带恢复「下一个」
    ls_session_undo(f.s);
    CHECK_EQ(f.count(), 2);
    CHECK_EQ(f.style(LS_TOOL_STEP).next_number, 3);
    // 空心：白色填充 + 描边（线宽 d/12），文字用颜色
    f.set_style(LS_TOOL_STEP, [](ls_style &s) { s.variant = 1; });
    f.click(350, 200);
    v = f.view(2);
    CHECK_STR(v.text, "3");
    CHECK_EQ(v.fill_color.r, 255);
    CHECK_EQ(v.fill_color.g, 255);
    CHECK(v.stroke != nullptr);
    CHECK_NEAR(v.style.line_width, 2, 1e-4);
    CHECK_EQ(v.text_color.g, 0x3B);
    // 方块 + 黄色：黑字
    f.set_style(LS_TOOL_STEP, [](ls_style &s) {
        s.variant = 2;
        s.color = ls_color{0xFF, 0xCC, 0x00, 0xFF};
    });
    f.click(400, 200);
    v = f.view(3);
    CHECK_EQ(v.text_color.r, 0);
    CHECK_EQ(count_verbs(v.fill, LS_PATH_CUBIC), 4);
    // 拖动也只在按下点放置
    f.drag(450, 300, 500, 350);
    CHECK_EQ(f.count(), 5);
    CHECK_NEAR(f.view(4).p0.x, 450, 1e-4);
}

TEST(session_highlight_mask) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_HIGHLIGHT);
    f.drag(200, 200, 300, 300);
    f.drag(400, 200, 500, 260);
    CHECK_EQ(f.st().shape_count, 2);
    CHECK_EQ(f.count(), 3);
    ls_shape_view m = f.view(0);
    CHECK_EQ(m.type, LS_SHAPE_MASK);
    CHECK(m.fill != nullptr);
    CHECK(m.stroke == nullptr);
    CHECK_EQ(m.fill_color.r, 0);
    CHECK_EQ(m.fill_color.a, 255);
    CHECK_NEAR(m.style.opacity, 0.55, 1e-5);
    CHECK_EQ(count_verbs(m.fill, LS_PATH_MOVE), 3);
    CHECK(rect_is(m.bounds, 100, 100, 600, 500));
    ls_shape_view h = f.view(1);
    CHECK_EQ(h.type, LS_TOOL_HIGHLIGHT);
    CHECK(h.fill == nullptr && h.stroke == nullptr);
    CHECK_EQ(ls_session_shape_handles(f.s, nullptr, 0), 8);
    // 遮罩深浅：滚轮作用于局部高亮（0.1–0.95）
    for (int i = 0; i < 20; ++i) ls_session_scroll(f.s, 650, 550, 1, 0);
    CHECK_NEAR(f.style(LS_TOOL_HIGHLIGHT).opacity, 0.95, 1e-5);
    // 撤销全部后遮罩消失
    while (f.st().can_undo) ls_session_undo(f.s);
    CHECK_EQ(f.count(), 0);
}

TEST(session_magnify) {
    Fx f;
    f.select(100, 100, 800, 600);
    ls_session_set_tool(f.s, LS_TOOL_MAGNIFY);
    f.click(300, 300);
    ls_shape_view v = f.view(0);
    CHECK_NEAR(v.radius, 60, 1e-4);
    CHECK(rect_is(v.magnify_src, 270, 270, 60, 60)); // 倍数 2
    CHECK(v.stroke != nullptr);
    CHECK_EQ(v.style.line_width, 3);
    ls_point hp[9];
    CHECK_EQ(ls_session_shape_handles(f.s, hp, 9), 1);
    CHECK_NEAR(hp[0].x, 360, 1e-4);
    f.move(360, 300);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_EW);
    f.drag(360, 300, 380, 300);
    CHECK_NEAR(f.view(0).radius, 80, 1e-4);
    f.drag(600, 500, 604, 500);
    CHECK_NEAR(f.view(1).radius, 12, 1e-4);
    f.drag(600, 300, 600, 400);
    CHECK_NEAR(f.view(2).radius, 100, 1e-4);
    CHECK_NEAR(f.view(2).p0.y, 300, 1e-4);
    // 区域内命中
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.click(850, 650);
    f.move(310, 310);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
}

TEST(session_select_move_shape_and_undo) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.drag(200, 200, 300, 260);
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.click(500, 500); // 空白处取消选中
    CHECK_EQ(f.st().selected_shape, 0);
    f.move(250, 250);
    CHECK_EQ(f.st().cursor, LS_CURSOR_MOVE); // 空心矩形内部不算命中
    int ev = f.move(200, 230);
    CHECK(ev & LS_EV_CURSOR);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
    CHECK_EQ(f.view(0).hovered, 1);
    f.down(200, 230);
    CHECK(f.st().selected_shape != 0); // 按下即选中
    f.move(225, 245);
    CHECK_EQ(f.st().dragging_shape, 1);
    f.move(250, 260);
    f.up(250, 260);
    ls_shape_view v = f.view(0);
    CHECK_NEAR(v.p0.x, 250, 1e-4);
    CHECK_NEAR(v.p0.y, 230, 1e-4);
    CHECK(rect_is(f.st().selection, 100, 100, 600, 500)); // 选区没动
    ls_session_undo(f.s);
    CHECK_NEAR(f.view(0).p0.x, 200, 1e-4);
    // 单击标注不移动、不记撤销
    const int undo_before = f.st().can_redo;
    f.click(200, 230);
    CHECK_NEAR(f.view(0).p0.x, 200, 1e-4);
    CHECK_EQ(f.st().can_redo, undo_before);
    // 有工具时按在标注上也是选中并移动（(200,210) 在左边上、不在控制点上）
    ls_session_set_tool(f.s, LS_TOOL_ELLIPSE);
    f.drag(200, 210, 210, 220);
    CHECK_EQ(f.count(), 1);
    CHECK_NEAR(f.view(0).p0.x, 210, 1e-4);
    CHECK_NEAR(f.view(0).p0.y, 210, 1e-4);
}

TEST(session_reshape_rect_and_line) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.drag(200, 200, 300, 260);
    ls_point hp[9];
    CHECK_EQ(ls_session_shape_handles(f.s, hp, 9), 8);
    CHECK_NEAR(hp[4].x, 300, 1e-4);
    CHECK_NEAR(hp[4].y, 260, 1e-4);
    f.move(300, 260);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_NWSE);
    f.drag(300, 260, 350, 300);
    ls_shape_view v = f.view(0);
    CHECK_NEAR(v.p1.x, 350, 1e-4);
    CHECK_NEAR(v.p1.y, 300, 1e-4);
    // 拉过对边翻转
    f.drag(350, 300, 150, 150);
    v = f.view(0);
    CHECK_NEAR(v.p0.x, 150, 1e-4);
    CHECK_NEAR(v.p0.y, 150, 1e-4);
    CHECK_NEAR(v.p1.x, 200, 1e-4);
    CHECK_NEAR(v.p1.y, 200, 1e-4);
    // 边中点只改一个方向
    f.drag(175, 200, 175, 230);
    v = f.view(0);
    CHECK_NEAR(v.p1.y, 230, 1e-4);
    CHECK_NEAR(v.p1.x, 200, 1e-4);
    ls_session_undo(f.s);
    CHECK_NEAR(f.view(0).p1.y, 200, 1e-4);
    // 直线端点
    ls_session_set_tool(f.s, LS_TOOL_LINE);
    f.drag(400, 400, 500, 400);
    f.drag(400, 400, 420, 450);
    v = f.view(1);
    CHECK_NEAR(v.p0.x, 420, 1e-4);
    CHECK_NEAR(v.p0.y, 450, 1e-4);
    CHECK_NEAR(v.p1.x, 500, 1e-4);
}

// ================================================================ 撤销、滚轮、样式

TEST(session_undo_redo_stack) {
    Fx f;
    f.select(100, 100, 800, 600);
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    CHECK_EQ(f.st().can_undo, 0);
    int ev = f.drag(150, 150, 170, 170);
    CHECK(ev & LS_EV_HISTORY);
    f.drag(200, 150, 220, 170);
    f.drag(250, 150, 270, 170);
    CHECK_EQ(f.count(), 3);
    ev = ls_session_undo(f.s);
    CHECK(ev & LS_EV_HISTORY);
    CHECK(ev & LS_EV_REDRAW);
    ls_session_undo(f.s);
    CHECK_EQ(f.count(), 1);
    CHECK_EQ(f.st().can_redo, 1);
    ls_session_redo(f.s);
    CHECK_EQ(f.count(), 2);
    f.key('Z', PRIMARY);
    CHECK_EQ(f.count(), 1);
    f.key('Z', PRIMARY | SHIFT);
    CHECK_EQ(f.count(), 2);
    f.key('Z', PRIMARY);
    f.key('Y', PRIMARY);
    CHECK_EQ(f.count(), 2);
    // 新的提交清空重做
    f.drag(300, 150, 320, 170);
    CHECK_EQ(f.st().can_redo, 0);
    // 最多 100 步
    Fx g;
    g.select(0, 0, 1000, 800);
    ls_session_set_tool(g.s, LS_TOOL_RECT);
    for (int i = 0; i < 105; ++i) {
        const float x = 20 + (float)(i % 30) * 30, y = 20 + (float)(i / 30) * 30;
        g.drag(x, y, x + 10, y + 10);
    }
    CHECK_EQ(g.count(), 105);
    int n = 0;
    while (g.st().can_undo) {
        ls_session_undo(g.s);
        ++n;
    }
    CHECK_EQ(n, 100);
    CHECK_EQ(g.count(), 5);
    // 删除键删除选中标注，可撤销
    f.click(310, 150);
    CHECK(f.st().selected_shape != 0);
    f.key(LS_KEY_DELETE);
    CHECK_EQ(f.count(), 2);
    f.key(LS_KEY_BACKSPACE); // 没有选中：无效
    CHECK_EQ(f.count(), 2);
    ls_session_undo(f.s);
    CHECK_EQ(f.count(), 3);
    f.click(310, 150);
    ev = ls_session_delete_selected(f.s);
    CHECK(ev & LS_EV_REDRAW);
    CHECK_EQ(f.count(), 2);
}

TEST(session_wheel_styles) {
    Fx f;
    f.select(100, 100, 800, 600);
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    int ev = ls_session_scroll(f.s, 500, 500, 1, 0);
    CHECK(ev & LS_EV_TOOL);
    CHECK_EQ(f.style(LS_TOOL_RECT).line_width, 4);
    ls_session_scroll(f.s, 500, 500, -10, 0);
    CHECK_EQ(f.style(LS_TOOL_RECT).line_width, 1);
    ls_session_scroll(f.s, 500, 500, 0.5f, 0);
    CHECK_EQ(f.style(LS_TOOL_RECT).line_width, 1);
    ls_session_scroll(f.s, 500, 500, 0.5f, 0);
    CHECK_EQ(f.style(LS_TOOL_RECT).line_width, 2);
    for (int i = 0; i < 40; ++i) ls_session_scroll(f.s, 500, 500, 1, 0);
    CHECK_EQ(f.style(LS_TOOL_RECT).line_width, 30);
    CHECK_EQ(f.st().can_undo, 0); // 没有选中标注：只改工具样式，不记撤销
    // 马赛克：块大小 ±1（2–60），画笔样式笔刷 ±2（4–120）
    ls_session_set_tool(f.s, LS_TOOL_MOSAIC);
    ls_session_scroll(f.s, 500, 500, -3, 0);
    CHECK_EQ(f.style(LS_TOOL_MOSAIC).block_size, 7);
    f.set_style(LS_TOOL_MOSAIC, [](ls_style &s) { s.variant = 3; });
    ls_session_scroll(f.s, 500, 500, 2, 0);
    CHECK_EQ(f.style(LS_TOOL_MOSAIC).line_width, 28);
    CHECK_EQ(f.style(LS_TOOL_MOSAIC).block_size, 7);
    ls_session_set_tool(f.s, LS_TOOL_TEXT);
    ls_session_scroll(f.s, 500, 500, 2, 0);
    CHECK_EQ(f.style(LS_TOOL_TEXT).font_size, 20);
    ls_session_set_tool(f.s, LS_TOOL_STEP);
    ls_session_scroll(f.s, 500, 500, -5, 0);
    CHECK_EQ(f.style(LS_TOOL_STEP).next_number, 1);
    ls_session_scroll(f.s, 500, 500, 4, 0);
    CHECK_EQ(f.style(LS_TOOL_STEP).next_number, 5);
    ls_session_set_tool(f.s, LS_TOOL_MAGNIFY);
    ls_session_scroll(f.s, 500, 500, 1, 0);
    CHECK_NEAR(f.style(LS_TOOL_MAGNIFY).zoom, 2.5, 1e-5);
    ls_session_scroll(f.s, 500, 500, -10, 0);
    CHECK_NEAR(f.style(LS_TOOL_MAGNIFY).zoom, 1.5, 1e-5);
    ls_session_set_tool(f.s, LS_TOOL_HIGHLIGHT);
    ls_session_scroll(f.s, 500, 500, -1, 0);
    CHECK_NEAR(f.style(LS_TOOL_HIGHLIGHT).opacity, 0.5, 1e-5);
    // 没有工具且不在标注上：无效
    ls_session_set_tool(f.s, LS_TOOL_HIGHLIGHT);
    CHECK_EQ(f.st().tool, LS_TOOL_NONE);
    CHECK_EQ(ls_session_scroll(f.s, 500, 500, 1, 0) & LS_EV_TOOL, 0);
}

TEST(session_wheel_opacity_merge_and_selected_shape) {
    Fx f;
    f.select(100, 100, 800, 600);
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.drag(200, 200, 300, 300);
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    // 光标在标注上：不透明度 ±0.05，连续滚动合并为一次撤销
    f.move(200, 250);
    ls_session_scroll(f.s, 200, 250, -1, 0);
    ls_session_scroll(f.s, 200, 250, -1, 0);
    ls_session_scroll(f.s, 200, 250, -1, 0);
    CHECK_NEAR(f.view(0).style.opacity, 0.85, 1e-5);
    for (int i = 0; i < 30; ++i) ls_session_scroll(f.s, 200, 250, -1, 0);
    CHECK_NEAR(f.view(0).style.opacity, 0.05, 1e-5);
    ls_session_undo(f.s);
    CHECK_NEAR(f.view(0).style.opacity, 1, 1e-5);
    CHECK_EQ(f.count(), 1);
    // 滚轮调工具样式同时作用于选中的同类标注
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.click(200, 250);
    CHECK(f.st().selected_shape != 0);
    ls_session_scroll(f.s, 600, 600, 1, 0);
    ls_session_scroll(f.s, 600, 600, 1, 0);
    CHECK_EQ(f.view(0).style.line_width, 5);
    CHECK_EQ(f.style(LS_TOOL_RECT).line_width, 5);
    ls_session_undo(f.s);
    CHECK_EQ(f.view(0).style.line_width, 3);
}

TEST(session_set_style_merge) {
    Fx f;
    f.select(100, 100, 800, 600);
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.drag(200, 200, 300, 300);
    // 拖滑块：同一字段连续改合并为一条撤销
    f.set_style(LS_TOOL_RECT, [](ls_style &s) { s.line_width = 5; });
    f.set_style(LS_TOOL_RECT, [](ls_style &s) { s.line_width = 6; });
    CHECK_EQ(f.view(0).style.line_width, 6);
    // 换颜色：另起一条
    f.set_style(LS_TOOL_RECT, [](ls_style &s) { s.color = ls_color{0, 0x7A, 0xFF, 0xFF}; });
    CHECK_EQ(f.view(0).style.color.b, 0xFF);
    ls_session_undo(f.s);
    CHECK_EQ(f.view(0).style.color.r, 0xFF);
    CHECK_EQ(f.view(0).style.line_width, 6);
    ls_session_undo(f.s);
    CHECK_EQ(f.view(0).style.line_width, 3);
    ls_session_undo(f.s);
    CHECK_EQ(f.count(), 0);
    // 没选中标注时只改工具样式
    f.set_style(LS_TOOL_ELLIPSE, [](ls_style &s) { s.opacity = 0.01f; });
    CHECK_NEAR(f.style(LS_TOOL_ELLIPSE).opacity, 0.05, 1e-6); // 夹到 0.05..1
    CHECK_EQ(f.st().can_undo, 0);
}

// ================================================================ 键盘、动作、右键

TEST(session_keys_selecting_phase) {
    Fx f;
    f.move(500, 400);
    f.key('R');
    f.key('H');
    f.key('C');
    std::vector<int> a = f.actions();
    CHECK_EQ(a.size(), 3);
    if (a.size() == 3) {
        CHECK_EQ(a[0], LS_ACTION_COPY_RGB);
        CHECK_EQ(a[1], LS_ACTION_COPY_HEX);
        CHECK_EQ(a[2], LS_ACTION_COPY_CUSTOM);
    }
    int ev = f.key('D'); // 没有上次区域
    CHECK(ev & LS_EV_ACTION);
    CHECK(has(f.actions(), LS_ACTION_LAST_AREA));
    CHECK_EQ(f.st().phase, LS_PHASE_SELECTING);
    ls_session_set_last_area(f.s, ls_rect{900, 700, 300, 300});
    f.key('D');
    CHECK_EQ(f.st().phase, LS_PHASE_SELECTED);
    CHECK(rect_is(f.st().selection, 900, 700, 100, 100));
    // 右键：SELECTING 阶段直接退出
    Fx g;
    g.move(10, 10);
    g.down(10, 10, 0, 1, LS_BUTTON_RIGHT);
    CHECK(has(g.actions(), LS_ACTION_CANCEL));
    // F 整屏
    Fx h;
    h.move(10, 10);
    h.key('F');
    CHECK(rect_is(h.st().selection, 0, 0, 1000, 800));
    // 回车：选悬停区域并完成
    Fx k;
    set_windows(k, {{1, {50, 50, 300, 200}}});
    k.move(100, 100);
    ev = k.key(LS_KEY_ENTER);
    CHECK(ev & LS_EV_SELECTION);
    CHECK(rect_is(k.st().selection, 50, 50, 300, 200));
    CHECK(has(k.actions(), LS_ACTION_DONE));
    // Esc
    Fx e;
    e.key(LS_KEY_ESCAPE);
    CHECK(has(e.actions(), LS_ACTION_CANCEL));
}

TEST(session_keys_selected_phase_and_bindings) {
    Fx f;
    f.select(100, 100, 600, 500);
    int ev = f.key('1');
    CHECK(ev & LS_EV_TOOL);
    CHECK_EQ(f.st().tool, LS_TOOL_RECT);
    f.key('1');
    CHECK_EQ(f.st().tool, LS_TOOL_NONE);
    f.key('0');
    CHECK_EQ(f.st().tool, LS_TOOL_MAGNIFY);
    f.key('9');
    CHECK_EQ(f.st().tool, LS_TOOL_HIGHLIGHT);
    // 改绑定
    ls_session_bind_key(f.s, LS_BIND_TOOL_RECT, 'Q');
    f.key('1');
    CHECK_EQ(f.st().tool, LS_TOOL_HIGHLIGHT);
    f.key('Q');
    CHECK_EQ(f.st().tool, LS_TOOL_RECT);
    ls_session_bind_key(f.s, LS_BIND_OCR, 0); // 解绑
    f.key('S');
    f.key('O');
    f.key('T');
    f.key('P');
    f.key('V');
    std::vector<int> a = f.actions();
    CHECK_EQ(a.size(), 4);
    if (a.size() == 4) {
        CHECK_EQ(a[0], LS_ACTION_LONGSHOT);
        CHECK_EQ(a[1], LS_ACTION_TRANSLATE);
        CHECK_EQ(a[2], LS_ACTION_PIN);
        CHECK_EQ(a[3], LS_ACTION_RECORD);
    }
    // Tab 显隐工具栏
    ev = f.key(LS_KEY_TAB);
    CHECK(ev & LS_EV_REDRAW);
    CHECK_EQ(f.st().toolbar_visible, 0);
    f.key(LS_KEY_TAB);
    CHECK_EQ(f.st().toolbar_visible, 1);
    ls_session_set_toolbar_visible(f.s, 0);
    CHECK_EQ(f.st().toolbar_visible, 0);
    // 主修饰键组合不触发单键绑定
    f.key('C', PRIMARY);
    f.key('S', PRIMARY);
    f.key('C', LS_MOD_CTRL); // Mac 上 Control+C：既不是主修饰键也不是单键
    f.key(LS_KEY_ENTER);
    f.key(LS_KEY_ESCAPE);
    a = f.actions();
    CHECK_EQ(a.size(), 4);
    if (a.size() == 4) {
        CHECK_EQ(a[0], LS_ACTION_COPY);
        CHECK_EQ(a[1], LS_ACTION_SAVE);
        CHECK_EQ(a[2], LS_ACTION_DONE);
        CHECK_EQ(a[3], LS_ACTION_CANCEL);
    }
    CHECK_EQ(ls_session_take_action(f.s), LS_ACTION_NONE);
    // F、D 在没有标注时可用，有标注时无效
    f.key('F');
    CHECK(rect_is(f.st().selection, 0, 0, 1000, 800));
    f.select(100, 100, 600, 500);
    CHECK_EQ(f.st().tool, LS_TOOL_RECT);
    f.drag(200, 200, 300, 300);
    f.key('F');
    ls_session_set_last_area(f.s, ls_rect{10, 10, 50, 50});
    f.key('D');
    CHECK(rect_is(f.st().selection, 100, 100, 600, 500));
    CHECK(f.actions().empty());
    // 双击选区（无工具、不在标注上）
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    CHECK_EQ(f.st().tool, LS_TOOL_NONE);
    f.down(500, 500);
    f.up(500, 500);
    ev = f.down(500, 500, 0, 2);
    f.up(500, 500);
    CHECK(ev & LS_EV_ACTION);
    CHECK(has(f.actions(), LS_ACTION_DOUBLE_CLICK));
}

TEST(session_right_click) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.drag(200, 200, 300, 300);
    // 正在画：放弃这一笔
    f.move(400, 400);
    f.down(400, 400);
    f.move(450, 450);
    CHECK_EQ(f.count(), 2);
    f.down(450, 450, 0, 1, LS_BUTTON_RIGHT);
    CHECK_EQ(f.count(), 1);
    f.move(480, 480);
    f.up(480, 480);
    CHECK_EQ(f.count(), 1);
    CHECK_EQ(f.st().phase, LS_PHASE_SELECTED);
    CHECK(f.actions().empty());
    // 拖选区时右键：恢复原状
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.move(700, 600);
    f.down(700, 600);
    f.move(750, 650);
    f.down(750, 650, 0, 1, LS_BUTTON_RIGHT);
    f.up(750, 650);
    CHECK(rect_is(f.st().selection, 100, 100, 600, 500));
    // 默认：清空选区和全部标注，回到 SELECTING
    int ev = f.down(10, 10, 0, 1, LS_BUTTON_RIGHT);
    CHECK(ev & LS_EV_SELECTION);
    ls_session_state st = f.st();
    CHECK_EQ(st.phase, LS_PHASE_SELECTING);
    CHECK_EQ(st.has_selection, 0);
    CHECK_EQ(st.shape_count, 0);
    CHECK_EQ(st.can_undo, 0);
    CHECK(f.actions().empty());
    // right_click_exits
    Opt o;
    o.rce = 1;
    Fx g(o);
    g.select(100, 100, 600, 500);
    g.down(10, 10, 0, 1, LS_BUTTON_RIGHT);
    CHECK(has(g.actions(), LS_ACTION_CANCEL));
}

// ================================================================ 其他

TEST(session_initial_selection_and_pixels) {
    Opt o;
    o.scale = 1.25f;
    o.init = true;
    o.init_rect = ls_rect{10.3f, 10.3f, 100.1f, 50.2f};
    Fx f(o);
    ls_session_state st = f.st();
    CHECK_EQ(st.phase, LS_PHASE_SELECTED);
    CHECK(aligned(st.selection.x, 1.25f) && aligned(st.selection.w, 1.25f));
    int32_t x, y, w, h;
    ls_session_selection_pixels(f.s, &x, &y, &w, &h);
    CHECK_EQ(x, 13);
    CHECK_EQ(y, 13);
    CHECK_EQ(w, 138 - 13);
    CHECK_EQ(h, 76 - 13);
    // 提示条改宽高
    int ev = f.select(10, 10, 2000, 50);
    CHECK(ev & LS_EV_SELECTION);
    CHECK(rect_is(f.st().selection, 0, 10.4f, 1000, 49.6f, 0.01f));
    ls_session_select_fullscreen(f.s);
    CHECK(rect_is(f.st().selection, 0, 0, 1000, 800));
}

TEST(session_placement) {
    Opt o;
    o.scale = 1;
    Fx f(o);
    // 没有选区：尺寸提示在光标右上方
    f.move(500, 400);
    CHECK(rect_is(ls_session_place_size_tip(f.s, 80, 24, 6), 506, 370, 80, 24));
    // 放大镜：光标右下，超屏翻到左/上
    CHECK(rect_is(ls_session_place_magnifier(f.s, 120, 184, 20), 520, 420, 120, 184));
    f.move(950, 700);
    CHECK(rect_is(ls_session_place_magnifier(f.s, 120, 184, 20), 810, 496, 120, 184));
    f.select(200, 150, 400, 300);
    CHECK(rect_is(ls_session_place_toolbar(f.s, 300, 40, 8, 6), 300, 458, 300, 40));
    CHECK(rect_is(ls_session_place_size_tip(f.s, 80, 24, 6), 200, 120, 80, 24));
    ls_rect tb = ls_session_place_toolbar(f.s, 300, 40, 8, 6);
    CHECK(rect_is(ls_session_place_popover(f.s, tb, 350, 200, 100, 6, 6), 334, 504, 200, 100));
    CHECK(rect_is(ls_session_place_popover(f.s, tb, 350, 200, 400, 6, 6), 334, 52, 200, 400));
    CHECK(rect_is(ls_session_place_popover(f.s, tb, 990, 200, 100, 6, 6), 794, 504, 200, 100));
    // 下方放不下：放上方
    f.select(200, 500, 400, 290);
    tb = ls_session_place_toolbar(f.s, 300, 40, 8, 6);
    CHECK(rect_is(tb, 300, 452, 300, 40));
    CHECK(rect_is(ls_session_place_popover(f.s, tb, 350, 200, 100, 6, 6), 334, 346, 200, 100));
    CHECK(rect_is(ls_session_place_popover(f.s, tb, 350, 200, 500, 6, 6), 334, 498, 200, 500));
    // 上下都放不下：选区内右下角，x 夹在屏内
    f.select(0, 0, 1000, 800);
    CHECK(rect_is(ls_session_place_toolbar(f.s, 300, 40, 8, 6), 694, 752, 300, 40));
    f.select(10, 100, 100, 100);
    CHECK(rect_is(ls_session_place_toolbar(f.s, 300, 40, 8, 6), 6, 208, 300, 40));
    // 尺寸提示在顶边放不下：放选区内左上角
    f.select(200, 10, 400, 300);
    CHECK(rect_is(ls_session_place_size_tip(f.s, 80, 24, 6), 206, 16, 80, 24));
}

TEST(session_pixel_at_and_events) {
    Fx f; // 底图：B = x&255, G = y&255, R = (x+y)&255（像素坐标）
    ls_color c = ls_session_pixel_at(f.s, 10.2f, 20.7f);
    CHECK_EQ(c.b, 20);
    CHECK_EQ(c.g, 41);
    CHECK_EQ(c.r, 61);
    CHECK_EQ(c.a, 255);
    c = ls_session_pixel_at(f.s, 5000, -3);
    CHECK_EQ(c.b, (1999 & 255));
    CHECK_EQ(c.g, 0);
    // SELECTING 阶段移动都要重画（放大镜跟随）
    CHECK(f.move(100, 100) & LS_EV_REDRAW);
    CHECK(f.move(101, 100) & LS_EV_REDRAW);
    // SELECTED 阶段没有变化的移动不需要重画
    f.select(100, 100, 600, 500);
    f.move(300, 300);
    CHECK_EQ(f.move(301, 300), 0);
    CHECK_EQ(f.move(302, 300) & LS_EV_CURSOR, 0);
    CHECK(f.move(100, 300) & LS_EV_CURSOR);
    // shape_at 越界给空视图
    ls_shape_view v = f.view(5);
    CHECK_EQ(v.id, 0);
    CHECK(v.fill == nullptr);
}

TEST(session_view_cache_stability) {
    Fx f;
    f.select(100, 100, 600, 500);
    ls_session_set_tool(f.s, LS_TOOL_ARROW);
    f.drag(200, 200, 400, 300);
    ls_shape_view a = f.view(0), b = f.view(0);
    CHECK(a.fill == b.fill);
    ls_point p1[3], p2[3];
    ls_path_get(a.fill, 3, p1);
    // 悬停变化不重建路径
    f.move(600, 550);
    ls_shape_view c = f.view(0);
    ls_path_get(c.fill, 3, p2);
    CHECK(c.fill == a.fill);
    CHECK_EQ(p1[0].x, p2[0].x);
    // 形状变化后重建
    f.key(LS_KEY_RIGHT, ALT);
    ls_shape_view d = f.view(0);
    ls_path_get(d.fill, 3, p2);
    CHECK_NEAR(p2[0].x, p1[0].x + 5, 1e-4);
}

// ================================================================ 区域类标注的命中随工具变化

TEST(session_area_hit_depends_on_tool) {
    Fx f;
    f.select(100, 100, 700, 600);
    ls_session_set_tool(f.s, LS_TOOL_MOSAIC);
    f.drag(200, 200, 400, 350);
    CHECK_EQ(f.count(), 1);
    const int mosaic = f.view(0).id;
    // 选着箭头工具：马赛克内部不算命中，光标是工具的十字，按下拖动新建箭头
    ls_session_set_tool(f.s, LS_TOOL_ARROW);
    f.move(300, 275);
    CHECK_EQ(f.st().cursor, LS_CURSOR_CROSSHAIR);
    CHECK_EQ(f.view(0).hovered, 0);
    f.drag(300, 275, 350, 300);
    CHECK_EQ(f.count(), 2);
    CHECK_EQ(f.view(1).type, LS_TOOL_ARROW);
    CHECK_NEAR(f.view(0).p0.x, 200, 1e-4); // 马赛克没动
    // 边框上（≤ 6pt）：选中并移动马赛克
    f.move(203, 320);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
    CHECK_EQ(f.view(0).hovered, 1);
    f.drag(203, 320, 213, 330);
    CHECK_EQ(f.count(), 2);
    CHECK_EQ(f.st().selected_shape, mosaic);
    CHECK_NEAR(f.view(0).p0.x, 210, 1e-4);
    CHECK_NEAR(f.view(0).p0.y, 210, 1e-4);
    // 边框外侧 6pt 内也算，再远就不算
    f.click(600, 600);
    f.move(205, 260);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
    f.move(203, 260);
    CHECK_EQ(f.st().cursor, LS_CURSOR_CROSSHAIR);
    // 滚轮在选着工具时的马赛克内部调工具样式，不调马赛克
    ls_session_scroll(f.s, 250, 330, 1, 0);
    CHECK_EQ(f.style(LS_TOOL_ARROW).line_width, 5);
    CHECK_NEAR(f.view(0).style.opacity, 1, 1e-6);
    // 工具 NONE：内部按下即选中（并可移动）
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.click(600, 600);
    CHECK_EQ(f.st().selected_shape, 0);
    f.move(250, 330);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
    f.down(250, 330);
    CHECK_EQ(f.st().selected_shape, mosaic);
    f.move(255, 335);
    f.up(255, 335);
    CHECK_NEAR(f.view(0).p0.x, 215, 1e-4);
    CHECK(rect_is(f.st().selection, 100, 100, 700, 600)); // 选区没动
}

TEST(session_area_hit_highlight_magnify_filled) {
    Fx f;
    f.select(0, 0, 1000, 800);
    // 局部高亮内部：选着矩形工具可以直接画
    ls_session_set_tool(f.s, LS_TOOL_HIGHLIGHT);
    f.drag(100, 100, 400, 300);
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.drag(200, 150, 300, 250);
    CHECK_EQ(f.st().shape_count, 2);
    // 放大镜内部：选着画笔可以直接画；圆周附近仍然命中
    ls_session_set_tool(f.s, LS_TOOL_MAGNIFY);
    f.click(600, 300); // 半径 60
    ls_session_set_tool(f.s, LS_TOOL_PEN);
    f.drag(600, 300, 620, 320);
    CHECK_EQ(f.st().shape_count, 4);
    f.move(663, 300);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
    // 实心椭圆：内部可画，轮廓附近命中
    ls_session_set_tool(f.s, LS_TOOL_ELLIPSE);
    f.set_style(LS_TOOL_ELLIPSE, [](ls_style &s) { s.filled = 1; });
    f.drag(100, 500, 300, 700);
    CHECK_EQ(f.st().shape_count, 5);
    f.drag(180, 580, 220, 620);
    CHECK_EQ(f.st().shape_count, 6);
    f.click(900, 750);
    f.move(103, 600);
    CHECK_EQ(f.st().cursor, LS_CURSOR_POINTER);
    // 选中标注的控制点始终优先：选着工具时也能拖高亮的角改形
    ls_session_set_tool(f.s, LS_TOOL_NONE);
    f.click(150, 120); // 工具 NONE：高亮内部即命中并选中
    ls_session_set_tool(f.s, LS_TOOL_RECT);
    f.move(400, 300);
    CHECK_EQ(f.st().cursor, LS_CURSOR_RESIZE_NWSE);
    f.drag(400, 300, 450, 320);
    CHECK_EQ(f.st().shape_count, 6);
    CHECK_EQ(f.view(1).type, LS_TOOL_HIGHLIGHT); // 0 是遮罩
    CHECK_NEAR(f.view(1).p1.x, 450, 1e-4);
    CHECK_NEAR(f.view(1).p1.y, 320, 1e-4);
}
