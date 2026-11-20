// GIF89a 编码：每帧局部调色板（颜色不超过 256 种时无损，否则中位切分量化，不抖动），
// 与上一帧相同的像素用透明色并把帧裁剪到变化的外接框（处置方式 1），NETSCAPE2.0 循环，标准 LZW。
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using lsk::row_ptr;

namespace {

FILE *open_utf8(const char *path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::vector<wchar_t> w((size_t)n);
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w.data(), n) <= 0) return nullptr;
    return _wfopen(w.data(), L"wb");
#else
    return std::fopen(path, "wb");
#endif
}

// LZW 字典：开放寻址散列，键 = (前缀码 << 8) | 字节；用代数标记清空，避免每次重置都清整张表。
struct LzwDict {
    enum { kBits = 14, kSize = 1 << kBits };
    std::vector<uint32_t> keys, gens;
    std::vector<uint16_t> codes;
    uint32_t gen = 1;
    LzwDict() : keys(kSize), gens(kSize, 0), codes(kSize) {}
    void reset() { ++gen; }
    static uint32_t slot(uint32_t key) { return (key * 2654435761u) >> (32 - kBits); }
    int find(uint32_t key) const {
        for (uint32_t h = slot(key);; h = (h + 1) & (kSize - 1)) {
            if (gens[h] != gen) return -1;
            if (keys[h] == key) return codes[h];
        }
    }
    void insert(uint32_t key, int code) {
        uint32_t h = slot(key);
        while (gens[h] == gen) h = (h + 1) & (kSize - 1);
        gens[h] = gen;
        keys[h] = key;
        codes[h] = (uint16_t)code;
    }
};

struct BitSink {
    std::vector<uint8_t> &out;
    uint32_t acc = 0;
    int nbits = 0;
    explicit BitSink(std::vector<uint8_t> &o) : out(o) {}
    void put(int code, int size) {
        acc |= (uint32_t)code << nbits;
        nbits += size;
        while (nbits >= 8) {
            out.push_back((uint8_t)(acc & 255));
            acc >>= 8;
            nbits -= 8;
        }
    }
    void flush() {
        if (nbits > 0) out.push_back((uint8_t)(acc & 255));
        acc = 0;
        nbits = 0;
    }
};

// 标准 GIF LZW（码长随字典增长到 12 位，满了发清除码）
void lzw_encode(const uint8_t *idx, size_t n, int min_code, LzwDict &dict, std::vector<uint8_t> &raw) {
    const int clear = 1 << min_code;
    const int eoi = clear + 1;
    int code_size = min_code + 1;
    int max_code = eoi; // 最后一个已分配的码
    BitSink bits(raw);
    dict.reset();
    bits.put(clear, code_size);
    if (n == 0) {
        bits.put(eoi, code_size);
        bits.flush();
        return;
    }
    int cur = idx[0];
    for (size_t i = 1; i < n; ++i) {
        const int c = idx[i];
        const uint32_t key = ((uint32_t)cur << 8) | (uint32_t)c;
        const int found = dict.find(key);
        if (found >= 0) {
            cur = found;
            continue;
        }
        bits.put(cur, code_size);
        ++max_code;
        dict.insert(key, max_code);
        if (max_code >= (1 << code_size) && code_size < 12) ++code_size;
        if (max_code >= 4095) {
            bits.put(clear, code_size);
            dict.reset();
            code_size = min_code + 1;
            max_code = eoi;
        }
        cur = c;
    }
    bits.put(cur, code_size);
    bits.put(eoi, code_size);
    bits.flush();
}

