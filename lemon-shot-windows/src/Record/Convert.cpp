#include "Record/Convert.h"

#include <cmath>
#include <cstring>

namespace ls {
namespace rec {

namespace {

const int kWeightBits = 14;
const int kOne = 1 << kWeightBits;

struct SpanBuilder {
    static void Build(int s, int d, std::vector<int> &starts, std::vector<std::vector<uint16_t>> &weights) {
        starts.assign((size_t)d, 0);
        weights.assign((size_t)d, std::vector<uint16_t>());
        double scale = (double)s / (double)d;
        for (int i = 0; i < d; ++i) {
            double a = i * scale, b = (i + 1) * scale;
            if (b > s) b = s;
            int i0 = (int)std::floor(a), i1 = (int)std::ceil(b);
            if (i1 > s) i1 = s;
            if (i1 <= i0) i1 = i0 + 1;
            if (i1 > s) {
                i1 = s;
                i0 = s - 1;
            }
            starts[(size_t)i] = i0;
            std::vector<uint16_t> &w = weights[(size_t)i];
            int sum = 0, best = 0;
            for (int k = i0; k < i1; ++k) {
                double lo = a > k ? a : (double)k, hi = b < k + 1 ? b : (double)(k + 1);
                double ov = hi - lo;
                if (ov < 0) ov = 0;
                int v = (int)std::lround(ov / scale * kOne);
                w.push_back((uint16_t)v);
                sum += v;
                if (v > w[(size_t)best]) best = (int)w.size() - 1;
            }
            // 权重和修正为精确的 1 << 14
            int fixed = (int)w[(size_t)best] + (kOne - sum);
            w[(size_t)best] = (uint16_t)(fixed < 0 ? 0 : fixed);
        }
    }
};

}  // namespace

void Scaler::Setup(int sw, int sh, int dw, int dh) {
    sw_ = sw;
    sh_ = sh;
    dw_ = dw;
    dh_ = dh;
    xs_.clear();
    ys_.clear();
    if (dw <= sw && dh <= sh && sw - dw <= 1 && sh - dh <= 1) {
        mode_ = 0;
        return;
    }
    if (sw == dw * 2 && sh == dh * 2) {
        mode_ = 1;
        return;
    }
    mode_ = 2;
    std::vector<int> starts;
    std::vector<std::vector<uint16_t>> weights;
    SpanBuilder::Build(sw, dw, starts, weights);
    xs_.resize((size_t)dw);
    for (int i = 0; i < dw; ++i) {
        xs_[(size_t)i].start = starts[(size_t)i];
        xs_[(size_t)i].w.swap(weights[(size_t)i]);
    }
    SpanBuilder::Build(sh, dh, starts, weights);
    ys_.resize((size_t)dh);
    for (int i = 0; i < dh; ++i) {
        ys_[(size_t)i].start = starts[(size_t)i];
        ys_[(size_t)i].w.swap(weights[(size_t)i]);
    }
    rowA_.assign((size_t)dw * 4, 0);
    rowB_.assign((size_t)dw * 4, 0);
    acc_.assign((size_t)dw * 4, 0);
}

// 一行横向面积平均：结果保留 8 位小数（0..65280）
void Scaler::HRow(const uint8_t *row, uint16_t *out) {
    for (int x = 0; x < dw_; ++x) {
        const Span &sp = xs_[(size_t)x];
        uint32_t b = 0, g = 0, r = 0;
        const uint8_t *p = row + (size_t)sp.start * 4;
        size_t n = sp.w.size();
        const uint16_t *w = sp.w.data();
        for (size_t k = 0; k < n; ++k, p += 4) {
            uint32_t wk = w[k];
            b += p[0] * wk;
            g += p[1] * wk;
            r += p[2] * wk;
        }
        out[x * 4 + 0] = (uint16_t)((b + 32) >> 6);
        out[x * 4 + 1] = (uint16_t)((g + 32) >> 6);
        out[x * 4 + 2] = (uint16_t)((r + 32) >> 6);
        out[x * 4 + 3] = 0;
    }
}

void Scaler::Run(const uint8_t *src, int sstride, uint8_t *dst, int dstride) {
    if (mode_ == 0) {
        for (int y = 0; y < dh_; ++y) {
            uint8_t *d = dst + (size_t)y * dstride;
            std::memcpy(d, src + (size_t)y * sstride, (size_t)dw_ * 4);
            for (int x = 0; x < dw_; ++x) d[x * 4 + 3] = 255;
        }
        return;
    }
    if (mode_ == 1) {
        for (int y = 0; y < dh_; ++y) {
            const uint8_t *s0 = src + (size_t)(y * 2) * sstride;
            const uint8_t *s1 = s0 + sstride;
            uint8_t *d = dst + (size_t)y * dstride;
            for (int x = 0; x < dw_; ++x, s0 += 8, s1 += 8, d += 4) {
                d[0] = (uint8_t)((s0[0] + s0[4] + s1[0] + s1[4] + 2) >> 2);
                d[1] = (uint8_t)((s0[1] + s0[5] + s1[1] + s1[5] + 2) >> 2);
                d[2] = (uint8_t)((s0[2] + s0[6] + s1[2] + s1[6] + 2) >> 2);
                d[3] = 255;
            }
        }
        return;
    }
    int cached = -1;
    size_t n4 = (size_t)dw_ * 4;
    for (int y = 0; y < dh_; ++y) {
        std::memset(acc_.data(), 0, n4 * sizeof(uint32_t));
        const Span &sy = ys_[(size_t)y];
        size_t rows = sy.w.size();
        for (size_t k = 0; k < rows; ++k) {
            int r = sy.start + (int)k;
            uint32_t wy = sy.w[k];
            const uint16_t *h;
            if (r == cached) {
                h = rowA_.data();
            } else {
                HRow(src + (size_t)r * sstride, rowB_.data());
                if (k + 1 == rows) {
                    // 最后一行可能被下一个输出行再用：放进缓存
                    rowA_.swap(rowB_);
                    cached = r;
                    h = rowA_.data();
                } else {
                    h = rowB_.data();
                }
            }
            if (wy == 0) continue;
            uint32_t *a = acc_.data();
            for (size_t i = 0; i < n4; ++i) a[i] += h[i] * wy;
        }
        uint8_t *d = dst + (size_t)y * dstride;
        const uint32_t *a = acc_.data();
        for (int x = 0; x < dw_; ++x) {
            for (int c = 0; c < 3; ++c) {
                uint32_t v = (a[x * 4 + c] + (1u << 21)) >> 22;
                d[x * 4 + c] = (uint8_t)(v > 255 ? 255 : v);
            }
            d[x * 4 + 3] = 255;
        }
    }
}

void BgraToNv12(const uint8_t *src, int stride, int w, int h, uint8_t *yp, int ystride, uint8_t *uvp, int uvstride,
                bool bt709) {
    // 整数系数（×256）：BT.709 / BT.601，有限范围
    const int yr = bt709 ? 47 : 66, yg = bt709 ? 157 : 129, yb = bt709 ? 16 : 25;
    const int ur = bt709 ? -26 : -38, ug = bt709 ? -87 : -74, ub = 112;
    const int vr = 112, vg = bt709 ? -102 : -94, vb = bt709 ? -10 : -18;
    for (int y = 0; y < h; y += 2) {
        const uint8_t *s0 = src + (size_t)y * stride;
        const uint8_t *s1 = s0 + stride;
        uint8_t *y0 = yp + (size_t)y * ystride;
        uint8_t *y1 = y0 + ystride;
        uint8_t *uv = uvp + (size_t)(y / 2) * uvstride;
        for (int x = 0; x < w; x += 2, s0 += 8, s1 += 8, y0 += 2, y1 += 2, uv += 2) {
            int b00 = s0[0], g00 = s0[1], r00 = s0[2];
            int b01 = s0[4], g01 = s0[5], r01 = s0[6];
            int b10 = s1[0], g10 = s1[1], r10 = s1[2];
            int b11 = s1[4], g11 = s1[5], r11 = s1[6];
            y0[0] = (uint8_t)(((yr * r00 + yg * g00 + yb * b00 + 128) >> 8) + 16);
            y0[1] = (uint8_t)(((yr * r01 + yg * g01 + yb * b01 + 128) >> 8) + 16);
            y1[0] = (uint8_t)(((yr * r10 + yg * g10 + yb * b10 + 128) >> 8) + 16);
            y1[1] = (uint8_t)(((yr * r11 + yg * g11 + yb * b11 + 128) >> 8) + 16);
            int r = (r00 + r01 + r10 + r11 + 2) >> 2;
            int g = (g00 + g01 + g10 + g11 + 2) >> 2;
            int b = (b00 + b01 + b10 + b11 + 2) >> 2;
            // 加 128 << 8 的偏置保证右移前为非负
            uv[0] = (uint8_t)((ur * r + ug * g + ub * b + 128 + (128 << 8)) >> 8);
            uv[1] = (uint8_t)((vr * r + vg * g + vb * b + 128 + (128 << 8)) >> 8);
        }
    }
}

}  // namespace rec
}  // namespace ls
