// 像素处理：像素化、模糊、裁剪、缩放、旋转、翻转、圆角投影、合成。像素格式 BGRA8 预乘。
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

using lsk::clampi;
using lsk::row_ptr;

namespace {

ls_image empty_image() {
    ls_image im;
    im.data = nullptr;
    im.width = im.height = im.stride = 0;
    return im;
}

bool valid(const ls_image *im) { return im && im->data && im->width > 0 && im->height > 0 && im->stride >= im->width * 4; }

// ---------------------------------------------------------------- 盒式模糊

// 三个盒子的宽度，使三次盒式模糊近似标准差 sigma 的高斯（W. Jarosz / I. Kutskir 的做法）。
void box_sizes(float sigma, int sizes[3]) {
    const int n = 3;
    double w_ideal = std::sqrt(12.0 * sigma * sigma / n + 1);
    int wl = (int)std::floor(w_ideal);
    if (wl % 2 == 0) --wl;
    if (wl < 1) wl = 1;
    int wu = wl + 2;
    double m_ideal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4);
    int m = (int)std::lround(m_ideal);
    for (int i = 0; i < n; ++i) sizes[i] = i < m ? wl : wu;
}

template <int C>
void box_h(const uint8_t *src, uint8_t *dst, int w, int h, int r) {
    const int d = 2 * r + 1;
    const float inv = 1.0f / (float)d;
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = src + (size_t)y * w * C;
        uint8_t *o = dst + (size_t)y * w * C;
        int sum[C];
        for (int c = 0; c < C; ++c) sum[c] = 0;
        for (int i = -r; i <= r; ++i) {
            const uint8_t *p = s + clampi(i, 0, w - 1) * C;
            for (int c = 0; c < C; ++c) sum[c] += p[c];
        }
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < C; ++c) {
                int v = (int)((float)sum[c] * inv + 0.5f);
                o[x * C + c] = (uint8_t)(v > 255 ? 255 : v);
            }
            const uint8_t *pin = s + clampi(x + r + 1, 0, w - 1) * C;
            const uint8_t *pout = s + clampi(x - r, 0, w - 1) * C;
            for (int c = 0; c < C; ++c) sum[c] += pin[c] - pout[c];
        }
    }
}

template <int C>
void box_v(const uint8_t *src, uint8_t *dst, int w, int h, int r, std::vector<int> &sums) {
    const int d = 2 * r + 1;
    const float inv = 1.0f / (float)d;
    const size_t rowlen = (size_t)w * C;
    sums.assign(rowlen, 0);
    for (int i = -r; i <= r; ++i) {
        const uint8_t *p = src + (size_t)clampi(i, 0, h - 1) * rowlen;
        for (size_t k = 0; k < rowlen; ++k) sums[k] += p[k];
    }
    for (int y = 0; y < h; ++y) {
        uint8_t *o = dst + (size_t)y * rowlen;
        for (size_t k = 0; k < rowlen; ++k) {
            int v = (int)((float)sums[k] * inv + 0.5f);
            o[k] = (uint8_t)(v > 255 ? 255 : v);
        }
        const uint8_t *pin = src + (size_t)clampi(y + r + 1, 0, h - 1) * rowlen;
        const uint8_t *pout = src + (size_t)clampi(y - r, 0, h - 1) * rowlen;
        for (size_t k = 0; k < rowlen; ++k) sums[k] += pin[k] - pout[k];
    }
}

// 原地三次盒式模糊（横三次、竖三次），buf 紧密排列 w*h*C。
template <int C>
void box3(std::vector<uint8_t> &buf, int w, int h, float sigma) {
    if (sigma < 0.4f || w <= 0 || h <= 0) return;
    int sizes[3];
    box_sizes(sigma, sizes);
    std::vector<uint8_t> tmp(buf.size());
    std::vector<int> sums;
    uint8_t *a = buf.data();
    uint8_t *b = tmp.data();
    for (int i = 0; i < 3; ++i) {
        int r = (sizes[i] - 1) / 2;
        if (r <= 0) continue;
        box_h<C>(a, b, w, h, r);
        std::swap(a, b);
    }
    for (int i = 0; i < 3; ++i) {
        int r = (sizes[i] - 1) / 2;
        if (r <= 0) continue;
        box_v<C>(a, b, w, h, r, sums);
        std::swap(a, b);
    }
    if (a != buf.data()) std::memcpy(buf.data(), a, buf.size());
}

