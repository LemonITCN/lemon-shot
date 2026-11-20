#include "UI/Gfx.h"

#include <cfloat>
#include <cmath>
#include <cstring>

#include "Settings/I18n.h"
#include "Settings/Icons.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {

RectF RectF::Intersect(const RectF &o) const {
    float x0 = x > o.x ? x : o.x;
    float y0 = y > o.y ? y : o.y;
    float x1 = r() < o.r() ? r() : o.r();
    float y1 = b() < o.b() ? b() : o.b();
    if (x1 <= x0 || y1 <= y0) return RectF(x0, y0, 0, 0);
    return RectF(x0, y0, x1 - x0, y1 - y0);
}

static D2D1_MATRIX_3X2_F Mat(float a, float b, float c, float d, float e, float f) {
    D2D1_MATRIX_3X2_F m;
    m._11 = a;
    m._12 = b;
    m._21 = c;
    m._22 = d;
    m._31 = e;
    m._32 = f;
    return m;
}
D2D1_MATRIX_3X2_F MatIdentity() { return Mat(1, 0, 0, 1, 0, 0); }
D2D1_MATRIX_3X2_F MatTranslate(float dx, float dy) { return Mat(1, 0, 0, 1, dx, dy); }
D2D1_MATRIX_3X2_F MatScale(float sx, float sy, float cx, float cy) { return Mat(sx, 0, 0, sy, cx - sx * cx, cy - sy * cy); }
D2D1_MATRIX_3X2_F MatMul(const D2D1_MATRIX_3X2_F &a, const D2D1_MATRIX_3X2_F &b) {
    D2D1_MATRIX_3X2_F m;
    m._11 = a._11 * b._11 + a._12 * b._21;
    m._12 = a._11 * b._12 + a._12 * b._22;
    m._21 = a._21 * b._11 + a._22 * b._21;
    m._22 = a._21 * b._12 + a._22 * b._22;
    m._31 = a._31 * b._11 + a._32 * b._21 + b._31;
    m._32 = a._31 * b._12 + a._32 * b._22 + b._32;
    return m;
}

namespace gfx {

namespace {

ID2D1Factory *g_d2d = nullptr;
IDWriteFactory *g_dw = nullptr;
IWICImagingFactory *g_wic = nullptr;
ComPtr<ID2D1StrokeStyle> g_round, g_dash, g_uiDash;
std::wstring g_family;

struct FontKey {
    float size;
    int weight;
    bool mono;
    std::string lang;
    ComPtr<IDWriteTextFormat> fmt;
};
std::vector<FontKey> g_fonts;
std::wstring g_monoFamily;

bool FamilyExists(IDWriteFontCollection *col, const std::wstring &name) {
    UINT32 idx = 0;
    BOOL exists = FALSE;
    return col && SUCCEEDED(col->FindFamilyName(name.c_str(), &idx, &exists)) && exists;
}

const wchar_t *Locale() { return i18n::IsChinese() ? L"zh-CN" : L"en-US"; }

}  // namespace

bool Init() {
    D2D1_FACTORY_OPTIONS opt = {D2D1_DEBUG_LEVEL_NONE};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opt, (void **)&g_d2d))) {
        LS_LOG("gfx: D2D1CreateFactory 失败");
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&g_dw))) {
        LS_LOG("gfx: DWriteCreateFactory 失败");
        return false;
    }
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory1, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                (void **)&g_wic))) {
        LS_LOG("gfx: WIC 工厂创建失败");
        return false;
    }
    D2D1_STROKE_STYLE_PROPERTIES p = {D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                      D2D1_LINE_JOIN_ROUND, 10.f, D2D1_DASH_STYLE_SOLID, 0.f};
    g_d2d->CreateStrokeStyle(&p, nullptr, 0, g_round.Put());
    // D2D 的虚线长度以线宽为单位：3×宽的线段、2×宽的间隔
    const float dashes[2] = {3.f, 2.f};
    p.dashStyle = D2D1_DASH_STYLE_CUSTOM;
    g_d2d->CreateStrokeStyle(&p, dashes, 2, g_dash.Put());
    const float uiDashes[2] = {4.f, 3.f};
    D2D1_STROKE_STYLE_PROPERTIES q = {D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                      D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM, 0.f};
    g_d2d->CreateStrokeStyle(&q, uiDashes, 2, g_uiDash.Put());

    // 字体：按 tokens.font.win 的顺序取第一个系统里存在的
    ComPtr<IDWriteFontCollection> col;
    g_dw->GetSystemFontCollection(col.Put(), FALSE);
    for (auto &f : theme::Tok().fontFamilies) {
        if (FamilyExists(col.Get(), f)) {
            g_family = f;
            break;
        }
    }
    if (g_family.empty()) g_family = L"Segoe UI";
    for (auto &f : theme::Tok().monoFamilies) {
        if (FamilyExists(col.Get(), f)) {
            g_monoFamily = f;
            break;
        }
    }
    if (g_monoFamily.empty()) g_monoFamily = L"Consolas";
    return g_round && g_dash;
}

