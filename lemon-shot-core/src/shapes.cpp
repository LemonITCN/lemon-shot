// 标注几何：路径、命中、控制点、绘制数据、马赛克/模糊 patch。规则见 docs/session-behavior.md「几何」。
#include "session_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lsk {

namespace {

const float kKappa = 0.5522847498f;

ls_point add(ls_point a, ls_point b) { return pt(a.x + b.x, a.y + b.y); }
ls_point sub(ls_point a, ls_point b) { return pt(a.x - b.x, a.y - b.y); }
ls_point mul(ls_point a, float k) { return pt(a.x * k, a.y * k); }
ls_point lerp(ls_point a, ls_point b, float t) { return pt(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t); }
ls_point rotate(ls_point v, float deg) {
    const float r = (float)(deg * kPi / 180.0);
    const float c = std::cos(r), s = std::sin(r);
    return pt(v.x * c - v.y * s, v.x * s + v.y * c);
}

ls_point quad_at(ls_point p0, ls_point c, ls_point p1, float t) {
    const float u = 1 - t;
    return pt(u * u * p0.x + 2 * u * t * c.x + t * t * p1.x, u * u * p0.y + 2 * u * t * c.y + t * t * p1.y);
}

// 二次曲线上离 p1 距离为 d 的点的参数（从终点往回找）
float quad_t_at_distance(ls_point p0, ls_point c, ls_point p1, float d) {
    if (dist(p0, p1) <= d && dist(p0, c) + dist(c, p1) <= d) return 0;
    float lo = 0, hi = 1;
    for (int i = 0; i < 30; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (dist(quad_at(p0, c, p1, mid), p1) > d)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

void quad_to_cubic(ls_path &p, ls_point from, ls_point c, ls_point to) {
    const ls_point c1 = add(from, mul(sub(c, from), 2.0f / 3));
    const ls_point c2 = add(to, mul(sub(c, to), 2.0f / 3));
    p.cubic_to(c1.x, c1.y, c2.x, c2.y, to.x, to.y);
}

void add_polygon(ls_path &p, const ls_point *pts, int n) {
    p.move_to(pts[0].x, pts[0].y);
    for (int i = 1; i < n; ++i) p.line_to(pts[i].x, pts[i].y);
    p.close();
}

void add_rect_path(ls_path &p, ls_rect r, float rad) {
    rad = clampf(rad, 0, std::min(r.w, r.h) * 0.5f);
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    if (rad <= 0) {
        p.move_to(x0, y0);
        p.line_to(x1, y0);
        p.line_to(x1, y1);
        p.line_to(x0, y1);
        p.close();
        return;
    }
    const float k = rad * (1 - kKappa);
    p.move_to(x0 + rad, y0);
    p.line_to(x1 - rad, y0);
    p.cubic_to(x1 - k, y0, x1, y0 + k, x1, y0 + rad);
    p.line_to(x1, y1 - rad);
    p.cubic_to(x1, y1 - k, x1 - k, y1, x1 - rad, y1);
    p.line_to(x0 + rad, y1);
    p.cubic_to(x0 + k, y1, x0, y1 - k, x0, y1 - rad);
    p.line_to(x0, y0 + rad);
    p.cubic_to(x0, y0 + k, x0 + k, y0, x0 + rad, y0);
    p.close();
}

void add_ellipse_path(ls_path &p, ls_rect r) {
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, rx = r.w * 0.5f, ry = r.h * 0.5f;
    const float kx = rx * kKappa, ky = ry * kKappa;
    p.move_to(cx + rx, cy);
    p.cubic_to(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    p.cubic_to(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    p.cubic_to(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    p.cubic_to(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    p.close();
}

// 中点二次平滑后升三次
void add_smooth_path(ls_path &p, const std::vector<ls_point> &pts) {
    const size_t n = pts.size();
    if (!n) return;
    p.move_to(pts[0].x, pts[0].y);
    if (n == 1) {
        p.line_to(pts[0].x, pts[0].y);
        return;
    }
    if (n == 2) {
        p.line_to(pts[1].x, pts[1].y);
        return;
    }
    ls_point from = pts[0];
    for (size_t i = 1; i + 1 < n; ++i) {
        const ls_point mid = lerp(pts[i], pts[i + 1], 0.5f);
        quad_to_cubic(p, from, pts[i], mid);
        from = mid;
    }
    p.line_to(pts[n - 1].x, pts[n - 1].y);
}

ls_rect path_bounds(const ls_path &p, bool *any) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool found = false;
    for (const ls_path_cmd &c : p.verbs) {
        int n = c.verb == LS_PATH_CUBIC ? 3 : (c.verb == LS_PATH_CLOSE ? 0 : 1);
        for (int i = 0; i < n; ++i) {
            x0 = std::min(x0, c.pts[i].x);
            y0 = std::min(y0, c.pts[i].y);
            x1 = std::max(x1, c.pts[i].x);
            y1 = std::max(y1, c.pts[i].y);
            found = true;
        }
    }
    if (any) *any = found;
    if (!found) return rc(0, 0, 0, 0);
    return rc(x0, y0, x1 - x0, y1 - y0);
}

ls_rect inflate(ls_rect r, float d) { return rc(r.x - d, r.y - d, r.w + 2 * d, r.h + 2 * d); }

float arrow_head(float w, float len) { return std::min(std::max(4 * w + 6, 10.0f), 0.6f * len); }

void build_arrow(const Shape &s, ShapeGeom &g) {
    const float w = s.style.line_width;
    const ls_point p0 = s.p0, p1 = s.p1;
    const float len = dist(p0, p1);
    if (len < 1e-3f) return;
    const float h = arrow_head(w, len);
    const ls_point u = mul(sub(p1, p0), 1 / len);
    const ls_point n = pt(-u.y, u.x);
    switch (s.style.variant) {
    case 1:
    case 2: {
        g.stroke.move_to(p0.x, p0.y);
        g.stroke.line_to(p1.x, p1.y);
        const ls_point back = mul(u, -1);
        ls_point a = add(p1, mul(rotate(back, 28), h)), b = add(p1, mul(rotate(back, -28), h));
        g.stroke.move_to(a.x, a.y);
        g.stroke.line_to(p1.x, p1.y);
        g.stroke.line_to(b.x, b.y);
        if (s.style.variant == 2) {
            a = add(p0, mul(rotate(u, 28), h));
            b = add(p0, mul(rotate(u, -28), h));
            g.stroke.move_to(a.x, a.y);
            g.stroke.line_to(p0.x, p0.y);
            g.stroke.line_to(b.x, b.y);
        }
        g.has_stroke = true;
        break;
    }
    case 3: {
        const ls_point c = s.ctrl;
        // 头的朝向：曲线上离终点 h 的点指向终点
        const float th = quad_t_at_distance(p0, c, p1, h);
        ls_point v = sub(p1, quad_at(p0, c, p1, th));
        float vl = std::sqrt(v.x * v.x + v.y * v.y);
        v = vl > 1e-4f ? mul(v, 1 / vl) : u;
        const ls_point nv = pt(-v.y, v.x);
        const ls_point base = sub(p1, mul(v, h));
        const ls_point tri[3] = {p1, add(base, mul(nv, 0.45f * h)), sub(base, mul(nv, 0.45f * h))};
        add_polygon(g.fill, tri, 3);
        g.has_fill = true;
        // 线身画到头里面一点（离尖端 h/2），圆头被三角形盖住
        const float ts = quad_t_at_distance(p0, c, p1, h * 0.5f);
        const ls_point q1 = lerp(p0, c, ts);
        const ls_point q2 = quad_at(p0, c, p1, ts);
        g.stroke.move_to(p0.x, p0.y);
        quad_to_cubic(g.stroke, p0, q1, q2);
        g.has_stroke = true;
        break;
    }
    default: {
        const ls_point base = sub(p1, mul(u, h));
        const float tail = 0.175f * w, shaft = 0.5f * w, head = 0.45f * h;
        const ls_point poly[7] = {add(p0, mul(n, tail)),   add(base, mul(n, shaft)), add(base, mul(n, head)), p1,
                                  sub(base, mul(n, head)), sub(base, mul(n, shaft)), sub(p0, mul(n, tail))};
        add_polygon(g.fill, poly, 7);
        g.has_fill = true;
        break;
    }
    }
}

ls_color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ls_color c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}

const ls_color kWhite = rgba(255, 255, 255, 255);
const ls_color kBlack = rgba(0, 0, 0, 255);
const ls_color kNone = rgba(0, 0, 0, 0);

}  // namespace

// ---------------------------------------------------------------- 基础几何

ls_rect rect_from(ls_point a, ls_point b) {
    return rc(std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y));
}

ls_rect rect_union(ls_rect a, ls_rect b) {
    const float x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const float x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return rc(x0, y0, x1 - x0, y1 - y0);
}

bool rect_contains(ls_rect r, float x, float y) { return x >= r.x && y >= r.y && x <= r.x + r.w && y <= r.y + r.h; }

float dist(ls_point a, ls_point b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); }

float dist_to_segment(ls_point p, ls_point a, ls_point b) {
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0;
    t = clampf(t, 0, 1);
    return dist(p, pt(a.x + vx * t, a.y + vy * t));
}

float luma(ls_color c) { return (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) / 255.0f; }

bool is_rect_shape(const Shape &s) {
    switch (s.type) {
    case LS_TOOL_RECT:
    case LS_TOOL_ELLIPSE:
    case LS_TOOL_HIGHLIGHT: return true;
    case LS_TOOL_MOSAIC: return s.style.variant == 0 || s.style.variant == 2;
    default: return false;
    }
}

bool is_brush_shape(const Shape &s) {
    return s.type == LS_TOOL_PEN || (s.type == LS_TOOL_MOSAIC && (s.style.variant == 1 || s.style.variant == 3));
}

bool is_line_shape(const Shape &s) { return s.type == LS_TOOL_LINE || s.type == LS_TOOL_ARROW; }

float effective_width(const Shape &s) {
    if (s.type == LS_TOOL_PEN && s.style.variant == 1) return s.style.line_width * 3;
    return s.style.line_width;
}

float magnify_radius(const Shape &s) { return std::max(12.0f, dist(s.p0, s.p1)); }

ls_point curve_mid(const Shape &s) { return quad_at(s.p0, s.ctrl, s.p1, 0.5f); }

void default_ctrl(Shape &s) {
    const ls_point m = lerp(s.p0, s.p1, 0.5f);
    const float len = dist(s.p0, s.p1);
    if (len < 1e-4f) {
        s.ctrl = m;
        return;
    }
    // 沿前进方向的左侧（屏幕坐标 y 向下）
    const ls_point u = mul(sub(s.p1, s.p0), 1 / len);
    s.ctrl = add(m, mul(pt(u.y, -u.x), 0.2f * len));
}

// ---------------------------------------------------------------- 命中

namespace {

const float kEdgeHit = 6.0f; // 选着绘图工具时，区域类标注只在边框这么近的范围内算命中

// 矩形区域：edge_only 时只认边框内外 kEdgeHit 的带状区域
bool hit_rect_area(ls_rect r, ls_point p, bool edge_only) {
    if (!edge_only) return rect_contains(r, p.x, p.y);
    if (!rect_contains(inflate(r, kEdgeHit), p.x, p.y)) return false;
    return !(r.w > 2 * kEdgeHit && r.h > 2 * kEdgeHit && rect_contains(inflate(r, -kEdgeHit), p.x, p.y));
}

// 椭圆轮廓（折线近似）到点的距离
float ellipse_outline_dist(ls_rect r, ls_point p) {
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rx = r.w / 2, ry = r.h / 2;
    float best = 1e30f;
    ls_point prev = pt(cx + rx, cy);
    for (int i = 1; i <= 72; ++i) {
        const double a = i * 2 * kPi / 72;
        const ls_point q = pt(cx + rx * (float)std::cos(a), cy + ry * (float)std::sin(a));
        best = std::min(best, dist_to_segment(p, prev, q));
        prev = q;
    }
    return best;
}

float polyline_dist(const std::vector<ls_point> &pts, ls_point p) {
    if (pts.empty()) return 1e30f;
    if (pts.size() == 1) return dist(p, pts[0]);
    float best = 1e30f;
    for (size_t i = 1; i < pts.size(); ++i) best = std::min(best, dist_to_segment(p, pts[i - 1], pts[i]));
    return best;
}

}  // namespace

// edge_only：当前选着绘图工具。区域类标注（实心矩形/椭圆、局部高亮、马赛克、放大镜）只有边框附近算命中，
// 内部留给新标注；描边类、文字、序号不受影响。
bool hit_shape(const Shape &s, ls_point p, bool edge_only) {
    const float tol = std::max(effective_width(s) * 0.5f + 4, 6.0f);
    switch (s.type) {
    case LS_TOOL_RECT:
    case LS_TOOL_ELLIPSE: {
        const ls_rect r = rect_from(s.p0, s.p1);
        if (s.type == LS_TOOL_RECT) {
            if (s.style.filled) return hit_rect_area(r, p, edge_only);
            return rect_contains(inflate(r, tol), p.x, p.y) && !(r.w > 2 * tol && r.h > 2 * tol && rect_contains(inflate(r, -tol), p.x, p.y));
        }
        if (s.style.filled) {
            if (edge_only) return ellipse_outline_dist(r, p) <= kEdgeHit;
            const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rx = r.w / 2, ry = r.h / 2;
            if (rx <= 0 || ry <= 0) return false;
            const float nx = (p.x - cx) / rx, ny = (p.y - cy) / ry;
            return nx * nx + ny * ny <= 1;
        }
        return ellipse_outline_dist(r, p) <= tol;
    }
    case LS_TOOL_LINE: return dist_to_segment(p, s.p0, s.p1) <= tol;
    case LS_TOOL_ARROW: {
        const float h = arrow_head(s.style.line_width, dist(s.p0, s.p1));
        if (dist(p, s.p1) <= std::max(h * 0.6f, tol)) return true;
        if (s.style.variant == 2 && dist(p, s.p0) <= std::max(h * 0.6f, tol)) return true;
        if (s.style.variant == 3) {
            ls_point prev = s.p0;
            for (int i = 1; i <= 32; ++i) {
                const ls_point q = quad_at(s.p0, s.ctrl, s.p1, i / 32.0f);
                if (dist_to_segment(p, prev, q) <= tol) return true;
                prev = q;
            }
            return false;
        }
        return dist_to_segment(p, s.p0, s.p1) <= tol;
    }
    case LS_TOOL_PEN: return polyline_dist(s.pts, p) <= tol;
    case LS_TOOL_MOSAIC: {
        if (is_rect_shape(s)) return hit_rect_area(rect_from(s.p0, s.p1), p, edge_only);
        // 画笔样式：区域 = 笔刷扫过的范围，边框 = 笔刷外沿
        const float r = std::max(s.style.line_width * 0.5f, 4.0f);
        const float d = polyline_dist(s.pts, p);
        return edge_only ? std::fabs(d - r) <= kEdgeHit : d <= r;
    }
    case LS_TOOL_TEXT: return rect_contains(inflate(s.text_rect, 4), p.x, p.y);
    case LS_TOOL_STEP: {
        const float d = s.style.line_width;
        return rect_contains(rc(s.p0.x - d / 2, s.p0.y - d / 2, d, d), p.x, p.y);
    }
    case LS_TOOL_HIGHLIGHT: return hit_rect_area(rect_from(s.p0, s.p1), p, edge_only);
    case LS_TOOL_MAGNIFY: {
        const float d = dist(p, s.p0), r = magnify_radius(s);
        return edge_only ? std::fabs(d - r) <= kEdgeHit : d <= r;
    }
    default: return false;
    }
}

// ---------------------------------------------------------------- 控制点

void rect_handle_points(ls_rect r, ls_point out[8]) {
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h, cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    out[0] = pt(x0, y0);
    out[1] = pt(cx, y0);
    out[2] = pt(x1, y0);
    out[3] = pt(x1, cy);
    out[4] = pt(x1, y1);
    out[5] = pt(cx, y1);
    out[6] = pt(x0, y1);
    out[7] = pt(x0, cy);
}

int rect_handle_cursor(int handle) {
    switch (handle) {
    case 0:
    case 4: return LS_CURSOR_RESIZE_NWSE;
    case 2:
    case 6: return LS_CURSOR_RESIZE_NESW;
    case 1:
    case 5: return LS_CURSOR_RESIZE_NS;
    default: return LS_CURSOR_RESIZE_EW;
    }
}

int shape_handles(const Shape &s, ls_point out[9]) {
    if (is_rect_shape(s)) {
        rect_handle_points(rect_from(s.p0, s.p1), out);
        return 8;
    }
    if (is_line_shape(s)) {
        out[0] = s.p0;
        out[1] = s.p1;
        if (s.type == LS_TOOL_ARROW && s.style.variant == 3) {
            out[2] = curve_mid(s);
            return 3;
        }
        return 2;
    }
    if (s.type == LS_TOOL_MAGNIFY) {
        out[0] = pt(s.p0.x + magnify_radius(s), s.p0.y);
        return 1;
    }
    return 0;
}

int shape_handle_cursor(const Shape &s, int handle) {
    if (is_rect_shape(s)) return rect_handle_cursor(handle);
    if (s.type == LS_TOOL_MAGNIFY) return LS_CURSOR_RESIZE_EW;
    return LS_CURSOR_MOVE;
}

ls_rect drag_rect_handle(ls_rect o, int handle, float dx, float dy) {
    float x0 = o.x, y0 = o.y, x1 = o.x + o.w, y1 = o.y + o.h;
    const bool left = handle == 0 || handle == 6 || handle == 7;
    const bool right = handle == 2 || handle == 3 || handle == 4;
    const bool top = handle == 0 || handle == 1 || handle == 2;
    const bool bottom = handle == 4 || handle == 5 || handle == 6;
    if (left) x0 += dx;
    if (right) x1 += dx;
    if (top) y0 += dy;
    if (bottom) y1 += dy;
    return rect_from(pt(x0, y0), pt(x1, y1));
}

// ---------------------------------------------------------------- 绘制数据

void text_colors(const Shape &s, ls_color *color, ls_color *background, ls_color *outline) {
    const ls_color c = s.style.color;
    const ls_color contrast = luma(c) < 0.5f ? kWhite : kBlack;
    *color = c;
    *background = kNone;
    *outline = kNone;
    if (s.style.variant == 1) {
        *outline = contrast;
    } else if (s.style.variant == 2) {
        *background = c;
        *color = contrast;
    }
}

void build_geom(const Shape &s, ShapeGeom &g) {
    g.fill.clear();
    g.stroke.clear();
    g.has_fill = g.has_stroke = false;
    g.dashed = 0;
    g.vstyle = s.style;
    g.fill_color = s.style.color;
    g.text.clear();
    g.text_rect = rc(0, 0, 0, 0);
    g.text_color = g.text_background = g.text_outline = kNone;
    g.text_size = 0;
    g.magnify_src = rc(0, 0, 0, 0);
    g.radius = 0;
    float stroke_w = s.style.line_width;
    ls_rect extra = rc(0, 0, 0, 0);
    bool has_extra = false;
    switch (s.type) {
    case LS_TOOL_RECT:
    case LS_TOOL_ELLIPSE: {
        const ls_rect r = rect_from(s.p0, s.p1);
        ls_path &p = s.style.filled ? g.fill : g.stroke;
        if (s.type == LS_TOOL_RECT)
            add_rect_path(p, r, s.style.corner_radius);
        else
            add_ellipse_path(p, r);
        (s.style.filled ? g.has_fill : g.has_stroke) = true;
        break;
    }
    case LS_TOOL_LINE:
        g.stroke.move_to(s.p0.x, s.p0.y);
        g.stroke.line_to(s.p1.x, s.p1.y);
        g.has_stroke = true;
        g.dashed = s.style.variant == 1 ? 1 : 0;
        break;
    case LS_TOOL_ARROW: build_arrow(s, g); break;
    case LS_TOOL_PEN:
        add_smooth_path(g.stroke, s.pts);
        g.has_stroke = !s.pts.empty();
        if (s.style.variant == 1) {
            g.vstyle.line_width = s.style.line_width * 3;
            g.vstyle.opacity = s.style.opacity * 0.4f;
            stroke_w = g.vstyle.line_width;
        }
        break;
    case LS_TOOL_MOSAIC: {
        if (is_brush_shape(s)) {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (const ls_point &q : s.pts) {
                x0 = std::min(x0, q.x);
                y0 = std::min(y0, q.y);
                x1 = std::max(x1, q.x);
                y1 = std::max(y1, q.y);
            }
            if (!s.pts.empty()) {
                extra = inflate(rc(x0, y0, x1 - x0, y1 - y0), s.style.line_width * 0.5f);
                has_extra = true;
            }
        } else {
            extra = rect_from(s.p0, s.p1);
            has_extra = true;
        }
        break;
    }
    case LS_TOOL_TEXT: {
        g.text_rect = s.text_rect;
        g.text_size = s.style.font_size;
        text_colors(s, &g.text_color, &g.text_background, &g.text_outline);
        extra = inflate(s.text_rect, 4);
        has_extra = true;
        break;
    }
    case LS_TOOL_STEP: {
        const float d = s.style.line_width;
        const ls_rect r = rc(s.p0.x - d / 2, s.p0.y - d / 2, d, d);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", (int)s.number);
        g.text = buf;
        g.text_rect = r;
        g.text_size = 0.55f * d;
        const ls_color c = s.style.color;
        if (s.style.variant == 1) {
            add_ellipse_path(g.fill, r);
            add_ellipse_path(g.stroke, r);
            g.has_fill = g.has_stroke = true;
            g.fill_color = kWhite;
            g.vstyle.line_width = d / 12;
            stroke_w = d / 12;
            g.text_color = c;
        } else {
            if (s.style.variant == 2)
                add_rect_path(g.fill, r, 0.2f * d);
            else
                add_ellipse_path(g.fill, r);
            g.has_fill = true;
            g.text_color = luma(c) > 0.7f ? kBlack : kWhite;
        }
        extra = r;
        has_extra = true;
        break;
    }
    case LS_TOOL_HIGHLIGHT:
        // 本身不画，统一合成到 LS_SHAPE_MASK
        extra = rect_from(s.p0, s.p1);
        has_extra = true;
        break;
    case LS_TOOL_MAGNIFY: {
        const float r = magnify_radius(s);
        const float zoom = s.style.zoom > 0.1f ? s.style.zoom : 2.0f;
        const float half = r / zoom;
        g.radius = r;
        g.magnify_src = rc(s.p0.x - half, s.p0.y - half, 2 * half, 2 * half);
        add_ellipse_path(g.stroke, rc(s.p0.x - r, s.p0.y - r, 2 * r, 2 * r));
        g.has_stroke = true;
        break;
    }
    default: break;
    }
    // 外接框：路径 + 半个线宽，再留 1pt 抗锯齿余量
    bool any = false;
    ls_rect b = rc(0, 0, 0, 0);
    if (g.has_fill) {
        b = path_bounds(g.fill, &any);
    }
    if (g.has_stroke) {
        bool sany = false;
        ls_rect sb = inflate(path_bounds(g.stroke, &sany), stroke_w * 0.5f);
        if (sany) {
            b = any ? rect_union(b, sb) : sb;
            any = true;
        }
    }
    if (has_extra) {
        b = any ? rect_union(b, extra) : extra;
        any = true;
    }
    g.bounds = any ? inflate(b, 1) : rc(0, 0, 0, 0);
}

void build_mask_geom(ls_rect sel, const std::vector<Shape> &shapes, ShapeGeom &g) {
    g.fill.clear();
    g.stroke.clear();
    g.has_stroke = false;
    g.has_fill = true;
    g.dashed = 0;
    std::memset(&g.vstyle, 0, sizeof g.vstyle);
    g.vstyle.color = kBlack;
    g.fill_color = kBlack;
    g.bounds = sel;
    bool first = true;
    add_rect_path(g.fill, sel, 0);
    for (const Shape &s : shapes) {
        if (s.type != LS_TOOL_HIGHLIGHT) continue;
        if (first) {
            g.vstyle.opacity = s.style.opacity;
            g.vstyle.corner_radius = s.style.corner_radius;
            first = false;
        }
        add_rect_path(g.fill, rect_from(s.p0, s.p1), s.style.corner_radius);
    }
}

// ---------------------------------------------------------------- 马赛克

TileSet::~TileSet() {
    for (ls_image &im : tiles) ls_image_free(&im);
}

TileSet *TileCache::get(int kind, int param) {
    ++clock;
    for (auto &s : sets) {
        if (s->kind == kind && s->param == param) {
            s->last_use = clock;
            return s.get();
        }
    }
    if (!base || !base->data) return nullptr;
    // 最多留 4 组，挤掉最久没用的
    if (sets.size() >= 4) {
        size_t oldest = 0;
        for (size_t i = 1; i < sets.size(); ++i)
            if (sets[i]->last_use < sets[oldest]->last_use) oldest = i;
        sets.erase(sets.begin() + (std::ptrdiff_t)oldest);
    }
    std::unique_ptr<TileSet> ts(new TileSet());
    ts->kind = kind;
    ts->param = param;
    if (kind == 0) {
        const int b = std::max(1, param);
        ts->tile = b * ((256 + b - 1) / b);
    } else {
        // 模糊每块要多算一圈边，块大一点更划算
        const float sigma = param / 16.0f;
        ts->tile = std::max(256, (int)std::ceil(sigma * 12));
    }
    ts->nx = (base->width + ts->tile - 1) / ts->tile;
    ts->ny = (base->height + ts->tile - 1) / ts->tile;
    ts->tiles.resize((size_t)ts->nx * ts->ny, ls_image{nullptr, 0, 0, 0});
    ts->last_use = clock;
    sets.push_back(std::move(ts));
    return sets.back().get();
}

void TileCache::gather(TileSet *ts, int x, int y, int w, int h, ls_image *dst, int dx, int dy) {
    const int T = ts->tile;
    const int tx0 = x / T, tx1 = (x + w - 1) / T, ty0 = y / T, ty1 = (y + h - 1) / T;
    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            ls_image &tile = ts->tiles[(size_t)ty * ts->nx + tx];
            const int ox = tx * T, oy = ty * T;
            const int tw = std::min(T, base->width - ox), th = std::min(T, base->height - oy);
            if (!tile.data) {
                tile = ls_image_alloc(tw, th);
                if (!tile.data) continue;
                if (ts->kind == 0)
                    pixelate_into(base, ox, oy, tw, th, ts->param, &tile, 0, 0);
                else
                    blur_into(base, ox, oy, tw, th, ts->param / 16.0f, &tile, 0, 0);
            }
            const int ix0 = std::max(x, ox), ix1 = std::min(x + w, ox + tw);
            const int iy0 = std::max(y, oy), iy1 = std::min(y + h, oy + th);
            for (int yy = iy0; yy < iy1; ++yy)
                std::memcpy(row_ptr(dst, dy + yy - y) + (dx + ix0 - x) * 4, row_ptr(&tile, yy - oy) + (ix0 - ox) * 4,
                            (size_t)(ix1 - ix0) * 4);
        }
    }
}