// 精确颜色表（≤ 256 色时无损）
struct ExactSet {
    enum { kSize = 1024 };
    uint32_t keys[kSize];
    uint8_t vals[kSize];
    int count = 0;
    void clear() {
        std::memset(keys, 0, sizeof keys);
        count = 0;
    }
    // 返回下标；超出 limit 返回 -1
    int add(uint32_t rgb, int limit) {
        const uint32_t key = rgb | 0x1000000u;
        uint32_t h = (rgb * 2654435761u) >> 22;
        while (keys[h]) {
            if (keys[h] == key) return vals[h];
            h = (h + 1) & (kSize - 1);
        }
        if (count >= limit) return -1;
        keys[h] = key;
        vals[h] = (uint8_t)count;
        return count++;
    }
    int get(uint32_t rgb) const {
        const uint32_t key = rgb | 0x1000000u;
        uint32_t h = (rgb * 2654435761u) >> 22;
        while (keys[h]) {
            if (keys[h] == key) return vals[h];
            h = (h + 1) & (kSize - 1);
        }
        return -1;
    }
};

struct HistEntry {
    uint32_t bin;
    uint32_t count;
    uint32_t sum[3];
    uint8_t c[3]; // 该格的平均色 r g b
};

}  // namespace

struct ls_gif {
    FILE *f = nullptr;
    int w = 0, h = 0;
    bool ok = true;
    bool finished = false;
    bool have_prev = false;
    std::vector<uint32_t> prev, cur; // 0xRRGGBB
    // 待写出的一帧（等下一帧来时才知道最终停留时长）
    bool has_pending = false;
    int pending_delay = 0;
    int pending_transparent = -1;
    std::vector<uint8_t> pending;
    // 临时
    std::vector<uint8_t> changed, indices, raw;
    ExactSet exact;
    std::vector<uint32_t> hist_count, hist_sum; // 2^18 格
    std::vector<uint32_t> touched;
    std::vector<int16_t> lut;
    LzwDict dict;

    void write(const void *p, size_t n) {
        if (!f || !ok) return;
        if (std::fwrite(p, 1, n, f) != n) ok = false;
    }
    void byte(int b) {
        uint8_t v = (uint8_t)b;
        write(&v, 1);
    }
    void word(int v) {
        byte(v & 255);
        byte((v >> 8) & 255);
    }
    void flush_pending();
    int quantize(int bx, int by, int bw, int bh, bool transparent, uint8_t palette[256][3]);
};

void ls_gif::flush_pending() {
    if (!has_pending) return;
    int d = pending_delay;
    if (d < 0) d = 0;
    if (d > 65535) d = 65535;
    const uint8_t gce[8] = {0x21, 0xF9, 0x04, (uint8_t)(0x04 | (pending_transparent >= 0 ? 1 : 0)), (uint8_t)(d & 255),
                            (uint8_t)(d >> 8), (uint8_t)(pending_transparent >= 0 ? pending_transparent : 0), 0};
    write(gce, sizeof gce);
    write(pending.data(), pending.size());
    has_pending = false;
}

