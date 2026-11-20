// 绘图基础：Direct2D 1.0 + DirectWrite + WIC（Win7 SP1 起都有）。
// 所有界面都写成「画进给定 ID2D1RenderTarget」的函数，坐标为逻辑点；屏幕窗口与自测离屏共用同一份绘制代码。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <string>
#include <vector>

#include "Settings/Theme.h"
#include "Util/ComPtr.h"
#include "lemonshot.h"

namespace ls {

struct SizeF {
    float w = 0, h = 0;
};

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    RectF() = default;
    RectF(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    explicit RectF(const ls_rect &r) : x(r.x), y(r.y), w(r.w), h(r.h) {}
    float r() const { return x + w; }
    float b() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool Empty() const { return w <= 0 || h <= 0; }
    RectF Inflate(float d) const { return RectF(x - d, y - d, w + 2 * d, h + 2 * d); }
    RectF Inflate(float dx, float dy) const { return RectF(x - dx, y - dy, w + 2 * dx, h + 2 * dy); }
    RectF Offset(float dx, float dy) const { return RectF(x + dx, y + dy, w, h); }
    RectF Intersect(const RectF &o) const;
    D2D1_RECT_F D2() const { return D2D1_RECT_F{x, y, x + w, y + h}; }
    ls_rect Ls() const { return ls_rect{x, y, w, h}; }
};

inline D2D1_COLOR_F D2C(Color c) { return D2D1_COLOR_F{c.r, c.g, c.b, c.a}; }
inline Color LsToColor(ls_color c) { return Color{c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f}; }
inline D2D1_POINT_2F P2(float x, float y) { return D2D1_POINT_2F{x, y}; }
D2D1_MATRIX_3X2_F MatIdentity();
D2D1_MATRIX_3X2_F MatTranslate(float dx, float dy);
D2D1_MATRIX_3X2_F MatScale(float sx, float sy, float cx = 0, float cy = 0);
D2D1_MATRIX_3X2_F MatMul(const D2D1_MATRIX_3X2_F &a, const D2D1_MATRIX_3X2_F &b);  // 先 a 后 b

enum FontWeight { kRegular = 400, kMedium = 500, kSemibold = 600, kBold = 700 };
enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Center, Bottom };

struct TextStyle {
    float size = 13;
    int weight = kRegular;
    Color color{0, 0, 0, 1};
    HAlign h = HAlign::Left;
    VAlign v = VAlign::Center;
    bool mono = false;
    bool tabular = false;   // 等宽数字
    bool wrap = false;
    bool ellipsis = false;  // 超宽用 … 截断
};

namespace gfx {

bool Init();  // 失败返回 false（没有 D2D 的极端环境）
void Shutdown();
ID2D1Factory *D2D();
IDWriteFactory *DW();
IWICImagingFactory *WIC();

ID2D1StrokeStyle *RoundStroke();  // 圆头圆角
ID2D1StrokeStyle *DashStroke();   // 圆头圆角，虚线 3×宽 / 2×宽（标注）
ID2D1StrokeStyle *UiDashStroke(); // 平头虚线 4/3（界面辅助线）

IDWriteTextFormat *Font(float size, int weight, bool mono = false);
const std::wstring &FontFamily();

struct TextSize {
    float w = 0, h = 0;
};
TextSize Measure(const std::wstring &text, float size, int weight, bool mono = false, float maxWidth = 100000.f);
// 给 ls_session_set_text_measurer 的测量回调（与标注文字同一字体）
void SessionMeasure(void *ctx, const char *utf8, float font_size, int32_t bold, float *out_w, float *out_h);
// 标注文字排版（多行按 \n），宽度不限
ComPtr<IDWriteTextLayout> AnnotationLayout(const std::wstring &text, float size, bool bold);

// ls_path → D2D 路径几何（evenOdd：奇偶填充规则）
ComPtr<ID2D1PathGeometry> PathGeometry(const ls_path *path, bool evenOdd = true);
// 带变换的 ls_path（缩放 + 平移）
ComPtr<ID2D1PathGeometry> PathGeometryXf(const ls_path *path, float scale, float dx, float dy, bool evenOdd = true);

ComPtr<ID2D1Bitmap> CreateBitmap(ID2D1RenderTarget *rt, const uint8_t *bgraPremul, int w, int h, int stride);
inline ComPtr<ID2D1Bitmap> CreateBitmap(ID2D1RenderTarget *rt, const ls_image &img) {
    return CreateBitmap(rt, img.data, img.width, img.height, img.stride);
}

// HWND 渲染目标：像素尺寸 = 客户区，DPI = 窗口 DPI（按逻辑点绘制）
ComPtr<ID2D1HwndRenderTarget> CreateHwndTarget(HWND hwnd, UINT dpi);

}  // namespace gfx

// 一次绘制的上下文：复用一个纯色画刷，管理图层与变换
class Canvas {
public:
    explicit Canvas(ID2D1RenderTarget *rt);
    ~Canvas();
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;