namespace {

// 圆形笔刷沿线段，透明度取最大值，边缘 1 像素抗锯齿。a、b 为整张图的像素坐标，(ox,oy) 为 mask 左上角；
// 距离按整图坐标算，同一像素不管 patch 从哪里开始结果都一样（增量更新与整笔重建逐位一致）。
void raster_capsule(std::vector<uint8_t> &mask, int mw, int mh, int ox, int oy, ls_point a, ls_point b, float r, int *dirty) {
    const int x0 = std::max(0, (int)std::floor(std::min(a.x, b.x) - r - 1) - ox);
    const int y0 = std::max(0, (int)std::floor(std::min(a.y, b.y) - r - 1) - oy);
    const int x1 = std::min(mw, (int)std::ceil(std::max(a.x, b.x) + r + 1) - ox);
    const int y1 = std::min(mh, (int)std::ceil(std::max(a.y, b.y) + r + 1) - oy);
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; ++y) {
        uint8_t *m = &mask[(size_t)y * mw];
        for (int x = x0; x < x1; ++x) {
            const float d = dist_to_segment(pt((float)(ox + x) + 0.5f, (float)(oy + y) + 0.5f), a, b);
            const float cov = r - d + 0.5f;
            if (cov <= 0) continue;
            const int v = cov >= 1 ? 255 : (int)(cov * 255 + 0.5f);
            if (v > m[x]) m[x] = (uint8_t)v;
        }
    }
    dirty[0] = std::min(dirty[0], x0);
    dirty[1] = std::min(dirty[1], y0);
    dirty[2] = std::max(dirty[2], x1);
    dirty[3] = std::max(dirty[3], y1);
}

