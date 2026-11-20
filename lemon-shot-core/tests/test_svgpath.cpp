// SVG 路径解析
#include "test.h"

#include <fstream>
#include <sstream>
#include <vector>

namespace {

struct Cmd {
    ls_path_verb verb;
    ls_point pts[3];
};

std::vector<Cmd> parse(const char *d, float scale = 1, float dx = 0, float dy = 0, int32_t *rc = nullptr) {
    ls_path *p = ls_path_create();
    int32_t r = ls_svg_path_parse(d, scale, dx, dy, p);
    if (rc) *rc = r;
    std::vector<Cmd> out;
    for (int32_t i = 0; i < ls_path_count(p); ++i) {
        Cmd c;
        c.verb = ls_path_get(p, i, c.pts);
        out.push_back(c);
    }
    ls_path_destroy(p);
    return out;
}

// 命令的终点
ls_point end_of(const Cmd &c) { return c.verb == LS_PATH_CUBIC ? c.pts[2] : c.pts[0]; }

}  // namespace

TEST(svg_basic_absolute) {
    int32_t rc = 1;
    auto v = parse("M10 20 L30 40", 1, 0, 0, &rc);
    CHECK_EQ(rc, 0);
    CHECK_EQ(v.size(), 2);
    CHECK_EQ(v[0].verb, LS_PATH_MOVE);
    CHECK_EQ(v[0].pts[0].x, 10);
    CHECK_EQ(v[0].pts[0].y, 20);
    CHECK_EQ(v[1].verb, LS_PATH_LINE);
    CHECK_EQ(v[1].pts[0].x, 30);
    CHECK_EQ(v[1].pts[0].y, 40);
}

TEST(svg_relative_and_hv) {
    auto v = parse("m10 10 l5 5 h10 v-5 z");
    CHECK_EQ(v.size(), 5);
    CHECK_EQ(end_of(v[1]).x, 15);
    CHECK_EQ(end_of(v[1]).y, 15);
    CHECK_EQ(end_of(v[2]).x, 25);
    CHECK_EQ(end_of(v[2]).y, 15);
    CHECK_EQ(end_of(v[3]).x, 25);
    CHECK_EQ(end_of(v[3]).y, 10);
    CHECK_EQ(v[4].verb, LS_PATH_CLOSE);
    // z 之后的相对移动从子路径起点算
    auto w = parse("M5 5 l10 0 z m1 1 l1 0");
    CHECK_EQ(w[3].verb, LS_PATH_MOVE);
    CHECK_EQ(w[3].pts[0].x, 6);
    CHECK_EQ(w[3].pts[0].y, 6);
}

TEST(svg_compact_numbers_and_implicit_repeat) {
    auto v = parse("M10-5L20.5.5");
    CHECK_EQ(v.size(), 2);
    CHECK_EQ(v[0].pts[0].y, -5);
    CHECK_NEAR(v[1].pts[0].x, 20.5, 1e-6);
    CHECK_NEAR(v[1].pts[0].y, 0.5, 1e-6);
    // M 后面多出的坐标按 L 处理；m 后面按 l
    auto w = parse("M0 0 10 10 20 0");
    CHECK_EQ(w.size(), 3);
    CHECK_EQ(w[1].verb, LS_PATH_LINE);
    CHECK_EQ(w[2].pts[0].x, 20);
    auto r = parse("m1 1 2 2 3 3");
    CHECK_EQ(r[2].pts[0].x, 6);
    CHECK_EQ(r[2].pts[0].y, 6);
    // 指数写法
    auto e = parse("M1e1 2E-1");
    CHECK_NEAR(e[0].pts[0].x, 10, 1e-6);
    CHECK_NEAR(e[0].pts[0].y, 0.2, 1e-6);
}

