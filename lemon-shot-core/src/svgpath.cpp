// SVG path 解析：M L H V C S Q T A Z（含相对形式），二次曲线升为三次，圆弧拆成三次贝塞尔。
#include "internal.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace {

struct Parser {
    const char *p;

    void skip() {
        while (*p && (std::isspace((unsigned char)*p) || *p == ',')) ++p;
    }
    bool number(float &out) {
        skip();
        if (!*p) return false;
        char *end = nullptr;
        out = std::strtof(p, &end);
        if (end == p) return false;
        p = end;
        return true;
    }
    // 圆弧标志位可以紧挨着写（如 "a2 2 0 011 1"），只吃一个字符
    bool flag(int &out) {
        skip();
        if (*p == '0' || *p == '1') {
            out = *p - '0';
            ++p;
            return true;
        }
        return false;
    }
    bool has_number() {
        skip();
        return *p && (std::isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.');
    }
};

void arc_to_cubic(ls_path *out, float scale, float dx, float dy, float x1, float y1, float rx, float ry, float angle,
                  int large, int sweep, float x2, float y2) {
    auto emit_line = [&](float x, float y) { out->line_to(x * scale + dx, y * scale + dy); };
    if (rx == 0 || ry == 0) {
        emit_line(x2, y2);
        return;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    const double phi = angle * kPi / 180.0;
    const double cphi = std::cos(phi), sphi = std::sin(phi);
    const double hx = (x1 - x2) / 2.0, hy = (y1 - y2) / 2.0;
    const double x1p = cphi * hx + sphi * hy;
    const double y1p = -sphi * hx + cphi * hy;
    double rxs = rx, rys = ry;
    double lambda = (x1p * x1p) / (rxs * rxs) + (y1p * y1p) / (rys * rys);
    if (lambda > 1) {
        double s = std::sqrt(lambda);
        rxs *= s;
        rys *= s;
    }
    double num = rxs * rxs * rys * rys - rxs * rxs * y1p * y1p - rys * rys * x1p * x1p;
    double den = rxs * rxs * y1p * y1p + rys * rys * x1p * x1p;
    double coef = den == 0 ? 0 : std::sqrt(std::fmax(0.0, num / den));
    if (large == sweep) coef = -coef;
    const double cxp = coef * (rxs * y1p / rys);
    const double cyp = coef * (-rys * x1p / rxs);
    const double cx = cphi * cxp - sphi * cyp + (x1 + x2) / 2.0;
    const double cy = sphi * cxp + cphi * cyp + (y1 + y2) / 2.0;
    auto angle_of = [](double ux, double uy, double vx, double vy) {
        double a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        return a;
    };
    double th1 = angle_of(1, 0, (x1p - cxp) / rxs, (y1p - cyp) / rys);
    double dth = angle_of((x1p - cxp) / rxs, (y1p - cyp) / rys, (-x1p - cxp) / rxs, (-y1p - cyp) / rys);
    if (!sweep && dth > 0) dth -= 2 * kPi;
    if (sweep && dth < 0) dth += 2 * kPi;
    int segs = (int)std::ceil(std::fabs(dth) / (kPi / 2) - 1e-6);
    if (segs < 1) segs = 1;
    const double delta = dth / segs;
    const double t = 4.0 / 3.0 * std::tan(delta / 4);
    double a = th1;
    for (int i = 0; i < segs; ++i) {
        double c1 = std::cos(a), s1 = std::sin(a);
        double c2 = std::cos(a + delta), s2 = std::sin(a + delta);
        double ex1 = c1 - t * s1, ey1 = s1 + t * c1;
        double ex2 = c2 + t * s2, ey2 = s2 - t * c2;
        auto map = [&](double ux, double uy, float &ox, float &oy) {
            double px = rxs * ux, py = rys * uy;
            ox = (float)(cphi * px - sphi * py + cx);
            oy = (float)(sphi * px + cphi * py + cy);
        };
        float ax, ay, bx, by, ex, ey;
        map(ex1, ey1, ax, ay);
        map(ex2, ey2, bx, by);
        map(c2, s2, ex, ey);
        out->cubic_to(ax * scale + dx, ay * scale + dy, bx * scale + dx, by * scale + dy, ex * scale + dx, ey * scale + dy);
        a += delta;
    }
}

}  // namespace

extern "C" int32_t ls_svg_path_parse(const char *d, float scale, float dx, float dy, ls_path *out) {
    if (!d || !out) return -1;
    Parser ps{d};
    float cx = 0, cy = 0;     // 当前点（SVG 坐标）
    float sx = 0, sy = 0;     // 子路径起点
    float lcx = 0, lcy = 0;   // 上一个三次控制点（S 用）
    float lqx = 0, lqy = 0;   // 上一个二次控制点（T 用）
    char prev = 0;
    char cmd = 0;
    auto X = [&](float x) { return x * scale + dx; };
    auto Y = [&](float y) { return y * scale + dy; };
    while (true) {
        ps.skip();
        if (!*ps.p) break;
        if (std::isalpha((unsigned char)*ps.p)) {
            cmd = *ps.p++;
        } else if (!cmd) {
            return -1;
        }
        const bool rel = std::islower((unsigned char)cmd) != 0;
        const char up = (char)std::toupper((unsigned char)cmd);
        float ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (up) {
        case 'Z':
            out->close();
            cx = sx;
            cy = sy;
            prev = 'Z';
            cmd = 0;
            continue;
        case 'M': {
            float x, y;
            if (!ps.number(x) || !ps.number(y)) return -1;
            cx = x + ox;
            cy = y + oy;
            sx = cx;
            sy = cy;
            out->move_to(X(cx), Y(cy));
            cmd = rel ? 'l' : 'L';  // 后续坐标按 L 处理
            prev = 'M';
            continue;
        }
        case 'L': {
            float x, y;
            if (!ps.number(x) || !ps.number(y)) return -1;
            cx = x + ox;
            cy = y + oy;
            out->line_to(X(cx), Y(cy));
            break;
        }
        case 'H': {
            float x;
            if (!ps.number(x)) return -1;
            cx = x + ox;
            out->line_to(X(cx), Y(cy));
            break;
        }
        case 'V': {
            float y;
            if (!ps.number(y)) return -1;
            cy = y + oy;
            out->line_to(X(cx), Y(cy));
            break;
        }
        case 'C': {
            float x1, y1, x2, y2, x, y;
            if (!ps.number(x1) || !ps.number(y1) || !ps.number(x2) || !ps.number(y2) || !ps.number(x) || !ps.number(y)) return -1;
            x1 += ox; y1 += oy; x2 += ox; y2 += oy; x += ox; y += oy;
            out->cubic_to(X(x1), Y(y1), X(x2), Y(y2), X(x), Y(y));
            lcx = x2; lcy = y2;
            cx = x; cy = y;
            break;
        }
        case 'S': {
            float x2, y2, x, y;
            if (!ps.number(x2) || !ps.number(y2) || !ps.number(x) || !ps.number(y)) return -1;
            x2 += ox; y2 += oy; x += ox; y += oy;
            float x1 = cx, y1 = cy;
            if (prev == 'C' || prev == 'S') { x1 = 2 * cx - lcx; y1 = 2 * cy - lcy; }
            out->cubic_to(X(x1), Y(y1), X(x2), Y(y2), X(x), Y(y));
            lcx = x2; lcy = y2;
            cx = x; cy = y;
            break;
        }
        case 'Q': {
            float qx, qy, x, y;
            if (!ps.number(qx) || !ps.number(qy) || !ps.number(x) || !ps.number(y)) return -1;
            qx += ox; qy += oy; x += ox; y += oy;
            out->cubic_to(X(cx + 2.f / 3 * (qx - cx)), Y(cy + 2.f / 3 * (qy - cy)), X(x + 2.f / 3 * (qx - x)), Y(y + 2.f / 3 * (qy - y)), X(x), Y(y));
            lqx = qx; lqy = qy;
            cx = x; cy = y;
            break;
        }
        case 'T': {
            float x, y;
            if (!ps.number(x) || !ps.number(y)) return -1;
            x += ox; y += oy;
            float qx = cx, qy = cy;
            if (prev == 'Q' || prev == 'T') { qx = 2 * cx - lqx; qy = 2 * cy - lqy; }
            out->cubic_to(X(cx + 2.f / 3 * (qx - cx)), Y(cy + 2.f / 3 * (qy - cy)), X(x + 2.f / 3 * (qx - x)), Y(y + 2.f / 3 * (qy - y)), X(x), Y(y));
            lqx = qx; lqy = qy;
            cx = x; cy = y;
            break;
        }
        case 'A': {
            float rx, ry, rot, x, y;
            int large, sweep;
            if (!ps.number(rx) || !ps.number(ry) || !ps.number(rot) || !ps.flag(large) || !ps.flag(sweep) || !ps.number(x) || !ps.number(y)) return -1;
            x += ox; y += oy;
            arc_to_cubic(out, scale, dx, dy, cx, cy, rx, ry, rot, large, sweep, x, y);
            cx = x; cy = y;
            break;
        }
        default:
            return -1;
        }
        prev = up;
        if (!ps.has_number()) cmd = 0;
    }
    return 0;
}
