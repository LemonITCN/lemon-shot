// 标注绘制（spec 2.5）：按内核给的 ls_shape_view 原样绘制。屏幕、导出、贴图标注、自测共用。
#pragma once

#include <vector>

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {

// 马赛克/模糊像素图在某个渲染目标上的位图缓存：内容不变就不重复上传
class PatchCache {
public:
    ID2D1Bitmap *Get(ID2D1RenderTarget *rt, const ls_shape_view &v);
    void BeginFrame();
    void EndFrame();  // 丢掉本帧没用到的
    void Clear();

private:
    struct Entry {
        int id;
        const uint8_t *ptr;
        int w, h;
        uint64_t sig;
        ComPtr<ID2D1Bitmap> bmp;
        bool used;
    };
    std::vector<Entry> entries_;
    ID2D1RenderTarget *rt_ = nullptr;
};

struct AnnotationContext {
    const TiledBitmap *base = nullptr;  // 冻结画面（放大镜标注用）
    float scale = 1;                    // 冻结画面每逻辑点像素数
    PatchCache *patches = nullptr;
};

void DrawShape(Canvas &c, const ls_shape_view &v, const AnnotationContext &ctx);
// 画全部标注；clip 为选区（逻辑点）
void DrawAnnotations(Canvas &c, const ls_session *s, const AnnotationContext &ctx, RectF clip);
// 描边文字：先描边再填充（字形几何完全对齐）
void DrawOutlinedText(Canvas &c, IDWriteTextLayout *lay, float x, float y, Color fill, Color outline, float width);
// 选中标注的控制点（没有控制点的标注画虚线外框）
void DrawShapeHandles(Canvas &c, const ls_session *s);

}  // namespace ls