TEST(svg_curves) {
    // Q 升三次
    auto q = parse("M0 0 Q10 10 20 0");
    CHECK_EQ(q[1].verb, LS_PATH_CUBIC);
    CHECK_NEAR(q[1].pts[0].x, 20.0 / 3, 1e-4);
    CHECK_NEAR(q[1].pts[0].y, 20.0 / 3, 1e-4);
    CHECK_NEAR(q[1].pts[1].x, 40.0 / 3, 1e-4);
    CHECK_NEAR(q[1].pts[2].x, 20, 1e-6);
    // T 反射上一个二次控制点：(30,-10)
    auto t = parse("M0 0 Q10 10 20 0 T40 0");
    CHECK_NEAR(t[2].pts[0].x, 20 + 2.0 / 3 * 10, 1e-4);
    CHECK_NEAR(t[2].pts[0].y, -20.0 / 3, 1e-4);
    // S 反射上一个三次控制点
    auto s = parse("M0 0 C0 10 10 10 10 0 S20 -10 20 0");
    CHECK_NEAR(s[2].pts[0].x, 10, 1e-6);
    CHECK_NEAR(s[2].pts[0].y, -10, 1e-6);
    // 相对 c
    auto c = parse("M10 10 c1 2 3 4 5 6");
    CHECK_NEAR(c[1].pts[2].x, 15, 1e-6);
    CHECK_NEAR(c[1].pts[2].y, 16, 1e-6);
}

TEST(svg_arcs) {
    // 半圆：(0,0) -> (20,0)，sweep=1 经过 (10,-10)
    auto a = parse("M0 0 A10 10 0 0 1 20 0");
    CHECK_EQ(a.size(), 3);
    CHECK_EQ(a[1].verb, LS_PATH_CUBIC);
    CHECK_NEAR(end_of(a[1]).x, 10, 1e-3);
    CHECK_NEAR(end_of(a[1]).y, -10, 1e-3);
    CHECK_NEAR(end_of(a[2]).x, 20, 1e-3);
    CHECK_NEAR(end_of(a[2]).y, 0, 1e-3);
    // sweep=0 走另一边
    auto b = parse("M0 0 A10 10 0 0 0 20 0");
    CHECK_NEAR(end_of(b[1]).y, 10, 1e-3);
    // 标志位紧凑写法
    int32_t rc = 1;
    auto c = parse("M0 0a5 5 0 0110 0", 1, 0, 0, &rc);
    CHECK_EQ(rc, 0);
    CHECK_NEAR(end_of(c.back()).x, 10, 1e-3);
    CHECK_NEAR(end_of(c.back()).y, 0, 1e-3);
    // 半径太小自动放大
    auto d = parse("M0 0 A1 1 0 0 1 20 0");
    CHECK_NEAR(end_of(d.back()).x, 20, 1e-3);
    // 半径为 0 退化为直线
    auto e = parse("M0 0 A0 5 0 0 1 20 0");
    CHECK_EQ(e[1].verb, LS_PATH_LINE);
    // 整圆（两段半圆，图标里常见）
    auto f = parse("M2 12A10 10 0 1 0 22 12A10 10 0 1 0 2 12Z");
    CHECK_NEAR(end_of(f[f.size() - 2]).x, 2, 1e-3);
}

TEST(svg_scale_offset_and_errors) {
    auto v = parse("M1 1 L2 3", 2, 10, 20);
    CHECK_NEAR(v[0].pts[0].x, 12, 1e-6);
    CHECK_NEAR(v[0].pts[0].y, 22, 1e-6);
    CHECK_NEAR(v[1].pts[0].x, 14, 1e-6);
    CHECK_NEAR(v[1].pts[0].y, 26, 1e-6);
    int32_t rc = 0;
    auto e = parse("M 10", 1, 0, 0, &rc);
    CHECK_EQ(rc, -1);
    parse("X 1 2", 1, 0, 0, &rc);
    CHECK_EQ(rc, -1);
    // 出错前已解析的部分保留
    auto k = parse("M1 2 L3 4 L5", 1, 0, 0, &rc);
    CHECK_EQ(rc, -1);
    CHECK_EQ(k.size(), 2);
}

TEST(svg_all_icons_parse) {
    std::ifstream f(lst::fixtures() + "/../../resources/icons.json");
    CHECK(f.good());
    if (!f.good()) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    int count = 0, bad = 0;
    size_t pos = 0;
    while ((pos = s.find("\"d\":", pos)) != std::string::npos) {
        size_t q0 = s.find('"', pos + 4);
        size_t q1 = s.find('"', q0 + 1);
        const std::string d = s.substr(q0 + 1, q1 - q0 - 1);
        ls_path *p = ls_path_create();
        if (ls_svg_path_parse(d.c_str(), 1, 0, 0, p) != 0 || ls_path_count(p) < 2) {
            ++bad;
            std::printf("    图标路径解析失败：%s\n", d.c_str());
        }
        ls_path_destroy(p);
        ++count;
        pos = q1;
    }
    CHECK(count > 50);
    CHECK_EQ(bad, 0);
}