void Shutdown() {
    g_fonts.clear();
    g_round.Reset();
    g_dash.Reset();
    g_uiDash.Reset();
    icons::Shutdown();
    if (g_wic) g_wic->Release();
    if (g_dw) g_dw->Release();
    if (g_d2d) g_d2d->Release();
    g_wic = nullptr;
    g_dw = nullptr;
    g_d2d = nullptr;
}

ID2D1Factory *D2D() { return g_d2d; }
IDWriteFactory *DW() { return g_dw; }
IWICImagingFactory *WIC() { return g_wic; }
ID2D1StrokeStyle *RoundStroke() { return g_round.Get(); }
ID2D1StrokeStyle *DashStroke() { return g_dash.Get(); }
ID2D1StrokeStyle *UiDashStroke() { return g_uiDash.Get(); }
const std::wstring &FontFamily() { return g_family; }

IDWriteTextFormat *Font(float size, int weight, bool mono) {
    std::string lang = i18n::Lang();
    for (auto &f : g_fonts)
        if (f.size == size && f.weight == weight && f.mono == mono && f.lang == lang) return f.fmt.Get();
    FontKey k{size, weight, mono, lang, ComPtr<IDWriteTextFormat>()};
    g_dw->CreateTextFormat(mono ? g_monoFamily.c_str() : g_family.c_str(), nullptr, (DWRITE_FONT_WEIGHT)weight,
                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, Locale(), k.fmt.Put());
    if (!k.fmt) return nullptr;
    g_fonts.push_back(k);
    return g_fonts.back().fmt.Get();
}

TextSize Measure(const std::wstring &text, float size, int weight, bool mono, float maxWidth) {
    TextSize ts;
    IDWriteTextFormat *fmt = Font(size, weight, mono);
    if (!fmt) return ts;
    ComPtr<IDWriteTextLayout> lay;
    if (FAILED(g_dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, maxWidth, 100000.f, lay.Put()))) return ts;
    if (maxWidth >= 100000.f) lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m;
    if (SUCCEEDED(lay->GetMetrics(&m))) {
        ts.w = m.widthIncludingTrailingWhitespace;
        ts.h = m.height;
    }
    return ts;
}

ComPtr<IDWriteTextLayout> AnnotationLayout(const std::wstring &text, float size, bool bold) {
    ComPtr<IDWriteTextLayout> lay;
    if (size < 1.f) size = 1.f;
    IDWriteTextFormat *fmt = Font(size, bold ? kBold : kRegular);
    if (!fmt) return lay;
    if (FAILED(g_dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, 100000.f, 100000.f, lay.Put()))) return lay;
    lay->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return lay;
}

void SessionMeasure(void *, const char *utf8, float font_size, int32_t bold, float *out_w, float *out_h) {
    std::wstring w = Widen(utf8 ? std::string(utf8) : std::string());
    ComPtr<IDWriteTextLayout> lay = AnnotationLayout(w, font_size, bold != 0);
    float tw = 0, th = font_size * 1.3f;
    DWRITE_TEXT_METRICS m;
    if (lay && SUCCEEDED(lay->GetMetrics(&m))) {
        tw = m.widthIncludingTrailingWhitespace;
        th = m.height;
    }
    // 空文字：按一个字宽、一行高
    if (w.empty()) tw = font_size * 0.6f;
    if (out_w) *out_w = std::ceil(tw);
    if (out_h) *out_h = std::ceil(th);
}

