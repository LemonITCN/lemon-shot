#include "Shell/ShellRender.h"

#include <windows.h>
#include <d2d1.h>
#include <objbase.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Settings/Theme.h"
#include "Util/ComPtr.h"
#include "Util/Log.h"

namespace ls {
namespace shell {

namespace {

struct R {
    float x = 0, y = 0, w = 0, h = 0;
    R() = default;
    R(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    float r() const { return x + w; }
    float b() const { return y + h; }
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
    R Inset(float d) const { return R(x + d, y + d, w - 2 * d, h - 2 * d); }
    R Off(float dx, float dy) const { return R(x + dx, y + dy, w, h); }
};

D2D1_COLOR_F Hex(const char *hex, float alpha = 1.f) {
    Color c = ParseColor(hex);
    return D2D1_COLOR_F{c.r, c.g, c.b, c.a * alpha};
}

D2D1_COLOR_F Gray(float v, float a) { return D2D1_COLOR_F{v, v, v, a}; }

float Aspect(int m) {
    switch (m) {
    case kMacbookAir: return 2560.f / 1664.f;
    case kMacbookPro14: return 3024.f / 1964.f;
    case kMacbookPro16: return 3456.f / 2234.f;
    case kLaptop: return 16.f / 10.f;
    case kIphone: return 2556.f / 1179.f;
    case kIpad: return 2360.f / 1640.f;
    default: return 16.f / 9.f;  // iMac、Studio Display、通用显示器
    }
}

bool IsLaptop(int m) { return m == kMacbookAir || m == kMacbookPro14 || m == kMacbookPro16 || m == kLaptop; }

struct Laptop {
    float side, top, bottom, lidRadius, baseH, baseOver;
    const char *rim, *bezel, *baseTop, *baseBottom, *notch;
    bool hasNotch;
};

Laptop LaptopOf(int m) {
    switch (m) {
    case kMacbookAir:
        return Laptop{0.019f, 0.03f, 0.03f, 0.028f, 0.024f, 0.075f, "#BFC1C6", "#0A0A0B", "#DDDEE2", "#A7A9AE", "#0A0A0B", true};
    case kMacbookPro14:
    case kMacbookPro16:
        return Laptop{0.02f, 0.03f, 0.032f, 0.03f, m == kMacbookPro16 ? 0.03f : 0.028f, 0.07f, "#3A3B3F", "#050506", "#55575C",
                      "#2A2B2E", "#050506", true};
    default:
        return Laptop{0.03f, 0.045f, 0.055f, 0.022f, 0.036f, 0.08f, "#2E3034", "#111214", "#5A5D63", "#33363B", "#111214", false};
    }
}

// 屏幕四角圆角半径（上、下）
void ScreenRadius(int m, float W, float H, float &top, float &bottom) {
    top = bottom = 0;
    if (m == kMacbookAir || m == kMacbookPro14 || m == kMacbookPro16) top = W * 0.0105f;
    if (m == kIphone) top = bottom = H * 0.17f;
    if (m == kIpad) top = bottom = H * 0.035f;
}

// 外壳总外接框（相对于屏幕区域所在坐标）
R Bounds(int m, R s, float W) {
    if (IsLaptop(m)) {
        Laptop l = LaptopOf(m);
        R lid(s.x - l.side * W, s.y - l.top * W, s.w + 2 * l.side * W, s.h + (l.top + l.bottom) * W);
        float baseW = lid.w * (1 + 2 * l.baseOver);
        return R(lid.cx() - baseW / 2, lid.y, baseW, lid.h + l.baseH * W);
    }
    switch (m) {
    case kImac24: {
        float b = 0.034f * W, chin = 0.15f * W;
        R body(s.x - b, s.y - b, s.w + 2 * b, s.h + b + chin);
        return R(body.x, body.y, body.w, body.h + 0.2f * W);
    }
    case kStudioDisplay: {
        float b = 0.034f * W;
        R body = s.Inset(-b);
        return R(body.x, body.y, body.w, body.h + 0.22f * W);
    }
    case kMonitor: {
        float b = 0.012f * W, chin = 0.034f * W;
        R body(s.x - b, s.y - b, s.w + 2 * b, s.h + b + chin);
        return R(body.x, body.y, body.w, body.h + 0.17f * W);
    }
    case kIphone: {
        float b = s.h * 0.048f;
        return s.Inset(-b - s.h * 0.012f);
    }
    default: return s.Inset(-s.h * 0.05f);  // iPad
    }
}

class Painter {
public:
    ID2D1Factory *f;
    ID2D1RenderTarget *rt;
    ComPtr<ID2D1SolidColorBrush> brush;