int blur_pad(float sigma) {
    if (sigma < 0.4f) return 0;
    int sizes[3];
    box_sizes(sigma, sizes);
    return (sizes[0] - 1) / 2 + (sizes[1] - 1) / 2 + (sizes[2] - 1) / 2;
}

// ---------------------------------------------------------------- 缩放权重

struct Weights {
    std::vector<int> start;   // 每个目标像素的第一个源像素
    std::vector<int> count;   // 源像素个数
    std::vector<int> offset;  // 在 w 里的起始下标
    std::vector<float> w;
};

void build_weights(int ns, int nd, Weights &out) {
    out.start.resize((size_t)nd);
    out.count.resize((size_t)nd);
    out.offset.resize((size_t)nd);
    out.w.clear();
    const double scale = (double)ns / (double)nd;
    for (int i = 0; i < nd; ++i) {
        out.offset[(size_t)i] = (int)out.w.size();
        if (nd < ns) {
            // 缩小：目标像素覆盖源区间 [a, b)，按覆盖长度加权
            double a = i * scale, b = (i + 1) * scale;
            int j0 = (int)std::floor(a);
            int j1 = (int)std::ceil(b) - 1;
            if (j1 >= ns) j1 = ns - 1;
            out.start[(size_t)i] = j0;
            out.count[(size_t)i] = j1 - j0 + 1;
            for (int j = j0; j <= j1; ++j) {
                double lo = j > a ? j : a;
                double hi = j + 1 < b ? j + 1 : b;
                out.w.push_back((float)((hi - lo) / scale));
            }
        } else {
            // 放大或等大：双线性
            double c = (i + 0.5) * scale - 0.5;
            int j0 = (int)std::floor(c);
            double f = c - j0;
            int a0 = clampi(j0, 0, ns - 1), a1 = clampi(j0 + 1, 0, ns - 1);
            if (a0 == a1 || f < 1e-9) {
                out.start[(size_t)i] = f < 1e-9 ? a0 : a1;
                out.count[(size_t)i] = 1;
                out.w.push_back(1.0f);
            } else {
                out.start[(size_t)i] = a0;
                out.count[(size_t)i] = 2;
                out.w.push_back((float)(1 - f));
                out.w.push_back((float)f);
            }
        }
    }
}

const uint8_t *image_row(const void *ctx, int32_t y) { return row_ptr((const ls_image *)ctx, y); }

}  // namespace