    ID2D1RenderTarget *RT() const { return rt_; }
    ID2D1SolidColorBrush *Brush(Color c);

    void Clear(Color c);
    void FillRect(RectF r, Color c);
    void StrokeRect(RectF r, Color c, float w);
    void FillRound(RectF r, float radius, Color c);
    void StrokeRound(RectF r, float radius, Color c, float w);
    void FillEllipse(float cx, float cy, float rx, float ry, Color c);
    void StrokeEllipse(float cx, float cy, float rx, float ry, Color c, float w, ID2D1StrokeStyle *style = nullptr);
    void Line(float x0, float y0, float x1, float y1, Color c, float w, ID2D1StrokeStyle *style = nullptr);
    void FillGeometry(ID2D1Geometry *g, Color c);
    void StrokeGeometry(ID2D1Geometry *g, Color c, float w, ID2D1StrokeStyle *style = nullptr);

    void Text(const std::wstring &s, RectF r, const TextStyle &st);
    void TextLayout(IDWriteTextLayout *layout, float x, float y, Color c);

    // 图标（icons.json，24 网格）：以 (cx, cy) 为中心、边长 size；stroke 为逻辑线宽（<0 用 toolbar.iconStroke）
    void Icon(const char *name, float cx, float cy, float size, Color c, float stroke = -1.f, bool fill = false);

    // 近似高斯投影（D2D 1.0 没有阴影效果：用逐层外扩的半透明圆角矩形叠加）
    void Shadow(RectF r, float radius, float blur, float offsetY, Color c);

    void PushClip(RectF r);
    void PopClip();
    // 透明层：整体不透明度一次合成（重叠部分不叠加变深）
    void PushOpacity(float opacity, RectF bounds);
    // 几何遮罩（可带不透明度）
    void PushGeometryMask(ID2D1Geometry *g, float opacity = 1.f);
    void PopLayer();

    D2D1_MATRIX_3X2_F GetTransform() const;
    void SetTransform(const D2D1_MATRIX_3X2_F &m);

    void Bitmap(ID2D1Bitmap *bmp, RectF dst, const RectF *srcPx, float opacity = 1.f, bool nearest = false);

private:
    ID2D1Layer *NextLayer();

    ID2D1RenderTarget *rt_;
    ID2D1SolidColorBrush *brush_ = nullptr;
    std::vector<ID2D1Layer *> layers_;
    size_t layerDepth_ = 0;
};

// 大图分块上传（老显卡最大纹理 2048/4096），绘制时按块映射
class TiledBitmap {
public:
    bool Create(ID2D1RenderTarget *rt, const ls_image &img);
    void Reset();
    bool Valid() const { return !tiles_.empty(); }
    int Width() const { return w_; }
    int Height() const { return h_; }
    // srcPx：像素矩形；dst：逻辑点矩形
    void Draw(Canvas &c, RectF dst, RectF srcPx, bool nearest, float opacity = 1.f) const;

private:
    struct Tile {
        ComPtr<ID2D1Bitmap> bmp;
        int x, y, w, h;
    };
    std::vector<Tile> tiles_;
    int w_ = 0, h_ = 0;
};

// 离屏渲染：WIC 位图渲染目标（软件渲染），自测与导出用
class Offscreen {
public:
    bool Create(int pxW, int pxH, float scale);
    ID2D1RenderTarget *RT() const { return rt_.Get(); }
    IWICBitmap *Wic() const { return bmp_.Get(); }
    int Width() const { return w_; }
    int Height() const { return h_; }
    // 复制出预乘 BGRA（ls_image_alloc 分配，调用者 ls_image_free）
    bool ReadPixels(ls_image &out) const;

private:
    ComPtr<IWICBitmap> bmp_;
    ComPtr<ID2D1RenderTarget> rt_;
    int w_ = 0, h_ = 0;
};

// 分层窗口表面：D2D 画进 32 位 DIB，再 UpdateLayeredWindow（逐像素透明）
class LayeredSurface {
public:
    ~LayeredSurface();
    // 返回可绘制的渲染目标（已 BeginDraw、清成透明）；失败返回 nullptr
    ID2D1RenderTarget *Begin(int pxW, int pxH, UINT dpi);
    // EndDraw 并提交到窗口；x、y 为屏幕像素坐标
    bool End(HWND hwnd, int x, int y, BYTE alpha);
    static void SetAlpha(HWND hwnd, BYTE alpha);
    int Width() const { return w_; }
    int Height() const { return h_; }
    // 渲染目标重建时加 1：调用方据此重建绑定在渲染目标上的位图
    unsigned Generation() const { return gen_; }

private:
    void Free();
    unsigned gen_ = 0;
    HDC dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void *bits_ = nullptr;
    int w_ = 0, h_ = 0;
    ComPtr<ID2D1DCRenderTarget> rt_;
};

}  // namespace ls