// patch 里 (x0,y0)-(x1,y1) 区域：处理后的像素 × 笔刷透明度
void fill_patch_region(ShapeGeom &g, TileCache &tc, TileSet *ts, int x0, int y0, int x1, int y1, bool use_mask) {
    if (x1 <= x0 || y1 <= y0) return;
    tc.gather(ts, g.px_x + x0, g.px_y + y0, x1 - x0, y1 - y0, &g.patch, x0, y0);
    if (!use_mask) return;
    for (int y = y0; y < y1; ++y) {
        uint8_t *p = row_ptr(&g.patch, y) + x0 * 4;
        const uint8_t *m = &g.mask[(size_t)y * g.patch.width + x0];
        for (int x = x0; x < x1; ++x, p += 4, ++m) {
            const uint32_t a = *m;
            if (a == 255) continue;
            if (a == 0) {
                p[0] = p[1] = p[2] = p[3] = 0;
                continue;
            }
            for (int c = 0; c < 4; ++c) p[c] = (uint8_t)((p[c] * a + 127) / 255);
        }
    }
}

}  // namespace

void build_patch(const Shape &s, ShapeGeom &g, TileCache &tc, float scale, bool drawing) {
    const ls_image *base = tc.base;
    if (!base || !base->data || s.type != LS_TOOL_MOSAIC) {
        ls_image_free(&g.patch);
        g.patch_valid = false;
        g.patch_rect = rc(0, 0, 0, 0);
        return;
    }
    const bool blur = s.style.variant >= 2;
    const float bs = std::max(1.0f, s.style.block_size) * scale;
    const int kind = blur ? 1 : 0;
    const int param = blur ? std::max(1, (int)std::lround(bs * 16)) : std::max(1, (int)std::lround(bs));
    TileSet *ts = tc.get(kind, param);
    const int BW = base->width, BH = base->height;
    if (!ts) return;

    if (!is_brush_shape(s)) {
        const ls_rect r = rect_from(s.p0, s.p1);
        int x0 = (int)std::floor(r.x * scale + 0.5f), y0 = (int)std::floor(r.y * scale + 0.5f);
        int x1 = (int)std::floor((r.x + r.w) * scale + 0.5f), y1 = (int)std::floor((r.y + r.h) * scale + 0.5f);
        x0 = clampi(x0, 0, BW);
        x1 = clampi(x1, 0, BW);
        y0 = clampi(y0, 0, BH);
        y1 = clampi(y1, 0, BH);
        ls_image_free(&g.patch);
        g.mask.clear();
        g.mask_pts = 0;
        g.patch_valid = true;
        g.patch_kind = kind;
        g.patch_param = param;
        g.px_x = x0;
        g.px_y = y0;
        g.patch_rect = rc(x0 / scale, y0 / scale, (x1 - x0) / scale, (y1 - y0) / scale);
        if (x1 <= x0 || y1 <= y0) return;
        g.patch = ls_image_alloc(x1 - x0, y1 - y0);
        if (!g.patch.data) return;
        fill_patch_region(g, tc, ts, 0, 0, x1 - x0, y1 - y0, false);
        return;
    }

    // 画笔样式
    const float r = std::max(0.5f, s.style.line_width * 0.5f * scale);
    if (s.pts.empty()) {
        ls_image_free(&g.patch);
        g.patch_valid = false;
        g.patch_rect = rc(0, 0, 0, 0);
        return;
    }
    float fx0 = 1e30f, fy0 = 1e30f, fx1 = -1e30f, fy1 = -1e30f;
    for (const ls_point &q : s.pts) {
        fx0 = std::min(fx0, q.x * scale);
        fy0 = std::min(fy0, q.y * scale);
        fx1 = std::max(fx1, q.x * scale);
        fy1 = std::max(fy1, q.y * scale);
    }
    const int nx0 = clampi((int)std::floor(fx0 - r - 1), 0, BW), ny0 = clampi((int)std::floor(fy0 - r - 1), 0, BH);
    const int nx1 = clampi((int)std::ceil(fx1 + r + 1), 0, BW), ny1 = clampi((int)std::ceil(fy1 + r + 1), 0, BH);

    // 增量：同一笔继续画（前面的点没变、样式没变、新范围还在原 patch 里）只画新线段
    bool incremental = g.patch_valid && g.patch.data && g.patch_kind == kind && g.patch_param == param &&
                       g.patch_brush == r && g.mask_pts > 0 && g.mask_pts <= s.pts.size() && nx0 >= g.px_x &&
                       ny0 >= g.px_y && nx1 <= g.px_x + g.patch.width && ny1 <= g.px_y + g.patch.height;
    if (incremental) {
        for (size_t i = 0; i < g.mask_pts && incremental; ++i)
            incremental = std::memcmp(&g.saved_pts[i], &s.pts[i], sizeof(ls_point)) == 0;
    }
    if (!incremental) {
        // 正在画时四周多留一些，后续的点大多还在范围内，不必反复重新分配
        const int margin = drawing ? (int)std::ceil(64 * scale) : 0;
        const int x0 = clampi(nx0 - margin, 0, BW), y0 = clampi(ny0 - margin, 0, BH);
        const int x1 = clampi(nx1 + margin, 0, BW), y1 = clampi(ny1 + margin, 0, BH);
        ls_image_free(&g.patch);
        g.patch_valid = true;
        g.patch_kind = kind;
        g.patch_param = param;
        g.patch_brush = r;
        g.px_x = x0;
        g.px_y = y0;
        g.patch_rect = rc(x0 / scale, y0 / scale, (x1 - x0) / scale, (y1 - y0) / scale);
        g.mask_pts = 0;
        g.saved_pts.clear();
        if (x1 <= x0 || y1 <= y0) {
            g.mask.clear();
            return;
        }
        g.patch = ls_image_alloc(x1 - x0, y1 - y0);
        if (!g.patch.data) return;
        g.mask.assign((size_t)(x1 - x0) * (y1 - y0), 0);
    }
    const int mw = g.patch.width, mh = g.patch.height;
    int dirty[4] = {mw, mh, 0, 0};
    auto abs_px = [&](const ls_point &q) { return pt(q.x * scale, q.y * scale); };
    const size_t start = g.mask_pts == 0 ? 0 : g.mask_pts - 1;
    if (s.pts.size() == 1) {
        raster_capsule(g.mask, mw, mh, g.px_x, g.px_y, abs_px(s.pts[0]), abs_px(s.pts[0]), r, dirty);
    } else {
        for (size_t i = start; i + 1 < s.pts.size(); ++i)
            raster_capsule(g.mask, mw, mh, g.px_x, g.px_y, abs_px(s.pts[i]), abs_px(s.pts[i + 1]), r, dirty);
    }
    g.mask_pts = s.pts.size();
    g.saved_pts = s.pts;
    if (!incremental) {
        fill_patch_region(g, tc, ts, 0, 0, mw, mh, true);
    } else {
        fill_patch_region(g, tc, ts, dirty[0], dirty[1], dirty[2], dirty[3], true);
    }
}

}  // namespace lsk