ComPtr<ID2D1PathGeometry> PathGeometryXf(const ls_path *path, float scale, float dx, float dy, bool evenOdd) {
    ComPtr<ID2D1PathGeometry> geo;
    if (!path || !g_d2d) return geo;
    if (FAILED(g_d2d->CreatePathGeometry(geo.Put()))) return geo;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(sink.Put()))) {
        geo.Reset();
        return geo;
    }
    sink->SetFillMode(evenOdd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
    int32_t n = ls_path_count(path);
    bool open = false;
    D2D1_POINT_2F start{0, 0}, cur{0, 0};
    auto tp = [&](ls_point p) { return D2D1_POINT_2F{p.x * scale + dx, p.y * scale + dy}; };
    for (int32_t i = 0; i < n; ++i) {
        ls_point pts[3];
        ls_path_verb v = ls_path_get(path, i, pts);
        switch (v) {
        case LS_PATH_MOVE:
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            start = cur = tp(pts[0]);
            sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
            open = true;
            break;
        case LS_PATH_LINE:
            if (!open) {
                sink->BeginFigure(cur, D2D1_FIGURE_BEGIN_FILLED);
                start = cur;
                open = true;
            }
            cur = tp(pts[0]);
            sink->AddLine(cur);
            break;
        case LS_PATH_CUBIC: {
            if (!open) {
                sink->BeginFigure(cur, D2D1_FIGURE_BEGIN_FILLED);
                start = cur;
                open = true;
            }
            D2D1_BEZIER_SEGMENT seg = {tp(pts[0]), tp(pts[1]), tp(pts[2])};
            sink->AddBezier(&seg);
            cur = seg.point3;
            break;
        }
        case LS_PATH_CLOSE:
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            cur = start;
            break;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) geo.Reset();
    return geo;
}

ComPtr<ID2D1PathGeometry> PathGeometry(const ls_path *path, bool evenOdd) {
    return PathGeometryXf(path, 1.f, 0.f, 0.f, evenOdd);
}

ComPtr<ID2D1Bitmap> CreateBitmap(ID2D1RenderTarget *rt, const uint8_t *data, int w, int h, int stride) {
    ComPtr<ID2D1Bitmap> bmp;
    if (!rt || !data || w <= 0 || h <= 0) return bmp;
    D2D1_BITMAP_PROPERTIES bp = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}, 96.f, 96.f};
    D2D1_SIZE_U sz = {(UINT32)w, (UINT32)h};
    if (FAILED(rt->CreateBitmap(sz, data, (UINT32)stride, &bp, bmp.Put()))) bmp.Reset();
    return bmp;
}

ComPtr<ID2D1HwndRenderTarget> CreateHwndTarget(HWND hwnd, UINT dpi) {
    ComPtr<ID2D1HwndRenderTarget> rt;
    RECT rc;
    GetClientRect(hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = {D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                           {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE},
                                           (float)dpi,
                                           (float)dpi,
                                           D2D1_RENDER_TARGET_USAGE_NONE,
                                           D2D1_FEATURE_LEVEL_DEFAULT};
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = {
        hwnd, {(UINT32)(rc.right > 0 ? rc.right : 1), (UINT32)(rc.bottom > 0 ? rc.bottom : 1)}, D2D1_PRESENT_OPTIONS_NONE};
    if (FAILED(g_d2d->CreateHwndRenderTarget(&props, &hp, rt.Put()))) {
        LS_LOG("gfx: CreateHwndRenderTarget 失败");
        rt.Reset();
        return rt;
    }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return rt;
}

}  // namespace gfx

// ---------------------------------------------------------------- Canvas