    Painter(ID2D1Factory *factory, ID2D1RenderTarget *target) : f(factory), rt(target) {
        D2D1_COLOR_F c = {0, 0, 0, 1};
        rt->CreateSolidColorBrush(&c, nullptr, brush.Put());
    }

    ID2D1SolidColorBrush *B(D2D1_COLOR_F c) {
        brush->SetColor(&c);
        return brush.Get();
    }

    // 上下圆角不同的圆角矩形
    ComPtr<ID2D1PathGeometry> Rounded(R r, float top, float bottom) {
        ComPtr<ID2D1PathGeometry> g;
        if (FAILED(f->CreatePathGeometry(g.Put()))) return g;
        ComPtr<ID2D1GeometrySink> s;
        if (FAILED(g->Open(s.Put()))) {
            g.Reset();
            return g;
        }
        float t = std::min({top, r.w / 2, r.h / 2}), b = std::min({bottom, r.w / 2, r.h / 2});
        if (t < 0) t = 0;
        if (b < 0) b = 0;
        auto arc = [&](float x, float y, float rad) {
            D2D1_ARC_SEGMENT a = {{x, y}, {rad, rad}, 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL};
            s->AddArc(&a);
        };
        s->BeginFigure(D2D1_POINT_2F{r.x + t, r.y}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine(D2D1_POINT_2F{r.r() - t, r.y});
        if (t > 0) arc(r.r(), r.y + t, t);
        s->AddLine(D2D1_POINT_2F{r.r(), r.b() - b});
        if (b > 0) arc(r.r() - b, r.b(), b);
        s->AddLine(D2D1_POINT_2F{r.x + b, r.b()});
        if (b > 0) arc(r.x, r.b() - b, b);
        s->AddLine(D2D1_POINT_2F{r.x, r.y + t});
        if (t > 0) arc(r.x + t, r.y, t);
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        return g;
    }
    ComPtr<ID2D1PathGeometry> Round(R r, float rad) { return Rounded(r, rad, rad); }

    void Fill(ID2D1Geometry *g, D2D1_COLOR_F c) {
        if (g) rt->FillGeometry(g, B(c), nullptr);
    }
    void FillRect(R r, D2D1_COLOR_F c, float rad = 0) {
        if (rad > 0) {
            Fill(Round(r, rad).Get(), c);
            return;
        }
        D2D1_RECT_F d = {r.x, r.y, r.r(), r.b()};
        rt->FillRectangle(&d, B(c));
    }
    void Ellipse(R r, D2D1_COLOR_F c) {
        D2D1_ELLIPSE e = {{r.cx(), r.cy()}, r.w / 2, r.h / 2};
        rt->FillEllipse(&e, B(c));
    }
    void StrokeRound(R r, D2D1_COLOR_F c, float w, float rad) {
        auto g = Round(r.Inset(w / 2), rad - w / 2 > 0 ? rad - w / 2 : 0);
        if (g) rt->DrawGeometry(g.Get(), B(c), w, nullptr);
    }
    // 线性渐变（默认竖直，从上到下）
    void Linear(ID2D1Geometry *g, D2D1_COLOR_F c0, D2D1_COLOR_F c1, bool vertical = true) {
        if (!g) return;
        D2D1_RECT_F bb;
        if (FAILED(g->GetBounds(nullptr, &bb))) return;
        D2D1_GRADIENT_STOP gs[2] = {{0.f, c0}, {1.f, c1}};
        ComPtr<ID2D1GradientStopCollection> stops;
        if (FAILED(rt->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stops.Put()))) return;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp = {{bb.left, bb.top}, vertical ? D2D1_POINT_2F{bb.left, bb.bottom} : D2D1_POINT_2F{bb.right, bb.top}};
        ComPtr<ID2D1LinearGradientBrush> lb;
        if (FAILED(rt->CreateLinearGradientBrush(&lp, nullptr, stops.Get(), lb.Put()))) return;
        rt->FillGeometry(g, lb.Get(), nullptr);
    }
    void LinearRect(R r, D2D1_COLOR_F c0, D2D1_COLOR_F c1, bool vertical = true) {
        ComPtr<ID2D1RectangleGeometry> g;
        D2D1_RECT_F d = {r.x, r.y, r.r(), r.b()};
        if (SUCCEEDED(f->CreateRectangleGeometry(&d, g.Put()))) Linear(g.Get(), c0, c1, vertical);
    }
    void Camera(float cx, float cy, float r) {
        Ellipse(R(cx - r, cy - r, 2 * r, 2 * r), Hex("#1B1D24"));
        float ir = r * 0.45f;
        Ellipse(R(cx - ir, cy - ir, 2 * ir, 2 * ir), Hex("#2C3550"));
        float hr = r * 0.18f;
        Ellipse(R(cx - ir * 0.4f - hr, cy - ir * 0.4f - hr, 2 * hr, 2 * hr), Gray(1, 0.35f));
    }
    void PushClip(ID2D1Geometry *g) {
        ComPtr<ID2D1Layer> layer;
        rt->CreateLayer(nullptr, layer.Put());
        D2D1_MATRIX_3X2_F id;
        id._11 = 1, id._12 = 0, id._21 = 0, id._22 = 1, id._31 = 0, id._32 = 0;
        D2D1_RECT_F inf = {-1e7f, -1e7f, 1e7f, 1e7f};
        D2D1_LAYER_PARAMETERS lp = {inf, g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, id, 1.f, nullptr, D2D1_LAYER_OPTIONS_NONE};
        rt->PushLayer(&lp, layer.Get());
    }
    void PopClip() { rt->PopLayer(); }
};

void DrawBody(Painter &p, int m, R s, float W) {
    if (IsLaptop(m)) {
        Laptop l = LaptopOf(m);
        R lid(s.x - l.side * W, s.y - l.top * W, s.w + 2 * l.side * W, s.h + (l.top + l.bottom) * W);
        // 底座：比屏幕宽一些的薄板，下缘圆角，正中开槽
        float baseW = lid.w * (1 + 2 * l.baseOver);
        R base(lid.cx() - baseW / 2, lid.b(), baseW, l.baseH * W);
        p.Linear(p.Rounded(base, base.h * 0.15f, base.h * 0.55f).Get(), Hex(l.baseTop), Hex(l.baseBottom));
        // 底座上缘高光
        p.FillRect(R(base.x + base.h * 0.3f, base.y, base.w - base.h * 0.6f, std::max(1.f, W * 0.0012f)), Gray(1, 0.35f));
        float notchW = W * 0.13f, notchH = base.h * 0.32f;
        p.Fill(p.Rounded(R(base.cx() - notchW / 2, base.y, notchW, notchH), 0, notchH).Get(), Hex(l.baseBottom, 0.9f));
        // 转轴阴影
        p.FillRect(R(lid.x + lid.w * 0.02f, lid.b() - W * 0.002f, lid.w * 0.96f, W * 0.004f), Gray(0, 0.45f));
        // 屏幕外壳：外缘金属边 + 黑色玻璃
        p.Linear(p.Rounded(lid, l.lidRadius * W, W * 0.004f).Get(), Hex(l.rim), Hex(l.rim, 0.85f));
        R glass = lid.Inset(W * 0.0035f);
        p.Fill(p.Rounded(glass, l.lidRadius * W - W * 0.0035f, W * 0.002f).Get(), Hex(l.bezel));
        if (!l.hasNotch) p.Camera(s.cx(), s.y - l.top * W / 2, W * 0.0035f);
        return;
    }
    switch (m) {
    case kImac24: {
        float b = 0.034f * W, chin = 0.15f * W;
        R body(s.x - b, s.y - b, s.w + 2 * b, s.h + b + chin);
        // 支架：一片薄板 + 底座
        float standW = W * 0.24f;
        R stand(body.cx() - standW / 2, body.b() - W * 0.01f, standW, W * 0.19f);
        p.Linear(p.Rounded(stand, 0, W * 0.004f).Get(), Hex("#9DB8D8"), Hex("#7E9CC0"));
        R foot(body.cx() - W * 0.15f, stand.b() - W * 0.004f, W * 0.3f, W * 0.014f);
        p.Linear(p.Round(foot, foot.h / 2).Get(), Hex("#A9C3E0"), Hex("#7393BA"));
        // 机身：白色玻璃边 + 彩色下巴
        auto bodyPath = p.Round(body, W * 0.014f);
        p.PushClip(bodyPath.Get());
        p.FillRect(body, Hex("#F4F5F7"));
        R chinR(body.x, s.b() + b, body.w, body.b() - s.b() - b);
        p.LinearRect(chinR, Hex("#B9D0EA"), Hex("#93B3D8"));
        p.PopClip();
        p.StrokeRound(body, Gray(0, 0.12f), std::max(1.f, W * 0.001f), W * 0.014f);
        p.Camera(s.cx(), s.y - b / 2, W * 0.0028f);
        break;
    }
    case kStudioDisplay: {
        float b = 0.034f * W;
        R body = s.Inset(-b);
        float neckW = W * 0.24f;
        R neck(body.cx() - neckW / 2, body.b() - W * 0.02f, neckW, W * 0.21f);
        p.Linear(p.Rounded(neck, 0, W * 0.006f).Get(), Hex("#D3D4D8"), Hex("#A6A8AD"));
        R foot(body.cx() - W * 0.17f, neck.b() - W * 0.006f, W * 0.34f, W * 0.018f);
        p.Linear(p.Round(foot, foot.h / 2).Get(), Hex("#DADBDF"), Hex("#9C9EA3"));
        p.Linear(p.Round(body, W * 0.016f).Get(), Hex("#D2D3D7"), Hex("#B4B6BB"));
        p.Fill(p.Round(body.Inset(W * 0.004f), W * 0.012f).Get(), Hex("#0A0A0B"));
        p.Camera(s.cx(), s.y - b / 2, W * 0.0028f);
        break;
    }
    case kMonitor: {
        float b = 0.012f * W, chin = 0.034f * W;
        R body(s.x - b, s.y - b, s.w + 2 * b, s.h + b + chin);
        float neckW = W * 0.06f;
        R neck(body.cx() - neckW / 2, body.b() - W * 0.01f, neckW, W * 0.15f);
        p.LinearRect(neck, Hex("#2F3135"), Hex("#1E1F22"), false);
        R foot(body.cx() - W * 0.16f, neck.b() - W * 0.008f, W * 0.32f, W * 0.028f);
        p.Linear(p.Rounded(foot, foot.h * 0.6f, foot.h * 0.3f).Get(), Hex("#3A3C41"), Hex("#1C1D20"));
        p.Linear(p.Round(body, W * 0.008f).Get(), Hex("#26272B"), Hex("#18191B"));
        p.Fill(p.Round(R(s.x - W * 0.004f, s.y - W * 0.004f, s.w + W * 0.008f, s.h + W * 0.008f), W * 0.003f).Get(), Hex("#08080A"));
        // 下巴正中的电源指示灯
        p.Ellipse(R(s.cx() - W * 0.002f, s.b() + chin / 2 - W * 0.002f, W * 0.004f, W * 0.004f), Hex("#4C82FF", 0.8f));
        break;
    }
    case kIphone: {
        float H = s.h;
        float b = H * 0.048f, rim = H * 0.012f;
        R outer = s.Inset(-b - rim);
        float r = H * 0.17f + b + rim;
        // 侧键：横放时音量键在下缘、电源键在上缘
        D2D1_COLOR_F key = Hex("#8D8983");
        float kt = rim * 0.9f;
        p.FillRect(R(outer.x + outer.w * 0.18f, outer.b() - 1, outer.w * 0.06f, kt + 1), key, kt / 2);
        p.FillRect(R(outer.x + outer.w * 0.27f, outer.b() - 1, outer.w * 0.09f, kt + 1), key, kt / 2);
        p.FillRect(R(outer.x + outer.w * 0.39f, outer.b() - 1, outer.w * 0.09f, kt + 1), key, kt / 2);
        p.FillRect(R(outer.x + outer.w * 0.28f, outer.y - kt, outer.w * 0.14f, kt + 1), key, kt / 2);
        p.Linear(p.Round(outer, r).Get(), Hex("#B7B2AA"), Hex("#7F7B75"));
        p.Fill(p.Round(outer.Inset(rim), r - rim).Get(), Hex("#050506"));
        break;
    }
    default: {  // iPad
        float H = s.h;
        float b = H * 0.05f;
        R outer = s.Inset(-b);
        float r = H * 0.035f + b;
        p.FillRect(R(outer.r() - outer.w * 0.12f, outer.y - H * 0.006f, outer.w * 0.06f, H * 0.008f), Hex("#6E7075"), H * 0.004f);
        p.Linear(p.Round(outer, r).Get(), Hex("#8A8C91"), Hex("#5E6065"));
        p.Fill(p.Round(outer.Inset(H * 0.006f), r - H * 0.006f).Get(), Hex("#0A0A0B"));
        p.Camera(s.cx(), s.y - b / 2, H * 0.006f);
        break;
    }
    }
}

void DrawScreen(Painter &p, ID2D1Bitmap *bmp, int m, R s, float W) {
    float top, bottom;
    ScreenRadius(m, W, s.h, top, bottom);
    auto clip = p.Rounded(s, top, bottom);
    p.PushClip(clip.Get());
    p.FillRect(s, Hex("#000000"));
    if (bmp) {
        D2D1_RECT_F d = {s.x, s.y, s.r(), s.b()};
        p.rt->DrawBitmap(bmp, &d, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    }
    p.PopClip();
}

// 压在画面上的部分：刘海、灵动岛
void DrawFront(Painter &p, int m, R s, float W) {
    if (m == kMacbookAir || m == kMacbookPro14 || m == kMacbookPro16) {
        Laptop l = LaptopOf(m);
        float nw = W * 0.072f, nh = W * 0.0235f;
        p.Fill(p.Rounded(R(s.cx() - nw / 2, s.y - 1, nw, nh + 1), 0, nh * 0.36f).Get(), Hex(l.notch));
        p.Camera(s.cx(), s.y + nh * 0.48f, W * 0.0028f);
    } else if (m == kIphone) {
        float H = s.h;
        float iw = W * 0.043f, ih = H * 0.32f;
        R island(s.x + W * 0.014f, s.cy() - ih / 2, iw, ih);
        p.Fill(p.Round(island, iw / 2).Get(), Hex("#000000"));
        p.Camera(island.cx(), island.b() - iw / 2, iw * 0.28f);
    }
}

// 截图按屏幕区域等比缩放铺满、居中裁切成 sw×sh
bool FitScreen(const ls_image &shot, int sw, int sh, ls_image &out) {
    double k = std::max((double)sw / shot.width, (double)sh / shot.height);
    int w = std::max(sw, (int)std::ceil(shot.width * k)), h = std::max(sh, (int)std::ceil(shot.height * k));
    ls_image scaled = {};
    const ls_image *src = &shot;
    if (w != shot.width || h != shot.height) {
        if (ls_image_resize(&shot, w, h, &scaled) != 0 || !scaled.data) return false;
        src = &scaled;
    }
    int ok = ls_image_crop(src, (w - sw) / 2, (h - sh) / 2, sw, sh, &out);
    ls_image_free(&scaled);
    return ok == 0 && out.data;
}

// 机身投影：透明层的 alpha 缩小后模糊，染成黑色（0.28），向下偏移 W×0.012
bool MakeShadow(const ls_image &layer, float W, ls_image &out) {
    float sigma = W * 0.02f;
    double k = std::min(1.0, 640.0 / std::max(layer.width, layer.height));
    int w = std::max(1, (int)std::lround(layer.width * k)), h = std::max(1, (int)std::lround(layer.height * k));
    ls_image small = {};
    if (ls_image_resize(&layer, w, h, &small) != 0 || !small.data) return false;
    for (int y = 0; y < h; ++y) {
        uint8_t *p = small.data + (size_t)y * small.stride;
        for (int x = 0; x < w; ++x, p += 4) {
            p[0] = p[1] = p[2] = 0;
            p[3] = (uint8_t)(p[3] * 0.28f + 0.5f);
        }
    }
    ls_image blurred = {};
    int rc = ls_blur(&small, 0, 0, w, h, (float)(sigma * k), &blurred);
    ls_image_free(&small);
    if (rc != 0 || !blurred.data) return false;
    rc = ls_image_resize(&blurred, layer.width, layer.height, &out);
    ls_image_free(&blurred);
    return rc == 0 && out.data;
}

}  // namespace

const char *ModelKey(int m) {
    static const char *keys[kModelCount] = {"macbookAir", "macbookPro14", "macbookPro16", "imac24", "studioDisplay",
                                            "laptop",     "monitor",      "iphone",       "ipad"};
    return m >= 0 && m < kModelCount ? keys[m] : "laptop";
}

int ModelFromKey(const std::string &key) {
    for (int i = 0; i < kModelCount; ++i)
        if (key == ModelKey(i)) return i;
    return -1;
}

const char *ModelIcon(int m) {
    if (IsLaptop(m)) return "laptop";
    if (m == kIphone) return "smartphone";
    if (m == kIpad) return "tablet";
    return "monitor";
}

bool Render(const ls_image &shot, int model, const std::string &background, ls_image &out) {
    out = ls_image{};
    if (!shot.data || shot.width <= 0 || shot.height <= 0) return false;
    if (model < 0 || model >= kModelCount) model = kLaptop;
    double t0 = GetTickCount();
    float aspect = Aspect(model);
    // 屏幕区域按截图像素 1:1（裁切后的那块），过大时限制在 4096 宽
    float sw = std::min((float)shot.width, shot.height * aspect);
    sw = std::round(std::min(sw, 4096.f));
    float sh = std::round(sw / aspect);
    if (sw < 2 || sh < 2) return false;
    float W = sw;
    R screen0(0, 0, sw, sh);
    R body = Bounds(model, screen0, W);
    float pad = std::round(std::max(body.w, body.h) * 0.06f);
    int cw = (int)std::round(body.w + 2 * pad), ch = (int)std::round(body.h + 2 * pad);
    R s = screen0.Off(pad - body.x, pad - body.y);

    ls_image fitted = {};
    if (!FitScreen(shot, (int)sw, (int)sh, fitted)) return false;

    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ls_image layer = {};
    bool ok = false;
    {
        ComPtr<ID2D1Factory> factory;
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bmp;
        ComPtr<ID2D1RenderTarget> rt;
        D2D1_FACTORY_OPTIONS opt = {D2D1_DEBUG_LEVEL_NONE};
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opt, factory.PutVoid())) &&
            SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                       wic.PutVoid())) &&
            SUCCEEDED(wic->CreateBitmap((UINT)cw, (UINT)ch, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.Put()))) {
            D2D1_RENDER_TARGET_PROPERTIES props = {D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                   {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
                                                   96.f,
                                                   96.f,
                                                   D2D1_RENDER_TARGET_USAGE_NONE,
                                                   D2D1_FEATURE_LEVEL_DEFAULT};
            if (SUCCEEDED(factory->CreateWicBitmapRenderTarget(bmp.Get(), &props, rt.Put()))) {
                rt->BeginDraw();
                D2D1_COLOR_F clear = {0, 0, 0, 0};
                rt->Clear(&clear);
                {
                    Painter p(factory.Get(), rt.Get());
                    ComPtr<ID2D1Bitmap> shotBmp;
                    D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96.f, 96.f};
                    D2D1_SIZE_U sz = {(UINT32)fitted.width, (UINT32)fitted.height};
                    rt->CreateBitmap(sz, fitted.data, (UINT32)fitted.stride, &bp, shotBmp.Put());
                    DrawBody(p, model, s, W);
                    DrawScreen(p, shotBmp.Get(), model, s, W);
                    DrawFront(p, model, s, W);
                }
                if (SUCCEEDED(rt->EndDraw(nullptr, nullptr))) {
                    layer = ls_image_alloc(cw, ch);
                    WICRect rc = {0, 0, cw, ch};
                    ComPtr<IWICBitmapLock> lock;
                    UINT stride = 0, size = 0;
                    BYTE *px = nullptr;
                    if (layer.data && SUCCEEDED(bmp->Lock(&rc, WICBitmapLockRead, lock.Put())) && SUCCEEDED(lock->GetStride(&stride)) &&
                        SUCCEEDED(lock->GetDataPointer(&size, &px)) && px) {
                        for (int y = 0; y < ch; ++y)
                            std::memcpy(layer.data + (size_t)y * layer.stride, px + (size_t)y * stride, (size_t)cw * 4);
                        ok = true;
                    }
                }
            }
        }
    }
    if (SUCCEEDED(co)) CoUninitialize();
    ls_image_free(&fitted);
    if (!ok) {
        ls_image_free(&layer);
        LS_LOG("shell: 外壳渲染失败 %dx%d", cw, ch);
        return false;
    }

    out = ls_image_alloc(cw, ch);
    if (!out.data) {
        ls_image_free(&layer);
        return false;
    }
    uint8_t bg = background == "white" ? 255 : 0;
    if (background == "white" || background == "black") {
        for (int y = 0; y < ch; ++y) {
            uint8_t *p = out.data + (size_t)y * out.stride;
            for (int x = 0; x < cw; ++x, p += 4) p[0] = p[1] = p[2] = bg, p[3] = 255;
        }
    }
    if (background != "black") {
        ls_image shadow = {};
        if (MakeShadow(layer, W, shadow)) {
            ls_image_draw(&out, &shadow, 0, (int)std::lround(W * 0.012f));
            ls_image_free(&shadow);
        }
    }
    ls_image_draw(&out, &layer, 0, 0);
    ls_image_free(&layer);
    LS_LOG("shell: %s %dx%d 用时 %.0f ms", ModelKey(model), cw, ch, GetTickCount() - t0);
    return true;
}

}  // namespace shell
}  // namespace ls
