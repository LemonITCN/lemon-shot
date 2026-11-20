#include "Capture/Annotations.h"

#include <cmath>
#include <cstring>

#include "Settings/Theme.h"
#include "Util/Util.h"

namespace ls {

// ---------------------------------------------------------------- PatchCache

static uint64_t PatchSignature(const ls_shape_view &v) {
    const ls_image *p = v.patch;
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](const void *d, size_t n) { h = Fnv1a64(d, n, h); };
    mix(&v.patch_rect, sizeof v.patch_rect);
    mix(&v.style.block_size, sizeof v.style.block_size);
    mix(&v.style.opacity, sizeof v.style.opacity);
    mix(&v.style.variant, sizeof v.style.variant);
    mix(&v.style.line_width, sizeof v.style.line_width);
    mix(&v.bounds, sizeof v.bounds);
    // 稀疏取样像素：同一块内存被原地改写时也能发现
    if (p && p->data && p->width > 0 && p->height > 0) {
        for (int i = 0; i < 64; ++i) {
            int x = (int)((uint64_t)(i * 7919) % (uint64_t)p->width);
            int y = (int)((uint64_t)(i * 104729) % (uint64_t)p->height);
            mix(p->data + (size_t)y * p->stride + (size_t)x * 4, 4);
        }
    }
    return h;
}

ID2D1Bitmap *PatchCache::Get(ID2D1RenderTarget *rt, const ls_shape_view &v) {
    if (!v.patch || !v.patch->data) return nullptr;
    if (rt != rt_) {
        entries_.clear();
        rt_ = rt;
    }
    uint64_t sig = PatchSignature(v);
    for (auto &e : entries_) {
        if (e.id == v.id && e.ptr == v.patch->data && e.w == v.patch->width && e.h == v.patch->height && e.sig == sig) {
            e.used = true;
            return e.bmp.Get();
        }
    }
    // 同一个标注的旧缓存作废
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].id == v.id) {
            entries_.erase(entries_.begin() + (long)i);
            break;
        }
    }
    Entry e;
    e.id = v.id;
    e.ptr = v.patch->data;
    e.w = v.patch->width;
    e.h = v.patch->height;
    e.sig = sig;
    e.bmp = gfx::CreateBitmap(rt, *v.patch);
    e.used = true;
    if (!e.bmp) return nullptr;
    entries_.push_back(e);
    return entries_.back().bmp.Get();
}

void PatchCache::BeginFrame() {
    for (auto &e : entries_) e.used = false;
}

void PatchCache::EndFrame() {
    for (size_t i = 0; i < entries_.size();) {
        if (!entries_[i].used)
            entries_.erase(entries_.begin() + (long)i);
        else
            ++i;
    }
}

void PatchCache::Clear() {
    entries_.clear();
    rt_ = nullptr;
}

// ---------------------------------------------------------------- 描边文字

namespace {

// 把 DirectWrite 排好的字形转成几何（描边文字用：先描边再填充，两者完全对齐）
class OutlineCollector : public IDWriteTextRenderer {
public:
    std::vector<ComPtr<ID2D1Geometry>> geos;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { return --ref_; }  // 栈上对象，不自删

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, WINBOOL *disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *m) override {
        DWRITE_MATRIX id = {1, 0, 0, 1, 0, 0};
        *m = id;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *p) override {
        *p = 1.f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN *run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION *, IUnknown *) override {
        if (!run || !run->fontFace || run->glyphCount == 0) return S_OK;
        ComPtr<ID2D1PathGeometry> path;
        if (FAILED(gfx::D2D()->CreatePathGeometry(path.Put()))) return S_OK;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(path->Open(sink.Put()))) return S_OK;
        run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                                          run->glyphCount, run->isSideways, (run->bidiLevel & 1) ? TRUE : FALSE,
                                          sink.Get());
        sink->Close();
        D2D1_MATRIX_3X2_F m = MatTranslate(x, y);
        ComPtr<ID2D1TransformedGeometry> tg;
        if (SUCCEEDED(gfx::D2D()->CreateTransformedGeometry(path.Get(), &m, tg.Put()))) {
            ComPtr<ID2D1Geometry> g;
            g.Attach(tg.Detach());
            geos.push_back(g);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT, FLOAT, const DWRITE_UNDERLINE *, IUnknown *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH *, IUnknown *) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void *, FLOAT, FLOAT, IDWriteInlineObject *, WINBOOL, WINBOOL,
                                               IUnknown *) override {
        return S_OK;
    }

private:
    ULONG ref_ = 1;
};

}  // namespace

void DrawOutlinedText(Canvas &c, IDWriteTextLayout *lay, float x, float y, Color fill, Color outline, float width) {
    if (!lay) return;
    OutlineCollector col;
    lay->Draw(nullptr, &col, x, y);
    for (auto &g : col.geos) c.StrokeGeometry(g.Get(), outline, width, gfx::RoundStroke());
    for (auto &g : col.geos) c.FillGeometry(g.Get(), fill);
}