Canvas::Canvas(ID2D1RenderTarget *rt) : rt_(rt) {
    D2D1_COLOR_F c = {0, 0, 0, 1};
    rt_->CreateSolidColorBrush(&c, nullptr, &brush_);
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

Canvas::~Canvas() {
    for (auto *l : layers_) l->Release();
    if (brush_) brush_->Release();
}

ID2D1SolidColorBrush *Canvas::Brush(Color c) {
    D2D1_COLOR_F d = D2C(c);
    if (brush_) brush_->SetColor(&d);
    return brush_;
}

void Canvas::Clear(Color c) {
    D2D1_COLOR_F d = D2C(c);
    rt_->Clear(&d);
}

void Canvas::FillRect(RectF r, Color c) {
    if (r.Empty() || c.a <= 0) return;
    D2D1_RECT_F d = r.D2();
    rt_->FillRectangle(&d, Brush(c));
}

void Canvas::StrokeRect(RectF r, Color c, float w) {
    if (c.a <= 0) return;
    D2D1_RECT_F d = r.D2();
    rt_->DrawRectangle(&d, Brush(c), w, nullptr);
}

void Canvas::FillRound(RectF r, float radius, Color c) {
    if (r.Empty() || c.a <= 0) return;
    if (radius <= 0) return FillRect(r, c);
    D2D1_ROUNDED_RECT rr = {r.D2(), radius, radius};
    rt_->FillRoundedRectangle(&rr, Brush(c));
}

void Canvas::StrokeRound(RectF r, float radius, Color c, float w) {
    if (c.a <= 0) return;
    if (radius <= 0) return StrokeRect(r, c, w);
    D2D1_ROUNDED_RECT rr = {r.D2(), radius, radius};
    rt_->DrawRoundedRectangle(&rr, Brush(c), w, nullptr);
}

void Canvas::FillEllipse(float cx, float cy, float rx, float ry, Color c) {
    if (c.a <= 0) return;
    D2D1_ELLIPSE e = {{cx, cy}, rx, ry};
    rt_->FillEllipse(&e, Brush(c));
}

void Canvas::StrokeEllipse(float cx, float cy, float rx, float ry, Color c, float w, ID2D1StrokeStyle *style) {
    if (c.a <= 0) return;
    D2D1_ELLIPSE e = {{cx, cy}, rx, ry};
    rt_->DrawEllipse(&e, Brush(c), w, style);
}

void Canvas::Line(float x0, float y0, float x1, float y1, Color c, float w, ID2D1StrokeStyle *style) {
    if (c.a <= 0) return;
    rt_->DrawLine(P2(x0, y0), P2(x1, y1), Brush(c), w, style);
}

void Canvas::FillGeometry(ID2D1Geometry *g, Color c) {
    if (!g || c.a <= 0) return;
    rt_->FillGeometry(g, Brush(c), nullptr);
}

void Canvas::StrokeGeometry(ID2D1Geometry *g, Color c, float w, ID2D1StrokeStyle *style) {
    if (!g || c.a <= 0) return;
    rt_->DrawGeometry(g, Brush(c), w, style);
}

void Canvas::Text(const std::wstring &s, RectF r, const TextStyle &st) {
    if (s.empty() || st.color.a <= 0) return;
    IDWriteTextFormat *fmt = gfx::Font(st.size, st.weight, st.mono);
    if (!fmt) return;
    ComPtr<IDWriteTextLayout> lay;
    float w = r.w > 0 ? r.w : 0, h = r.h > 0 ? r.h : 0;
    if (FAILED(gfx::DW()->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt, w, h, lay.Put()))) return;
    lay->SetWordWrapping(st.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    lay->SetTextAlignment(st.h == HAlign::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                          : st.h == HAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                   : DWRITE_TEXT_ALIGNMENT_TRAILING);
    lay->SetParagraphAlignment(st.v == VAlign::Top      ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                               : st.v == VAlign::Center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER
                                                        : DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    DWRITE_TEXT_RANGE all = {0, (UINT32)s.size()};
    if (st.tabular) {
        ComPtr<IDWriteTypography> typo;
        if (SUCCEEDED(gfx::DW()->CreateTypography(typo.Put()))) {
            DWRITE_FONT_FEATURE f = {DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
            typo->AddFontFeature(f);
            lay->SetTypography(typo.Get(), all);
        }
    }
    ComPtr<IDWriteInlineObject> sign;
    if (st.ellipsis && SUCCEEDED(gfx::DW()->CreateEllipsisTrimmingSign(lay.Get(), sign.Put()))) {
        DWRITE_TRIMMING t = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        lay->SetTrimming(&t, sign.Get());
    }
    rt_->DrawTextLayout(P2(r.x, r.y), lay.Get(), Brush(st.color), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Canvas::TextLayout(IDWriteTextLayout *layout, float x, float y, Color c) {
    if (!layout || c.a <= 0) return;
    rt_->DrawTextLayout(P2(x, y), layout, Brush(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

void Canvas::Icon(const char *name, float cx, float cy, float size, Color c, float stroke, bool fill) {
    ID2D1Geometry *g = icons::Get(name);
    if (!g || size <= 0 || c.a <= 0) return;
    if (stroke < 0) stroke = theme::Tok().toolbar.iconStroke;
    float s = size / 24.f;
    D2D1_MATRIX_3X2_F old = GetTransform();
    D2D1_MATRIX_3X2_F m = MatMul(MatScale(s, s), MatTranslate(cx - 12.f * s, cy - 12.f * s));
    SetTransform(MatMul(m, old));
    if (fill) rt_->FillGeometry(g, Brush(c), nullptr);
    rt_->DrawGeometry(g, Brush(c), stroke / s, gfx::RoundStroke());
    SetTransform(old);
}

namespace {

// 圆角矩形 [0,w]×[0,h]（半径 rad）对格子 [x0,x1]×[y0,y1] 的覆盖率。不碰圆角的格子直接是 1，圆角处 8×8 超采样。
float RoundRectCoverage(float w, float h, float rad, float x0, float y0, float x1, float y1) {
    bool cornerX = x0 < rad || x1 > w - rad;
    bool cornerY = y0 < rad || y1 > h - rad;
    if (rad <= 0 || !cornerX || !cornerY) return 1.f;
    const int N = 8;
    int in = 0;
    for (int sy = 0; sy < N; ++sy) {
        float py = y0 + (y1 - y0) * ((float)sy + 0.5f) / N;
        float dy = py < rad ? rad - py : (py > h - rad ? py - (h - rad) : 0.f);
        for (int sx = 0; sx < N; ++sx) {
            float px = x0 + (x1 - x0) * ((float)sx + 0.5f) / N;
            float dx = px < rad ? rad - px : (px > w - rad ? px - (w - rad) : 0.f);
            if (dx * dx + dy * dy <= rad * rad) ++in;
        }
    }
    return (float)in / (float)(N * N);
}

// 归一化的离散高斯核：格宽 step，半径 kr 格（截到 3σ）
std::vector<float> GaussKernel(float sigma, float step, int kr) {
    std::vector<float> g((size_t)(2 * kr + 1));
    float sum = 0;
    for (int k = -kr; k <= kr; ++k) {
        float t = (float)k * step / sigma;
        float v = std::exp(-0.5f * t * t);
        g[(size_t)(k + kr)] = v;
        sum += v;
    }
    for (auto &v : g) v /= sum;
    return g;
}

}  // namespace

// spec 1「阴影」：shadowBlur 按 CSS box-shadow 的模糊半径理解，高斯 σ = blur / 2，颜色 c，向下偏移 offsetY。
// D2D 1.0 没有阴影效果：在 CPU 上把圆角矩形的覆盖率做可分离高斯模糊，得到一张 σ/3 网格的小位图，
// 再线性插值拉伸画出（插值误差远小于 1/255）。行、列相同的部分只算一次，大贴图也很快。
void Canvas::Shadow(RectF r, float radius, float blur, float offsetY, Color c) {
    if (r.Empty() || c.a <= 0) return;
    RectF sr = r.Offset(0, offsetY);
    float rad = radius;
    if (rad > sr.w / 2) rad = sr.w / 2;
    if (rad > sr.h / 2) rad = sr.h / 2;
    if (rad < 0) rad = 0;
    float sigma = blur / 2.f;
    if (sigma < 0.25f) {  // 几乎不模糊：硬投影
        FillRound(sr, rad, c);
        return;
    }
    // 网格：每格约 σ/3；超大矩形放宽格宽，保证每边不超过约 1024 格
    float step = sigma / 3.f;
    float maxLen = (sr.w > sr.h ? sr.w : sr.h) + 6.f * sigma;
    if (maxLen / step > 1000.f) step = maxLen / 1000.f;
    // 每轴格数取整、格宽微调，让矩形四边正好落在格线上（覆盖率不受网格位置影响）
    int nx = (int)std::lround(sr.w / step), ny = (int)std::lround(sr.h / step);
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;
    float sx = sr.w / (float)nx, sy = sr.h / (float)ny;
    int kx = (int)std::ceil(3.f * sigma / sx), ky = (int)std::ceil(3.f * sigma / sy);
    int W = nx + 2 * kx, H = ny + 2 * ky;
    std::vector<float> gx = GaussKernel(sigma, sx, kx), gy = GaussKernel(sigma, sy, ky);

    // 1) 横向：只有矩形所在的行有覆盖；与上一行覆盖率相同的行直接复用结果
    std::vector<float> hb((size_t)W * (size_t)H, 0.f);
    std::vector<float> cov((size_t)nx), prev;
    for (int j = 0; j < ny; ++j) {
        float y0 = (float)j * sy, y1 = y0 + sy;
        for (int i = 0; i < nx; ++i)
            cov[(size_t)i] = RoundRectCoverage(sr.w, sr.h, rad, (float)i * sx, y0, (float)i * sx + sx, y1);
        float *dst = &hb[(size_t)(j + ky) * (size_t)W];
        if (j > 0 && cov == prev) {
            std::memcpy(dst, dst - W, sizeof(float) * (size_t)W);
            continue;
        }
        for (int i = 0; i < W; ++i) {
            // 输出格 i 对应覆盖格 i - kx；核覆盖 [i-2kx, i]，夹到 [0, nx)
            int a = i - 2 * kx, b = i;
            if (a < 0) a = 0;
            if (b > nx - 1) b = nx - 1;
            float s = 0;
            for (int m = a; m <= b; ++m) s += cov[(size_t)m] * gx[(size_t)(m - (i - 2 * kx))];
            dst[i] = s;
        }
        prev = cov;
    }

    // 2) 纵向：与左边一列相同的列直接复用结果
    std::vector<float> out((size_t)W * (size_t)H, 0.f);
    std::vector<float> col((size_t)H), pcol;
    for (int i = 0; i < W; ++i) {
        for (int j = 0; j < H; ++j) col[(size_t)j] = hb[(size_t)j * (size_t)W + (size_t)i];
        if (i > 0 && col == pcol) {
            for (int j = 0; j < H; ++j) out[(size_t)j * (size_t)W + (size_t)i] = out[(size_t)j * (size_t)W + (size_t)i - 1];
            continue;
        }
        // 输出行 j 对应 hb 行 j；有值的行是 [ky, ky+ny)，核覆盖 [j-ky, j+ky]
        for (int j = 0; j < H; ++j) {
            int a = j - ky, b = j + ky;
            if (a < ky) a = ky;
            if (b > ky + ny - 1) b = ky + ny - 1;
            float s = 0;
            for (int m = a; m <= b; ++m) s += col[(size_t)m] * gy[(size_t)(m - j + ky)];
            out[(size_t)j * (size_t)W + (size_t)i] = s;
        }
        pcol = col;
    }

    // 3) 预乘 BGRA
    std::vector<uint8_t> px((size_t)W * (size_t)H * 4);
    for (size_t p = 0; p < out.size(); ++p) {
        float a = c.a * out[p];
        if (a < 0) a = 0;
        if (a > 1) a = 1;
        uint8_t *q = &px[p * 4];
        q[0] = (uint8_t)(c.b * a * 255.f + 0.5f);
        q[1] = (uint8_t)(c.g * a * 255.f + 0.5f);
        q[2] = (uint8_t)(c.r * a * 255.f + 0.5f);
        q[3] = (uint8_t)(a * 255.f + 0.5f);
    }
    ComPtr<ID2D1Bitmap> bmp = gfx::CreateBitmap(rt_, px.data(), W, H, W * 4);
    if (!bmp) return;
    // 位图像素中心对应格子中心：整张位图铺在矩形外扩 (kx, ky) 格的范围上
    Bitmap(bmp.Get(), RectF(sr.x - (float)kx * sx, sr.y - (float)ky * sy, (float)W * sx, (float)H * sy), nullptr, 1.f, false);
}

void Canvas::PushClip(RectF r) {
    D2D1_RECT_F d = r.D2();
    rt_->PushAxisAlignedClip(&d, D2D1_ANTIALIAS_MODE_ALIASED);
}

void Canvas::PopClip() { rt_->PopAxisAlignedClip(); }

ID2D1Layer *Canvas::NextLayer() {
    if (layerDepth_ < layers_.size()) return layers_[layerDepth_++];
    ID2D1Layer *l = nullptr;
    if (FAILED(rt_->CreateLayer(nullptr, &l)) || !l) return nullptr;
    layers_.push_back(l);
    ++layerDepth_;
    return l;
}

void Canvas::PushOpacity(float opacity, RectF bounds) {
    ID2D1Layer *l = NextLayer();
    D2D1_LAYER_PARAMETERS p = {bounds.D2(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, MatIdentity(), opacity, nullptr,
                               D2D1_LAYER_OPTIONS_NONE};
    if (l) {
        rt_->PushLayer(&p, l);
    } else {
        // 建层失败：退化为轴对齐裁剪，保证 Push/Pop 成对
        D2D1_RECT_F d = bounds.D2();
        rt_->PushAxisAlignedClip(&d, D2D1_ANTIALIAS_MODE_ALIASED);
        layers_.push_back(nullptr);
        ++layerDepth_;
    }
}

void Canvas::PushGeometryMask(ID2D1Geometry *g, float opacity) {
    ID2D1Layer *l = NextLayer();
    D2D1_RECT_F inf = {-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX};
    D2D1_LAYER_PARAMETERS p = {inf, g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, MatIdentity(), opacity, nullptr,
                               D2D1_LAYER_OPTIONS_NONE};
    if (l) {
        rt_->PushLayer(&p, l);
    } else {
        rt_->PushAxisAlignedClip(&inf, D2D1_ANTIALIAS_MODE_ALIASED);
        layers_.push_back(nullptr);
        ++layerDepth_;
    }
}

void Canvas::PopLayer() {
    if (layerDepth_ == 0) return;
    --layerDepth_;
    if (layers_[layerDepth_]) {
        rt_->PopLayer();
    } else {
        rt_->PopAxisAlignedClip();
        layers_.erase(layers_.begin() + (long)layerDepth_);
    }
}

D2D1_MATRIX_3X2_F Canvas::GetTransform() const {
    D2D1_MATRIX_3X2_F m;
    rt_->GetTransform(&m);
    return m;
}

void Canvas::SetTransform(const D2D1_MATRIX_3X2_F &m) { rt_->SetTransform(&m); }

void Canvas::Bitmap(ID2D1Bitmap *bmp, RectF dst, const RectF *srcPx, float opacity, bool nearest) {
    if (!bmp || dst.Empty()) return;
    D2D1_RECT_F d = dst.D2();
    D2D1_RECT_F s;
    if (srcPx) s = srcPx->D2();
    rt_->DrawBitmap(bmp, &d, opacity,
                    nearest ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                    srcPx ? &s : nullptr);
}

// ---------------------------------------------------------------- TiledBitmap

bool TiledBitmap::Create(ID2D1RenderTarget *rt, const ls_image &img) {
    Reset();
    if (!img.data || img.width <= 0 || img.height <= 0) return false;
    int maxSize = (int)rt->GetMaximumBitmapSize();
    if (maxSize <= 0) maxSize = 2048;
    if (maxSize > 8192) maxSize = 8192;
    w_ = img.width;
    h_ = img.height;
    for (int y = 0; y < img.height; y += maxSize) {
        for (int x = 0; x < img.width; x += maxSize) {
            Tile t;
            t.x = x;
            t.y = y;
            t.w = img.width - x < maxSize ? img.width - x : maxSize;
            t.h = img.height - y < maxSize ? img.height - y : maxSize;
            t.bmp = gfx::CreateBitmap(rt, img.data + (size_t)y * (size_t)img.stride + (size_t)x * 4, t.w, t.h, img.stride);
            if (!t.bmp) {
                LS_LOG("gfx: 分块位图创建失败 %dx%d", t.w, t.h);
                Reset();
                return false;
            }
            tiles_.push_back(t);
        }
    }
    return true;
}

void TiledBitmap::Reset() {
    tiles_.clear();
    w_ = h_ = 0;
}

void TiledBitmap::Draw(Canvas &c, RectF dst, RectF src, bool nearest, float opacity) const {
    if (tiles_.empty() || src.Empty() || dst.Empty()) return;
    float sx = dst.w / src.w, sy = dst.h / src.h;
    for (const Tile &t : tiles_) {
        RectF tr((float)t.x, (float)t.y, (float)t.w, (float)t.h);
        RectF in = src.Intersect(tr);
        if (in.Empty()) continue;
        RectF d(dst.x + (in.x - src.x) * sx, dst.y + (in.y - src.y) * sy, in.w * sx, in.h * sy);
        RectF s(in.x - (float)t.x, in.y - (float)t.y, in.w, in.h);
        c.Bitmap(t.bmp.Get(), d, &s, opacity, nearest);
    }
}

// ---------------------------------------------------------------- Offscreen

bool Offscreen::Create(int pxW, int pxH, float scale) {
    bmp_.Reset();
    rt_.Reset();
    w_ = pxW;
    h_ = pxH;
    if (pxW <= 0 || pxH <= 0 || !gfx::WIC()) return false;
    if (FAILED(gfx::WIC()->CreateBitmap((UINT)pxW, (UINT)pxH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                        bmp_.Put())))
        return false;
    D2D1_RENDER_TARGET_PROPERTIES props = {D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                           {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
                                           96.f * scale,
                                           96.f * scale,
                                           D2D1_RENDER_TARGET_USAGE_NONE,
                                           D2D1_FEATURE_LEVEL_DEFAULT};
    if (FAILED(gfx::D2D()->CreateWicBitmapRenderTarget(bmp_.Get(), &props, rt_.Put()))) {
        bmp_.Reset();
        return false;
    }
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

bool Offscreen::ReadPixels(ls_image &out) const {
    out = ls_image_alloc(w_, h_);
    if (!out.data || !bmp_) return false;
    WICRect rc = {0, 0, w_, h_};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bmp_->Lock(&rc, WICBitmapLockRead, lock.Put()))) {
        ls_image_free(&out);
        return false;
    }
    UINT stride = 0, size = 0;
    BYTE *p = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&size, &p);
    if (!p) {
        ls_image_free(&out);
        return false;
    }
    for (int y = 0; y < h_; ++y) std::memcpy(out.data + (size_t)y * out.stride, p + (size_t)y * stride, (size_t)w_ * 4);
    return true;
}

// ---------------------------------------------------------------- LayeredSurface

LayeredSurface::~LayeredSurface() { Free(); }

void LayeredSurface::Free() {
    rt_.Reset();
    if (dc_) {
        if (old_) SelectObject(dc_, old_);
        DeleteDC(dc_);
    }
    if (dib_) DeleteObject(dib_);
    dc_ = nullptr;
    dib_ = nullptr;
    old_ = nullptr;
    bits_ = nullptr;
    w_ = h_ = 0;
}

ID2D1RenderTarget *LayeredSurface::Begin(int pxW, int pxH, UINT dpi) {
    if (pxW <= 0) pxW = 1;
    if (pxH <= 0) pxH = 1;
    if (pxW != w_ || pxH != h_ || !dc_) {
        Free();
        HDC screen = GetDC(nullptr);
        dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!dc_) return nullptr;
        BITMAPINFO bi;
        std::memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = pxW;
        bi.bmiHeader.biHeight = -pxH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!dib_) {
            Free();
            return nullptr;
        }
        old_ = SelectObject(dc_, dib_);
        w_ = pxW;
        h_ = pxH;
    }
    if (!rt_) {
        D2D1_RENDER_TARGET_PROPERTIES props = {D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                               {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
                                               (float)dpi,
                                               (float)dpi,
                                               D2D1_RENDER_TARGET_USAGE_NONE,
                                               D2D1_FEATURE_LEVEL_DEFAULT};
        if (FAILED(gfx::D2D()->CreateDCRenderTarget(&props, rt_.Put()))) return nullptr;
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        ++gen_;
    }
    rt_->SetDpi((float)dpi, (float)dpi);
    RECT rc = {0, 0, w_, h_};
    if (FAILED(rt_->BindDC(dc_, &rc))) return nullptr;
    rt_->BeginDraw();
    rt_->SetTransform(MatIdentity());
    D2D1_COLOR_F clear = {0, 0, 0, 0};
    rt_->Clear(&clear);
    return rt_.Get();
}

bool LayeredSurface::End(HWND hwnd, int x, int y, BYTE alpha) {
    if (!rt_) return false;
    HRESULT hr = rt_->EndDraw(nullptr, nullptr);
    if (FAILED(hr)) {
        rt_.Reset();  // 下次重建
        return false;
    }
    GdiFlush();
    POINT dst = {x, y}, src = {0, 0};
    SIZE sz = {w_, h_};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    BOOL ok = UpdateLayeredWindow(hwnd, screen, &dst, &sz, dc_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    return ok != 0;
}

void LayeredSurface::SetAlpha(HWND hwnd, BYTE alpha) {
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
}

}  // namespace ls
