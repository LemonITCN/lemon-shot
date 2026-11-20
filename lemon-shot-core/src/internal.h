// 内核内部共用定义（不对外）。
#pragma once

#include "lemonshot.h"

#include <cstddef>
#include <vector>

#define LEMONSHOT_VERSION "0.1.0"

constexpr double kPi = 3.14159265358979323846;

struct ls_path_cmd {
    ls_path_verb verb;
    ls_point pts[3];
};

struct ls_path {
    std::vector<ls_path_cmd> verbs;

    void clear() { verbs.clear(); }
    void move_to(float x, float y) { verbs.push_back({LS_PATH_MOVE, {{x, y}, {0, 0}, {0, 0}}}); }
    void line_to(float x, float y) { verbs.push_back({LS_PATH_LINE, {{x, y}, {0, 0}, {0, 0}}}); }
    void cubic_to(float x1, float y1, float x2, float y2, float x, float y) {
        verbs.push_back({LS_PATH_CUBIC, {{x1, y1}, {x2, y2}, {x, y}}});
    }
    void close() { verbs.push_back({LS_PATH_CLOSE, {{0, 0}, {0, 0}, {0, 0}}}); }
};

namespace lsk {

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline uint8_t *row_ptr(const ls_image *im, int32_t y) { return im->data + (size_t)y * (size_t)im->stride; }

// 按行取像素的源：长截图的分块存储也能直接缩放，不必先拼成整张图。
typedef const uint8_t *(*RowFn)(const void *ctx, int32_t y);

// 缩放（缩小面积平均、放大双线性；两个方向各自判断）。out 新分配。
int32_t resize_rows(RowFn row, const void *ctx, int32_t sw, int32_t sh, int32_t dw, int32_t dh, ls_image *out);

// 像素化：块网格对齐到整张图的原点（同一张图上不同区域的块边界一致）。
// 把 src 的 (x,y,w,h) 区域结果写到 dst 的 (dx,dy) 处；区域超出 src 时按边缘夹取。
void pixelate_into(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, int32_t block, ls_image *dst,
                   int32_t dx, int32_t dy);

// 高斯模糊近似（三次盒式模糊，sigma 像素），边缘夹取；结果写到 dst 的 (dx,dy) 处。
void blur_into(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, float sigma, ls_image *dst,
               int32_t dx, int32_t dy);

}  // namespace lsk