namespace {

void DrawShapeText(Canvas &c, const ls_shape_view &v) {
    if (v.editing || !v.text || !v.text[0]) return;
    std::wstring text = Widen(std::string(v.text));
    bool bold = v.type == LS_TOOL_STEP ? true : v.style.bold != 0;
    ComPtr<IDWriteTextLayout> lay = gfx::AnnotationLayout(text, v.text_size, bold);
    if (!lay) return;
    RectF tr(v.text_rect);
    float x = tr.x, y = tr.y;
    if (v.type == LS_TOOL_STEP) {
        // 序号：在 text_rect 里水平垂直居中
        lay->SetMaxWidth(tr.w > 0 ? tr.w : 1);
        lay->SetMaxHeight(tr.h > 0 ? tr.h : 1);
        lay->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (v.text_background.a > 0) {
        c.FillRound(tr.Inflate(4), 4, LsToColor(v.text_background));
    }
    if (v.text_outline.a > 0) {
        DrawOutlinedText(c, lay.Get(), x, y, LsToColor(v.text_color), LsToColor(v.text_outline), v.text_size / 8.f);
        return;
    }
    c.TextLayout(lay.Get(), x, y, LsToColor(v.text_color));
}

}  // namespace

// ---------------------------------------------------------------- 绘制

void DrawShape(Canvas &c, const ls_shape_view &v, const AnnotationContext &ctx) {
    Color color = LsToColor(v.style.color);
    float opacity = v.style.opacity;
    if (opacity <= 0) return;
    if (v.type == LS_SHAPE_MASK) {
        if (!v.fill) return;
        ComPtr<ID2D1PathGeometry> g = gfx::PathGeometry(v.fill, true);
        Color mc = LsToColor(v.fill_color);
        if (mc.a <= 0) mc = Color{0, 0, 0, 1};
        c.FillGeometry(g.Get(), WithAlpha(mc, opacity));
        return;
    }
    switch (v.type) {
    case LS_TOOL_MOSAIC: {
        if (!ctx.patches) return;
        ID2D1Bitmap *bmp = ctx.patches->Get(c.RT(), v);
        if (bmp) c.Bitmap(bmp, RectF(v.patch_rect), nullptr, 1.f, false);
        return;
    }
    case LS_TOOL_MAGNIFY: {
        if (v.radius <= 0) return;
        float cx = v.p0.x, cy = v.p0.y, r = v.radius;
        if (ctx.base && ctx.base->Valid()) {
            ComPtr<ID2D1EllipseGeometry> circle;
            D2D1_ELLIPSE e = {{cx, cy}, r, r};
            gfx::D2D()->CreateEllipseGeometry(&e, circle.Put());
            c.PushGeometryMask(circle.Get(), opacity);
            RectF src(v.magnify_src.x * ctx.scale, v.magnify_src.y * ctx.scale, v.magnify_src.w * ctx.scale,
                      v.magnify_src.h * ctx.scale);
            ctx.base->Draw(c, RectF(cx - r, cy - r, 2 * r, 2 * r), src, false);
            c.PopLayer();
        }
        if (v.stroke) {
            ComPtr<ID2D1PathGeometry> g = gfx::PathGeometry(v.stroke, true);
            if (opacity < 1) c.PushOpacity(opacity, RectF(v.bounds).Inflate(2));
            c.StrokeGeometry(g.Get(), color, v.style.line_width, gfx::RoundStroke());
            if (opacity < 1) c.PopLayer();
        }
        return;
    }
    default: break;
    }
    bool hasText = (v.type == LS_TOOL_TEXT || v.type == LS_TOOL_STEP) && v.text;
    if (!v.fill && !v.stroke && !hasText) return;  // 局部高亮本身不画（合成在 MASK 里）
    bool layer = opacity < 0.999f;
    if (layer) c.PushOpacity(opacity, RectF(v.bounds).Inflate(v.style.line_width + 4));
    if (v.fill) {
        ComPtr<ID2D1PathGeometry> g = gfx::PathGeometry(v.fill, true);
        // fill 用 fill_color（空心序号为白色），不是 style.color
        c.FillGeometry(g.Get(), LsToColor(v.fill_color));
    }
    if (v.stroke) {
        ComPtr<ID2D1PathGeometry> g = gfx::PathGeometry(v.stroke, true);
        c.StrokeGeometry(g.Get(), color, v.style.line_width, v.dashed ? gfx::DashStroke() : gfx::RoundStroke());
    }
    if (hasText) DrawShapeText(c, v);
    if (layer) c.PopLayer();
}

void DrawAnnotations(Canvas &c, const ls_session *s, const AnnotationContext &ctx, RectF clip) {
    int n = ls_session_shape_count(s);
    if (n <= 0) return;
    c.PushClip(clip);
    for (int i = 0; i < n; ++i) {
        ls_shape_view v;
        std::memset(&v, 0, sizeof v);
        ls_session_shape_at(s, i, &v);
        DrawShape(c, v, ctx);
    }
    c.PopClip();
}

void DrawShapeHandles(Canvas &c, const ls_session *s) {
    ls_point pts[9];
    int n = ls_session_shape_handles(s, pts, 9);
    const auto &cap = theme::Cap();
    float d = theme::Tok().capture.handleSize;
    if (n <= 0) {
        // 没有控制点的标注（画笔、文字、序号等）：选中时画虚线外框
        int count = ls_session_shape_count(s);
        for (int i = 0; i < count; ++i) {
            ls_shape_view v;
            std::memset(&v, 0, sizeof v);
            ls_session_shape_at(s, i, &v);
            if (v.selected && !v.editing && v.type != LS_SHAPE_MASK) {
                RectF b = RectF(v.bounds).Inflate(0.5f);
                c.Line(b.x, b.y, b.r(), b.y, cap.shapeSelectionDash, 1, gfx::UiDashStroke());
                c.Line(b.r(), b.y, b.r(), b.b(), cap.shapeSelectionDash, 1, gfx::UiDashStroke());
                c.Line(b.r(), b.b(), b.x, b.b(), cap.shapeSelectionDash, 1, gfx::UiDashStroke());
                c.Line(b.x, b.b(), b.x, b.y, cap.shapeSelectionDash, 1, gfx::UiDashStroke());
            }
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        c.FillEllipse(pts[i].x, pts[i].y, d / 2, d / 2, cap.shapeHandleFill);
        c.StrokeEllipse(pts[i].x, pts[i].y, d / 2, d / 2, cap.shapeHandleBorder, 1);
    }
}

}  // namespace ls