namespace lsk {

int32_t resize_rows(RowFn row, const void *ctx, int32_t sw, int32_t sh, int32_t dw, int32_t dh, ls_image *out) {
    if (!out) return -1;
    *out = empty_image();
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return -1;
    *out = ls_image_alloc(dw, dh);
    if (!out->data) return -1;
    Weights wx, wy;
    build_weights(sw, dw, wx);
    build_weights(sh, dh, wy);
    std::vector<float> acc((size_t)sw * 4);
    for (int y = 0; y < dh; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        const int ys = wy.start[(size_t)y], yc = wy.count[(size_t)y], yo = wy.offset[(size_t)y];
        for (int k = 0; k < yc; ++k) {
            const float wt = wy.w[(size_t)(yo + k)];
            const uint8_t *s = row(ctx, ys + k);
            for (size_t i = 0; i < acc.size(); ++i) acc[i] += wt * (float)s[i];
        }
        uint8_t *o = row_ptr(out, y);
        for (int x = 0; x < dw; ++x) {
            const int xs = wx.start[(size_t)x], xc = wx.count[(size_t)x], xo = wx.offset[(size_t)x];
            float c[4] = {0, 0, 0, 0};
            for (int k = 0; k < xc; ++k) {
                const float wt = wx.w[(size_t)(xo + k)];
                const float *p = &acc[(size_t)(xs + k) * 4];
                c[0] += wt * p[0];
                c[1] += wt * p[1];
                c[2] += wt * p[2];
                c[3] += wt * p[3];
            }
            int a = clampi((int)(c[3] + 0.5f), 0, 255);
            for (int ch = 0; ch < 3; ++ch) o[x * 4 + ch] = (uint8_t)clampi((int)(c[ch] + 0.5f), 0, a);
            o[x * 4 + 3] = (uint8_t)a;
        }
    }
    return 0;
}

void pixelate_into(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, int32_t block, ls_image *dst,
                   int32_t dx, int32_t dy) {
    const int W = src->width, H = src->height;
    const int B = block < 1 ? 1 : block;
    const int cx0 = clampi(x, 0, W - 1), cx1 = clampi(x + w - 1, 0, W - 1);
    const int cy0 = clampi(y, 0, H - 1), cy1 = clampi(y + h - 1, 0, H - 1);
    const int bx0 = cx0 / B, bx1 = cx1 / B, by0 = cy0 / B, by1 = cy1 / B;
    const int nbx = bx1 - bx0 + 1, nby = by1 - by0 + 1;
    std::vector<uint8_t> avg((size_t)nbx * nby * 4);
    std::vector<uint32_t> sums((size_t)nbx * 4);
    for (int bj = 0; bj < nby; ++bj) {
        const int py0 = (by0 + bj) * B, py1 = py0 + B < H ? py0 + B : H;
        std::fill(sums.begin(), sums.end(), 0u);
        const int px0 = bx0 * B, px1 = (bx1 + 1) * B < W ? (bx1 + 1) * B : W;
        for (int py = py0; py < py1; ++py) {
            const uint8_t *s = row_ptr(src, py);
            for (int px = px0; px < px1; ++px) {
                uint32_t *sm = &sums[(size_t)(px / B - bx0) * 4];
                const uint8_t *p = s + px * 4;
                sm[0] += p[0];
                sm[1] += p[1];
                sm[2] += p[2];
                sm[3] += p[3];
            }
        }
        for (int bi = 0; bi < nbx; ++bi) {
            const int bpx0 = (bx0 + bi) * B, bpx1 = bpx0 + B < W ? bpx0 + B : W;
            const uint32_t n = (uint32_t)((bpx1 - bpx0) * (py1 - py0));
            uint8_t *a = &avg[((size_t)bj * nbx + bi) * 4];
            const uint32_t *sm = &sums[(size_t)bi * 4];
            for (int c = 0; c < 4; ++c) a[c] = (uint8_t)((sm[c] + n / 2) / n);
        }
    }
    std::vector<int> colblock((size_t)w);
    for (int ox = 0; ox < w; ++ox) colblock[(size_t)ox] = clampi(x + ox, 0, W - 1) / B - bx0;
    for (int oy = 0; oy < h; ++oy) {
        const int ty = dy + oy;
        if (ty < 0 || ty >= dst->height) continue;
        const int bj = clampi(y + oy, 0, H - 1) / B - by0;
        const uint8_t *arow = &avg[(size_t)bj * nbx * 4];
        uint8_t *o = row_ptr(dst, ty);
        for (int ox = 0; ox < w; ++ox) {
            const int tx = dx + ox;
            if (tx < 0 || tx >= dst->width) continue;
            std::memcpy(o + tx * 4, arow + colblock[(size_t)ox] * 4, 4);
        }
    }
}

void blur_into(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, float sigma, ls_image *dst,
               int32_t dx, int32_t dy) {
    const int W = src->width, H = src->height;
    const int pad = blur_pad(sigma);
    const int bw = w + 2 * pad, bh = h + 2 * pad;
    std::vector<uint8_t> buf((size_t)bw * bh * 4);
    for (int j = 0; j < bh; ++j) {
        const uint8_t *s = row_ptr(src, clampi(y - pad + j, 0, H - 1));
        uint8_t *o = &buf[(size_t)j * bw * 4];
        // 中间连续的一段直接整段复制，两头按边缘夹取
        for (int i = 0; i < bw; ++i) {
            const int sx = x - pad + i;
            if (sx >= 0 && sx < W) {
                int run = bw - i;
                if (sx + run > W) run = W - sx;
                std::memcpy(o + i * 4, s + sx * 4, (size_t)run * 4);
                i += run - 1;
            } else {
                std::memcpy(o + i * 4, s + clampi(sx, 0, W - 1) * 4, 4);
            }
        }
    }
    box3<4>(buf, bw, bh, sigma);
    for (int j = 0; j < h; ++j) {
        const int ty = dy + j;
        if (ty < 0 || ty >= dst->height) continue;
        int x0 = 0, x1 = w;
        if (dx + x0 < 0) x0 = -dx;
        if (dx + x1 > dst->width) x1 = dst->width - dx;
        if (x1 <= x0) continue;
        std::memcpy(row_ptr(dst, ty) + (dx + x0) * 4, &buf[((size_t)(j + pad) * bw + pad + x0) * 4], (size_t)(x1 - x0) * 4);
    }
}

}  // namespace lsk

