// 颜色：色彩空间换算、最近颜色名称、HEX 与自定义格式。
#include "internal.h"

#include <cmath>
#include <cstring>

namespace {

// ---------------------------------------------------------------- 色彩空间

struct SpaceDef {
    double rx, ry, gx, gy, bx, by; // 三原色 xy
    double wx, wy;                 // 白点 xy
    int transfer;                  // 0 sRGB 分段曲线，1 纯伽马
    double gamma;
};

const double kD65x = 0.3127, kD65y = 0.3290;

// Generic 用 Apple RGB 的原色（白点 D65、伽马 1.8），即 macOS「通用 RGB」按 D65 适配后的近似。
const SpaceDef kSpaces[4] = {
    {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, kD65x, kD65y, 0, 2.4},          // sRGB
    {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, kD65x, kD65y, 0, 2.4},          // Display P3
    {0.640, 0.330, 0.210, 0.710, 0.150, 0.060, kD65x, kD65y, 1, 563.0 / 256}, // Adobe RGB (1998)
    {0.625, 0.340, 0.280, 0.595, 0.155, 0.070, kD65x, kD65y, 1, 1.8},          // Apple 通用 RGB
};

typedef double Mat3[3][3];

void mat_mul(const Mat3 a, const Mat3 b, Mat3 out) {
    Mat3 t;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    std::memcpy(out, t, sizeof t);
}

void mat_vec(const Mat3 m, const double v[3], double out[3]) {
    double t[3];
    for (int i = 0; i < 3; ++i) t[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    out[0] = t[0];
    out[1] = t[1];
    out[2] = t[2];
}

bool mat_inv(const Mat3 m, Mat3 out) {
    double a = m[0][0], b = m[0][1], c = m[0][2];
    double d = m[1][0], e = m[1][1], f = m[1][2];
    double g = m[2][0], h = m[2][1], i = m[2][2];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12) return false;
    double inv = 1.0 / det;
    out[0][0] = A * inv;
    out[0][1] = -(b * i - c * h) * inv;
    out[0][2] = (b * f - c * e) * inv;
    out[1][0] = B * inv;
    out[1][1] = (a * i - c * g) * inv;
    out[1][2] = -(a * f - c * d) * inv;
    out[2][0] = C * inv;
    out[2][1] = -(a * h - b * g) * inv;
    out[2][2] = (a * e - b * d) * inv;
    return true;
}

void xy_to_XYZ(double x, double y, double out[3]) {
    out[0] = x / y;
    out[1] = 1.0;
    out[2] = (1.0 - x - y) / y;
}

// 线性 RGB -> XYZ（以该空间自己的白点为准）
void rgb_to_xyz_matrix(const SpaceDef &s, Mat3 out) {
    double r[3], g[3], b[3], w[3];
    xy_to_XYZ(s.rx, s.ry, r);
    xy_to_XYZ(s.gx, s.gy, g);
    xy_to_XYZ(s.bx, s.by, b);
    xy_to_XYZ(s.wx, s.wy, w);
    Mat3 p = {{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}};
    Mat3 pi;
    mat_inv(p, pi);
    double sc[3];
    mat_vec(pi, w, sc);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = p[i][j] * sc[j];
}

// Bradford 适配：XYZ（白点 ws）-> XYZ（白点 wd）
void bradford(const double ws[3], const double wd[3], Mat3 out) {
    static const Mat3 M = {{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}};
    Mat3 Mi;
    mat_inv(M, Mi);
    double cs[3], cd[3];
    mat_vec(M, ws, cs);
    mat_vec(M, wd, cd);
    Mat3 D = {{cd[0] / cs[0], 0, 0}, {0, cd[1] / cs[1], 0}, {0, 0, cd[2] / cs[2]}};
    Mat3 t;
    mat_mul(D, M, t);
    mat_mul(Mi, t, out);
}

double decode(const SpaceDef &s, double v) {
    if (s.transfer == 0) return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    return v <= 0 ? 0 : std::pow(v, s.gamma);
}

double encode(const SpaceDef &s, double v) {
    if (v <= 0) return 0;
    if (s.transfer == 0) return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return std::pow(v, 1.0 / s.gamma);
}

double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// ---------------------------------------------------------------- Lab 与颜色名

struct Lab {
    double L, a, b;
};

double srgb_lin(int c) { return decode(kSpaces[LS_SPACE_SRGB], c / 255.0); }

double lab_f(double t) {
    const double d = 6.0 / 29.0;
    return t > d * d * d ? std::cbrt(t) : t / (3 * d * d) + 4.0 / 29.0;
}

Lab rgb_to_lab(int r, int g, int b) {
    double lr = srgb_lin(r), lg = srgb_lin(g), lb = srgb_lin(b);
    // sRGB（D65）-> XYZ
    double X = 0.4123908 * lr + 0.3575843 * lg + 0.1804808 * lb;
    double Y = 0.2126390 * lr + 0.7151687 * lg + 0.0721923 * lb;
    double Z = 0.0193308 * lr + 0.1191948 * lg + 0.9505322 * lb;
    double fx = lab_f(X / 0.95047), fy = lab_f(Y / 1.0), fz = lab_f(Z / 1.08883);
    Lab o;
    o.L = 116 * fy - 16;
    o.a = 500 * (fx - fy);
    o.b = 200 * (fy - fz);
    return o;
}

struct NamedColor {
    const char *en;
    const char *zh;
    uint32_t rgb;
};

const NamedColor kNames[] = {
#define X(en, zh, hex) {en, zh, hex},
#include "color_names.inc"
#undef X
};
const int kNameCount = (int)(sizeof kNames / sizeof kNames[0]);

struct NameLabs {
    Lab lab[sizeof kNames / sizeof kNames[0]];
    NameLabs() {
        for (int i = 0; i < kNameCount; ++i) {
            uint32_t c = kNames[i].rgb;
            lab[i] = rgb_to_lab((int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255));
        }
    }
};

const NameLabs &name_labs() {
    static const NameLabs labs;
    return labs;
}

// 把 src 写进 buf（截断时不切开 UTF-8 多字节字符），返回写入字节数（不含结尾 0）
int32_t put_str(const char *src, char *buf, int32_t capacity) {
    if (!buf || capacity <= 0) return 0;
    int32_t n = (int32_t)std::strlen(src);
    if (n > capacity - 1) {
        n = capacity - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) --n;
    }
    std::memcpy(buf, src, (size_t)n);
    buf[n] = 0;
    return n;
}