// 为外接框里的变化像素生成调色板，并把下标写进 indices。返回调色板颜色数（不含透明色）。
int ls_gif::quantize(int bx, int by, int bw, int bh, bool transparent, uint8_t palette[256][3]) {
    const int limit = transparent ? 255 : 256;
    // 透明色排在最后一个颜色之后（下标 = 颜色数）
    indices.resize((size_t)bw * bh);
    // 先试精确颜色
    exact.clear();
    bool fits = true;
    for (int y = 0; y < bh && fits; ++y) {
        const uint32_t *row = &cur[(size_t)(by + y) * w + bx];
        const uint8_t *ch = &changed[(size_t)y * bw];
        for (int x = 0; x < bw; ++x) {
            if (!ch[x]) continue;
            if (exact.add(row[x], limit) < 0) {
                fits = false;
                break;
            }
        }
    }
    if (fits) {
        for (int i = 0; i < ExactSet::kSize; ++i) {
            if (!exact.keys[i]) continue;
            const uint32_t rgb = exact.keys[i] & 0xFFFFFFu;
            uint8_t *p = palette[exact.vals[i]];
            p[0] = (uint8_t)(rgb >> 16);
            p[1] = (uint8_t)(rgb >> 8);
            p[2] = (uint8_t)rgb;
        }
        const int tindex = exact.count;
        for (int y = 0; y < bh; ++y) {
            const uint32_t *row = &cur[(size_t)(by + y) * w + bx];
            const uint8_t *ch = &changed[(size_t)y * bw];
            uint8_t *o = &indices[(size_t)y * bw];
            for (int x = 0; x < bw; ++x) o[x] = ch[x] ? (uint8_t)exact.get(row[x]) : (uint8_t)tindex;
        }
        return exact.count;
    }

    // 中位切分：先按每通道 6 位分格统计（保留各格的精确色和），再切分
    const size_t nbins = (size_t)1 << 18;
    if (hist_count.size() != nbins) {
        hist_count.assign(nbins, 0);
        hist_sum.assign(nbins * 3, 0);
        lut.assign(nbins, -1);
    }
    touched.clear();
    for (int y = 0; y < bh; ++y) {
        const uint32_t *row = &cur[(size_t)(by + y) * w + bx];
        const uint8_t *ch = &changed[(size_t)y * bw];
        for (int x = 0; x < bw; ++x) {
            if (!ch[x]) continue;
            const uint32_t c = row[x];
            const uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            const uint32_t bin = ((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2);
            if (!hist_count[bin]++) touched.push_back(bin);
            hist_sum[bin * 3] += r;
            hist_sum[bin * 3 + 1] += g;
            hist_sum[bin * 3 + 2] += b;
        }
    }
    std::vector<HistEntry> ents(touched.size());
    for (size_t i = 0; i < touched.size(); ++i) {
        const uint32_t bin = touched[i];
        HistEntry &e = ents[i];
        e.bin = bin;
        e.count = hist_count[bin];
        for (int k = 0; k < 3; ++k) {
            e.sum[k] = hist_sum[bin * 3 + k];
            e.c[k] = (uint8_t)((e.sum[k] + e.count / 2) / e.count);
        }
    }
    struct Box {
        size_t begin, end;
        uint64_t count;
        int lo[3], hi[3];
    };
    auto measure = [&](Box &bx2) {
        bx2.count = 0;
        for (int k = 0; k < 3; ++k) {
            bx2.lo[k] = 255;
            bx2.hi[k] = 0;
        }
        for (size_t i = bx2.begin; i < bx2.end; ++i) {
            bx2.count += ents[i].count;
            for (int k = 0; k < 3; ++k) {
                bx2.lo[k] = std::min(bx2.lo[k], (int)ents[i].c[k]);
                bx2.hi[k] = std::max(bx2.hi[k], (int)ents[i].c[k]);
            }
        }
    };
    std::vector<Box> boxes;
    boxes.reserve((size_t)limit);
    Box all{0, ents.size(), 0, {0, 0, 0}, {0, 0, 0}};
    measure(all);
    boxes.push_back(all);
    while ((int)boxes.size() < limit) {
        // 选「范围平方 × 像素数」最大的可分盒子
        int pick = -1;
        double best = -1;
        for (size_t i = 0; i < boxes.size(); ++i) {
            const Box &b = boxes[i];
            if (b.end - b.begin < 2) continue;
            int range = 0;
            for (int k = 0; k < 3; ++k) range = std::max(range, b.hi[k] - b.lo[k]);
            const double score = (double)range * range * (double)b.count;
            if (score > best) {
                best = score;
                pick = (int)i;
            }
        }
        if (pick < 0 || best <= 0) break;
        Box b = boxes[(size_t)pick];
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (b.hi[k] - b.lo[k] > b.hi[axis] - b.lo[axis]) axis = k;
        std::sort(ents.begin() + (std::ptrdiff_t)b.begin, ents.begin() + (std::ptrdiff_t)b.end,
                  [axis](const HistEntry &a, const HistEntry &c) { return a.c[axis] < c.c[axis]; });
        uint64_t half = b.count / 2, acc = 0;
        size_t split = b.begin + 1;
        for (size_t i = b.begin; i < b.end - 1; ++i) {
            acc += ents[i].count;
            split = i + 1;
            if (acc >= half) break;
        }
        Box l{b.begin, split, 0, {0, 0, 0}, {0, 0, 0}}, r{split, b.end, 0, {0, 0, 0}, {0, 0, 0}};
        measure(l);
        measure(r);
        boxes[(size_t)pick] = l;
        boxes.push_back(r);
    }
    const int ncolors = (int)boxes.size();
    for (int i = 0; i < ncolors; ++i) {
        uint64_t s[3] = {0, 0, 0}, n = 0;
        for (size_t k = boxes[(size_t)i].begin; k < boxes[(size_t)i].end; ++k) {
            n += ents[k].count;
            for (int c = 0; c < 3; ++c) s[c] += ents[k].sum[c];
        }
        for (int c = 0; c < 3; ++c) palette[i][c] = (uint8_t)(n ? (s[c] + n / 2) / n : 0);
    }
    // 每格映射到最近的调色板颜色（用该格的平均色比较）
    for (const HistEntry &e : ents) {
        int best = 0, bestd = 1 << 30;
        for (int i = 0; i < ncolors; ++i) {
            const int dr = (int)e.c[0] - palette[i][0], dg = (int)e.c[1] - palette[i][1], db = (int)e.c[2] - palette[i][2];
            const int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (d < bestd) {
                bestd = d;
                best = i;
            }
        }
        lut[e.bin] = (int16_t)best;
    }
    const int tindex = ncolors;
    for (int y = 0; y < bh; ++y) {
        const uint32_t *row = &cur[(size_t)(by + y) * w + bx];
        const uint8_t *ch = &changed[(size_t)y * bw];
        uint8_t *o = &indices[(size_t)y * bw];
        for (int x = 0; x < bw; ++x) {
            if (!ch[x]) {
                o[x] = (uint8_t)tindex;
                continue;
            }
            const uint32_t c = row[x];
            const uint32_t bin = (((c >> 16) & 255) >> 2 << 12) | (((c >> 8) & 255) >> 2 << 6) | ((c & 255) >> 2);
            o[x] = (uint8_t)lut[bin];
        }
    }
    for (uint32_t bin : touched) {
        hist_count[bin] = 0;
        hist_sum[bin * 3] = hist_sum[bin * 3 + 1] = hist_sum[bin * 3 + 2] = 0;
        lut[bin] = -1;
    }
    return ncolors;
}

extern "C" {

ls_gif *ls_gif_create(const char *path, int32_t width, int32_t height, int32_t loop) {
    if (!path || width <= 0 || height <= 0 || width > 65535 || height > 65535) return nullptr;
    FILE *f = open_utf8(path);
    if (!f) return nullptr;
    ls_gif *g = new ls_gif();
    g->f = f;
    g->w = width;
    g->h = height;
    g->write("GIF89a", 6);
    g->word(width);
    g->word(height);
    g->byte(0x70); // 无全局调色板，色彩分辨率 8 位
    g->byte(0);
    g->byte(0);
    // NETSCAPE2.0 循环扩展
    static const uint8_t app[3 + 11] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'};
    g->write(app, sizeof app);
    g->byte(3);
    g->byte(1);
    int l = loop < 0 ? 0 : (loop > 65535 ? 65535 : loop);
    g->word(l);
    g->byte(0);
    return g;
}

int32_t ls_gif_add(ls_gif *g, const ls_image *frame, int32_t delay_cs) {
    if (!g || g->finished || !g->ok || !frame || !frame->data || frame->width != g->w || frame->height != g->h) return -1;
    const int W = g->w, H = g->h;
    const size_t n = (size_t)W * H;
    g->cur.resize(n);
    // 去预乘，透明度忽略（录屏帧都是不透明的）
    for (int y = 0; y < H; ++y) {
        const uint8_t *s = row_ptr(frame, y);
        uint32_t *o = &g->cur[(size_t)y * W];
        for (int x = 0; x < W; ++x, s += 4) {
            uint32_t b = s[0], gg = s[1], r = s[2];
            const uint32_t a = s[3];
            if (a == 0) {
                r = gg = b = 0;
            } else if (a < 255) {
                r = std::min(255u, (r * 255 + a / 2) / a);
                gg = std::min(255u, (gg * 255 + a / 2) / a);
                b = std::min(255u, (b * 255 + a / 2) / a);
            }
            o[x] = (r << 16) | (gg << 8) | b;
        }
    }
    int bx0 = 0, by0 = 0, bx1 = W - 1, by1 = H - 1;
    if (g->have_prev) {
        bx0 = W;
        bx1 = -1;
        by0 = H;
        by1 = -1;
        for (int y = 0; y < H; ++y) {
            const uint32_t *a = &g->cur[(size_t)y * W];
            const uint32_t *b = &g->prev[(size_t)y * W];
            if (std::memcmp(a, b, (size_t)W * 4) == 0) continue;
            int x0 = 0, x1 = W - 1;
            while (a[x0] == b[x0]) ++x0;
            while (a[x1] == b[x1]) --x1;
            bx0 = std::min(bx0, x0);
            bx1 = std::max(bx1, x1);
            if (by0 == H) by0 = y;
            by1 = y;
        }
        if (bx1 < 0) {
            // 与上一帧完全相同：把时长并入上一帧
            g->pending_delay += delay_cs > 0 ? delay_cs : 0;
            return 0;
        }
    }
    g->flush_pending();
    const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    g->changed.assign((size_t)bw * bh, 1);
    bool transparent = false;
    if (g->have_prev) {
        for (int y = 0; y < bh; ++y) {
            const uint32_t *a = &g->cur[(size_t)(by0 + y) * W + bx0];
            const uint32_t *b = &g->prev[(size_t)(by0 + y) * W + bx0];
            uint8_t *ch = &g->changed[(size_t)y * bw];
            for (int x = 0; x < bw; ++x) {
                if (a[x] == b[x]) {
                    ch[x] = 0;
                    transparent = true;
                }
            }
        }
    }
    uint8_t palette[256][3];
    std::memset(palette, 0, sizeof palette);
    const int ncolors = g->quantize(bx0, by0, bw, bh, transparent, palette);
    const int used = ncolors + (transparent ? 1 : 0);
    int bits = 1;
    while ((1 << bits) < used) ++bits;
    const int min_code = bits < 2 ? 2 : bits;

    std::vector<uint8_t> &p = g->pending;
    p.clear();
    p.push_back(0x2C);
    auto push_word = [&p](int v) {
        p.push_back((uint8_t)(v & 255));
        p.push_back((uint8_t)((v >> 8) & 255));
    };
    push_word(bx0);
    push_word(by0);
    push_word(bw);
    push_word(bh);
    p.push_back((uint8_t)(0x80 | (bits - 1)));
    for (int i = 0; i < (1 << bits); ++i) {
        p.push_back(palette[i][0]);
        p.push_back(palette[i][1]);
        p.push_back(palette[i][2]);
    }
    p.push_back((uint8_t)min_code);
    g->raw.clear();
    lzw_encode(g->indices.data(), g->indices.size(), min_code, g->dict, g->raw);
    for (size_t i = 0; i < g->raw.size(); i += 255) {
        const size_t len = std::min<size_t>(255, g->raw.size() - i);
        p.push_back((uint8_t)len);
        p.insert(p.end(), g->raw.begin() + (std::ptrdiff_t)i, g->raw.begin() + (std::ptrdiff_t)(i + len));
    }
    p.push_back(0);
    g->has_pending = true;
    g->pending_delay = delay_cs > 0 ? delay_cs : 0;
    g->pending_transparent = transparent ? ncolors : -1;
    std::swap(g->prev, g->cur);
    g->have_prev = true;
    return g->ok ? 0 : -1;
}

int32_t ls_gif_finish(ls_gif *g) {
    if (!g || g->finished) return -1;
    g->flush_pending();
    g->byte(0x3B);
    g->finished = true;
    if (g->f) {
        if (std::fflush(g->f) != 0) g->ok = false;
        if (std::fclose(g->f) != 0) g->ok = false;
        g->f = nullptr;
    }
    return g->ok ? 0 : -1;
}

void ls_gif_destroy(ls_gif *g) {
    if (!g) return;
    if (g->f) std::fclose(g->f);
    delete g;
}

}  // extern "C"