extern "C" {

int32_t ls_pixelate(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, int32_t block, ls_image *out) {
    if (!out) return -1;
    *out = empty_image();
    if (!valid(src) || w <= 0 || h <= 0) return -1;
    *out = ls_image_alloc(w, h);
    if (!out->data) return -1;
    lsk::pixelate_into(src, x, y, w, h, block, out, 0, 0);
    return 0;
}

// radius 即高斯标准差（像素），与 CSS blur()、Core Image 的半径含义一致。
int32_t ls_blur(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, float radius, ls_image *out) {
    if (!out) return -1;
    *out = empty_image();
    if (!valid(src) || w <= 0 || h <= 0) return -1;
    *out = ls_image_alloc(w, h);
    if (!out->data) return -1;
    lsk::blur_into(src, x, y, w, h, radius, out, 0, 0);
    return 0;
}

// 超出原图的部分为透明。
int32_t ls_image_crop(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, ls_image *out) {
    if (!out) return -1;
    *out = empty_image();
    if (!valid(src) || w <= 0 || h <= 0) return -1;
    *out = ls_image_alloc(w, h);
    if (!out->data) return -1;
    const int x0 = x > 0 ? x : 0, y0 = y > 0 ? y : 0;
    const int x1 = x + w < src->width ? x + w : src->width;
    const int y1 = y + h < src->height ? y + h : src->height;
    if (x1 <= x0 || y1 <= y0) return 0;
    for (int j = y0; j < y1; ++j)
        std::memcpy(row_ptr(out, j - y) + (x0 - x) * 4, row_ptr(src, j) + x0 * 4, (size_t)(x1 - x0) * 4);
    return 0;
}

int32_t ls_image_resize(const ls_image *src, int32_t w, int32_t h, ls_image *out) {
    if (!out) return -1;
    *out = empty_image();
    if (!valid(src)) return -1;
    return lsk::resize_rows(image_row, src, src->width, src->height, w, h, out);
}

int32_t ls_image_rotate90(const ls_image *src, int32_t clockwise, ls_image *out) {
    if (!out) return -1;
    *out = empty_image();
    if (!valid(src)) return -1;
    const int W = src->width, H = src->height;
    *out = ls_image_alloc(H, W);
    if (!out->data) return -1;
    for (int y = 0; y < H; ++y) {
        const uint32_t *s = (const uint32_t *)(const void *)row_ptr(src, y);
        for (int x = 0; x < W; ++x) {
            // 顺时针：(x, y) -> (H-1-y, x)；逆时针：(x, y) -> (y, W-1-x)
            int ox = clockwise ? H - 1 - y : y;
            int oy = clockwise ? x : W - 1 - x;
            uint32_t v;
            std::memcpy(&v, s + x, 4);
            std::memcpy(row_ptr(out, oy) + ox * 4, &v, 4);
        }
    }
    return 0;
}

int32_t ls_image_flip(const ls_image *src, int32_t horizontal, ls_image *out) {
    if (!out) return -1;
    *out = empty_image();
    if (!valid(src)) return -1;
    const int W = src->width, H = src->height;
    *out = ls_image_alloc(W, H);
    if (!out->data) return -1;
    for (int y = 0; y < H; ++y) {
        if (horizontal) {
            const uint8_t *s = row_ptr(src, y);
            uint8_t *o = row_ptr(out, y);
            for (int x = 0; x < W; ++x) std::memcpy(o + (W - 1 - x) * 4, s + x * 4, 4);
        } else {
            std::memcpy(row_ptr(out, H - 1 - y), row_ptr(src, y), (size_t)W * 4);
        }
    }
    return 0;
}

// 圆角 + 投影。shadow_radius 是投影的模糊半径（约 2 倍标准差，与 CSS box-shadow 的 blur 含义一致）。
// 输出四周各留 ceil(shadow_radius + |shadow_offset_y|) 的透明边；没有投影时不留边。
int32_t ls_image_frame(const ls_image *src, float radius, float shadow_radius, float shadow_offset_y, ls_color shadow,
                       ls_image *out) {
    if (!out) return -1;
    *out = empty_image();
    if (!valid(src)) return -1;
    const int W = src->width, H = src->height;
    float r = radius < 0 ? 0 : radius;
    const float rmax = (float)(W < H ? W : H) * 0.5f;
    if (r > rmax) r = rmax;
    const float sr = shadow_radius > 0 ? shadow_radius : 0;
    const int off = (int)std::lround(shadow_offset_y);
    const int pad = sr > 0 ? (int)std::ceil(sr + std::fabs(shadow_offset_y)) : 0;
    const int OW = W + 2 * pad, OH = H + 2 * pad;
    *out = ls_image_alloc(OW, OH);
    if (!out->data) return -1;

    // 圆角后的原图（预乘，整体乘覆盖率）
    ls_image body = ls_image_alloc(W, H);
    if (!body.data) {
        ls_image_free(out);
        return -1;
    }
    for (int y = 0; y < H; ++y) std::memcpy(row_ptr(&body, y), row_ptr(src, y), (size_t)W * 4);
    if (r > 0) {
        const int ri = (int)std::ceil(r);
        for (int y = 0; y < H; ++y) {
            const bool top = y < ri, bottom = y >= H - ri;
            if (!top && !bottom) continue;
            uint8_t *row = row_ptr(&body, y);
            for (int x = 0; x < W; ++x) {
                const bool left = x < ri, right = x >= W - ri;
                if (!left && !right) {
                    x = W - ri - 1;
                    continue;
                }
                const float cx = left ? r : (float)W - r;
                const float cy = top ? r : (float)H - r;
                const float px = (float)x + 0.5f, py = (float)y + 0.5f;
                if ((left && px > cx) || (right && px < cx) || (top && py > cy) || (bottom && py < cy)) continue;
                const float d = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
                const float cov = lsk::clampf(r - d + 0.5f, 0.0f, 1.0f);
                if (cov >= 1.0f) continue;
                uint8_t *p = row + x * 4;
                for (int c = 0; c < 4; ++c) p[c] = (uint8_t)((float)p[c] * cov + 0.5f);
            }
        }
    }

    if (sr > 0 && shadow.a > 0) {
        std::vector<uint8_t> plane((size_t)OW * OH, 0);
        for (int y = 0; y < H; ++y) {
            const int ty = y + pad + off;
            if (ty < 0 || ty >= OH) continue;
            const uint8_t *s = row_ptr(&body, y);
            uint8_t *o = &plane[(size_t)ty * OW + pad];
            for (int x = 0; x < W; ++x) o[x] = s[x * 4 + 3];
        }
        box3<1>(plane, OW, OH, sr * 0.5f);
        const uint32_t sa = shadow.a;
        for (int y = 0; y < OH; ++y) {
            uint8_t *o = row_ptr(out, y);
            const uint8_t *pl = &plane[(size_t)y * OW];
            for (int x = 0; x < OW; ++x) {
                const uint32_t a = (pl[x] * sa + 127) / 255;
                if (!a) continue;
                o[x * 4 + 0] = (uint8_t)((shadow.b * a + 127) / 255);
                o[x * 4 + 1] = (uint8_t)((shadow.g * a + 127) / 255);
                o[x * 4 + 2] = (uint8_t)((shadow.r * a + 127) / 255);
                o[x * 4 + 3] = (uint8_t)a;
            }
        }
    }
    ls_image_draw(out, &body, pad, pad);
    ls_image_free(&body);
    return 0;
}

void ls_image_draw(ls_image *dst, const ls_image *src, int32_t x, int32_t y) {
    if (!valid(dst) || !valid(src)) return;
    const int x0 = x > 0 ? x : 0, y0 = y > 0 ? y : 0;
    const int x1 = x + src->width < dst->width ? x + src->width : dst->width;
    const int y1 = y + src->height < dst->height ? y + src->height : dst->height;
    for (int j = y0; j < y1; ++j) {
        const uint8_t *s = row_ptr(src, j - y) + (x0 - x) * 4;
        uint8_t *d = row_ptr(dst, j) + x0 * 4;
        for (int i = x0; i < x1; ++i, s += 4, d += 4) {
            const uint32_t sa = s[3];
            if (sa == 255) {
                std::memcpy(d, s, 4);
            } else if (sa != 0 || s[0] || s[1] || s[2]) {
                const uint32_t ia = 255 - sa;
                for (int c = 0; c < 4; ++c) {
                    uint32_t v = s[c] + (d[c] * ia + 127) / 255;
                    d[c] = (uint8_t)(v > 255 ? 255 : v);
                }
            }
        }
    }
}

}  // extern "C"