struct Out {
    char *buf;
    int32_t cap;
    int32_t len;
    bool full;
    void put(char c) {
        if (len + 1 < cap) {
            buf[len++] = c;
        } else {
            full = true;
        }
    }
    void puts(const char *s) {
        while (*s) put(*s++);
    }
    void finish() {
        if (buf && cap > 0) buf[len < cap ? len : cap - 1] = 0;
    }
};

void put_uint(Out &o, unsigned v) {
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) o.put(tmp[--n]);
}

void put_hex(Out &o, uint8_t r, uint8_t g, uint8_t b, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    const uint8_t v[3] = {r, g, b};
    for (int i = 0; i < 3; ++i) {
        o.put(digits[v[i] >> 4]);
        o.put(digits[v[i] & 15]);
    }
}

// 0..1，三位小数，不受区域设置影响
void put_unit(Out &o, uint8_t c) {
    unsigned m = (unsigned)((c * 1000 + 127) / 255); // 四舍五入到千分位
    put_uint(o, m / 1000);
    o.put('.');
    unsigned f = m % 1000;
    o.put((char)('0' + f / 100));
    o.put((char)('0' + f / 10 % 10));
    o.put((char)('0' + f % 10));
}

}  // namespace

extern "C" {

void ls_color_convert(int32_t from, int32_t to, const float in[3], float out[3]) {
    if (from < 0 || from > 3 || to < 0 || to > 3) {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        return;
    }
    const SpaceDef &sf = kSpaces[from];
    const SpaceDef &st = kSpaces[to];
    double lin[3];
    for (int i = 0; i < 3; ++i) lin[i] = decode(sf, clamp01(in[i]));
    Mat3 m1, m2, m2i;
    rgb_to_xyz_matrix(sf, m1);
    rgb_to_xyz_matrix(st, m2);
    mat_inv(m2, m2i);
    double xyz[3];
    mat_vec(m1, lin, xyz);
    if (sf.wx != st.wx || sf.wy != st.wy) {
        double ws[3], wd[3];
        xy_to_XYZ(sf.wx, sf.wy, ws);
        xy_to_XYZ(st.wx, st.wy, wd);
        Mat3 ad;
        bradford(ws, wd, ad);
        mat_vec(ad, xyz, xyz);
    }
    double rgb[3];
    mat_vec(m2i, xyz, rgb);
    for (int i = 0; i < 3; ++i) out[i] = (float)clamp01(encode(st, clamp01(rgb[i])));
}

int32_t ls_color_name(uint8_t r, uint8_t g, uint8_t b, const char *lang, char *buf, int32_t capacity) {
    const NameLabs &labs = name_labs();
    Lab c = rgb_to_lab(r, g, b);
    int best = 0;
    double bestd = 1e300;
    for (int i = 0; i < kNameCount; ++i) {
        double dl = c.L - labs.lab[i].L, da = c.a - labs.lab[i].a, db = c.b - labs.lab[i].b;
        double d = dl * dl + da * da + db * db;
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    const bool zh = lang && std::strcmp(lang, "zh-Hans") == 0;
    return put_str(zh ? kNames[best].zh : kNames[best].en, buf, capacity);
}

int32_t ls_color_hex(uint8_t r, uint8_t g, uint8_t b, int32_t prefix, int32_t upper, char *buf, int32_t capacity) {
    if (!buf || capacity <= 0) return 0;
    Out o{buf, capacity, 0, false};
    if (prefix) o.put('#');
    put_hex(o, r, g, b, upper);
    o.finish();
    return o.len;
}

int32_t ls_color_format(const char *pattern, uint8_t r, uint8_t g, uint8_t b, int32_t upper, char *buf, int32_t capacity) {
    if (!buf || capacity <= 0) return 0;
    Out o{buf, capacity, 0, false};
    const char *p = pattern ? pattern : "";
    while (*p) {
        if (p[0] == '_') {
            if (std::strncmp(p, "_HEX", 4) == 0) {
                put_hex(o, r, g, b, upper);
                p += 4;
                continue;
            }
            switch (p[1]) {
            case 'R': put_uint(o, r); p += 2; continue;
            case 'G': put_uint(o, g); p += 2; continue;
            case 'B': put_uint(o, b); p += 2; continue;
            case 'r': put_unit(o, r); p += 2; continue;
            case 'g': put_unit(o, g); p += 2; continue;
            case 'b': put_unit(o, b); p += 2; continue;
            default: break;
            }
        }
        o.put(*p++);
    }
    o.finish();
    return o.len;
}

}  // extern "C"
