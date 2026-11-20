// GIF 编码：用测试里的最小解码器解回来比对
#include "test.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

struct DecodedFrame {
    std::vector<uint32_t> rgb; // 合成后的整屏 0xRRGGBB
    int delay = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool transparent = false;
};

struct Decoded {
    bool ok = false;
    int width = 0, height = 0;
    int loop = -1;
    std::vector<DecodedFrame> frames;
};

bool lzw_decode(const std::vector<uint8_t> &data, int min_code, size_t npix, std::vector<uint8_t> &out) {
    const int clear = 1 << min_code, eoi = clear + 1;
    int code_size = min_code + 1;
    std::vector<int> prefix(4096), suffix(4096), length(4096);
    for (int i = 0; i < clear; ++i) {
        prefix[i] = -1;
        suffix[i] = i;
        length[i] = 1;
    }
    int next = clear + 2, prev = -1;
    size_t bitpos = 0;
    std::vector<uint8_t> tmp;
    auto read = [&](int n) -> int {
        int v = 0;
        for (int i = 0; i < n; ++i, ++bitpos) {
            if (bitpos / 8 >= data.size()) return -1;
            v |= ((data[bitpos / 8] >> (bitpos % 8)) & 1) << i;
        }
        return v;
    };
    auto emit = [&](int code) {
        tmp.resize((size_t)length[code]);
        int c = code;
        for (int i = length[code] - 1; i >= 0; --i) {
            tmp[(size_t)i] = (uint8_t)suffix[c];
            c = prefix[c];
        }
        out.insert(out.end(), tmp.begin(), tmp.end());
    };
    auto first_of = [&](int code) {
        while (prefix[code] >= 0) code = prefix[code];
        return suffix[code];
    };
    while (true) {
        int code = read(code_size);
        if (code < 0) return false;
        if (code == clear) {
            code_size = min_code + 1;
            next = clear + 2;
            prev = -1;
            continue;
        }
        if (code == eoi) break;
        if (prev < 0) {
            if (code >= clear) return false;
            emit(code);
            prev = code;
            continue;
        }
        if (code < next) {
            emit(code);
            if (next < 4096) {
                prefix[next] = prev;
                suffix[next] = first_of(code);
                length[next] = length[prev] + 1;
                ++next;
            }
        } else if (code == next) {
            const int k = first_of(prev);
            if (next < 4096) {
                prefix[next] = prev;
                suffix[next] = k;
                length[next] = length[prev] + 1;
                ++next;
            }
            emit(code);
        } else {
            return false;
        }
        if (next >= (1 << code_size) && code_size < 12) ++code_size;
        prev = code;
    }
    return out.size() >= npix;
}

std::string temp_dir() {
    for (const char *k : {"TMPDIR", "TEMP", "TMP"})
        if (const char *v = std::getenv(k)) return v;
    return "/tmp";
}

FILE *open_read(const std::string &path) {
#ifdef _WIN32
    // UTF-8 路径转宽字符（与内核写文件的方式一致）
    std::wstring w;
    for (size_t i = 0; i < path.size();) {
        unsigned c = (unsigned char)path[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        unsigned cp = n == 1 ? c : n == 2 ? (c & 31) : n == 3 ? (c & 15) : (c & 7);
        for (int k = 1; k < n && i + k < path.size(); ++k) cp = (cp << 6) | ((unsigned char)path[i + k] & 63);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            w += (wchar_t)(0xD800 + (cp >> 10));
            w += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            w += (wchar_t)cp;
        }
        i += (size_t)n;
    }
    return _wfopen(w.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

Decoded decode_gif(const std::string &path) {
    Decoded d;
    FILE *f = open_read(path);
    if (!f) return d;
    std::vector<uint8_t> b;
    int c;
    while ((c = std::fgetc(f)) != EOF) b.push_back((uint8_t)c);
    std::fclose(f);
    if (b.size() < 13 || std::memcmp(b.data(), "GIF89a", 6) != 0) return d;
    size_t p = 6;
    auto u16 = [&](size_t at) { return (int)b[at] | ((int)b[at + 1] << 8); };
    d.width = u16(p);
    d.height = u16(p + 2);
    const int flags = b[p + 4];
    p += 7;
    if (flags & 0x80) p += 3u * (1u << ((flags & 7) + 1));
    std::vector<uint32_t> screen((size_t)d.width * d.height, 0);
    int delay = 0, tindex = -1;
    while (p < b.size()) {
        const int block = b[p++];
        if (block == 0x3B) {
            d.ok = true;
            break;
        }
        if (block == 0x21) {
            const int label = b[p++];
            if (label == 0xF9) {
                const int fl = b[p + 1];
                delay = u16(p + 2);
                tindex = (fl & 1) ? b[p + 4] : -1;
                if (((fl >> 2) & 7) != 1) return d; // 处置方式必须是 1
            } else if (label == 0xFF && b[p] == 11 && std::memcmp(&b[p + 1], "NETSCAPE2.0", 11) == 0) {
                d.loop = u16(p + 12 + 2);
            }
            while (b[p]) p += b[p] + 1u;
            ++p;
            continue;
        }
        if (block != 0x2C) return d;
        DecodedFrame fr;
        fr.x = u16(p);
        fr.y = u16(p + 2);
        fr.w = u16(p + 4);
        fr.h = u16(p + 6);
        const int ifl = b[p + 8];
        p += 9;
        std::vector<uint32_t> pal;
        if (ifl & 0x80) {
            const int n = 1 << ((ifl & 7) + 1);
            for (int i = 0; i < n; ++i, p += 3) pal.push_back(((uint32_t)b[p] << 16) | ((uint32_t)b[p + 1] << 8) | b[p + 2]);
        } else {
            return d; // 内核总是写局部调色板
        }
        const int min_code = b[p++];
        std::vector<uint8_t> data;
        while (b[p]) {
            data.insert(data.end(), b.begin() + (std::ptrdiff_t)p + 1, b.begin() + (std::ptrdiff_t)(p + 1 + b[p]));
            p += b[p] + 1u;
        }
        ++p;
        std::vector<uint8_t> idx;
        if (!lzw_decode(data, min_code, (size_t)fr.w * fr.h, idx)) return d;
        for (int y = 0; y < fr.h; ++y)
            for (int x = 0; x < fr.w; ++x) {
                const int i = idx[(size_t)y * fr.w + x];
                if (i == tindex) continue;
                if (i >= (int)pal.size()) return d;
                screen[(size_t)(fr.y + y) * d.width + fr.x + x] = pal[(size_t)i];
            }
        fr.rgb = screen;
        fr.delay = delay;
        fr.transparent = tindex >= 0;
        d.frames.push_back(fr);
        tindex = -1;
    }
    return d;
}

ls_image make_frame(int w, int h, uint32_t (*fn)(int, int, int), int arg) {
    ls_image im = ls_image_alloc(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t c = fn(x, y, arg);
            uint8_t *p = im.data + (size_t)y * im.stride + x * 4;
            p[0] = (uint8_t)c;
            p[1] = (uint8_t)(c >> 8);
            p[2] = (uint8_t)(c >> 16);
            p[3] = 255;
        }
    return im;
}

uint32_t blocks(int x, int y, int k) {
    // 有限的几种颜色（UI 截图那种）
    static const uint32_t pal[6] = {0xFFFFFF, 0x2F6BFF, 0x1D1D1F, 0xE5484D, 0x30A46C, 0xF5F5F7};
    if (k && x >= 20 && x < 30 && y >= 10 && y < 18) return 0xFF9500; // 第二帧局部变化
    return pal[((x / 8) + (y / 6)) % 6];
}

uint32_t gradient(int x, int y, int) {
    return ((uint32_t)(x * 4 & 255) << 16) | ((uint32_t)(y * 5 & 255) << 8) | (uint32_t)((x + y) * 2 & 255);
}

uint32_t rgb_of(const ls_image &im, int x, int y) {
    const uint8_t *p = im.data + (size_t)y * im.stride + x * 4;
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

}  // namespace

TEST(gif_encode_decode_roundtrip) {
    // 输出放在系统临时目录，避免写进仓库；文件名带中文，验证 UTF-8 路径
    const std::string out = temp_dir() + "/lemonshot-测试-动画.gif";
    const int W = 64, H = 48;
    ls_gif *g = ls_gif_create(out.c_str(), W, H, 0);
    CHECK(g != nullptr);
    if (!g) return;
    ls_image f1 = make_frame(W, H, blocks, 0);
    ls_image f2 = make_frame(W, H, blocks, 1);
    ls_image f3 = make_frame(W, H, gradient, 0);
    CHECK_EQ(ls_gif_add(g, &f1, 10), 0);
    CHECK_EQ(ls_gif_add(g, &f2, 20), 0);
    CHECK_EQ(ls_gif_add(g, &f2, 30), 0); // 与上一帧相同：时长并入上一帧
    CHECK_EQ(ls_gif_add(g, &f3, 10), 0);
    CHECK_EQ(ls_gif_add(g, &f1, 10), 0);
    ls_image wrong = ls_image_alloc(10, 10);
    CHECK_EQ(ls_gif_add(g, &wrong, 10), -1);
    CHECK_EQ(ls_gif_finish(g), 0);
    ls_gif_destroy(g);

    Decoded d = decode_gif(out);
    CHECK(d.ok);
    CHECK_EQ(d.width, W);
    CHECK_EQ(d.height, H);
    CHECK_EQ(d.loop, 0);
    CHECK_EQ(d.frames.size(), 4);
    if (d.frames.size() == 4) {
        CHECK_EQ(d.frames[0].delay, 10);
        CHECK_EQ(d.frames[1].delay, 50);
        CHECK_EQ(d.frames[2].delay, 10);
        // 第一帧整帧无透明
        CHECK_EQ(d.frames[0].w, W);
        CHECK(!d.frames[0].transparent);
        // 第二帧只编码变化的外接框
        CHECK_EQ(d.frames[1].x, 20);
        CHECK_EQ(d.frames[1].y, 10);
        CHECK_EQ(d.frames[1].w, 10);
        CHECK_EQ(d.frames[1].h, 8);
        // 颜色少于 256 种：无损
        bool exact = true;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                exact = exact && d.frames[0].rgb[(size_t)y * W + x] == rgb_of(f1, x, y);
                exact = exact && d.frames[1].rgb[(size_t)y * W + x] == rgb_of(f2, x, y);
                exact = exact && d.frames[3].rgb[(size_t)y * W + x] == rgb_of(f1, x, y);
            }
        CHECK(exact);
        // 渐变超过 256 色：量化误差在容许范围
        double sum = 0;
        int maxe = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const uint32_t a = d.frames[2].rgb[(size_t)y * W + x], e = rgb_of(f3, x, y);
                for (int s = 0; s < 24; s += 8) {
                    const int diff = std::abs((int)((a >> s) & 255) - (int)((e >> s) & 255));
                    sum += diff;
                    if (diff > maxe) maxe = diff;
                }
            }
        const double mean = sum / (W * H * 3);
        std::printf("    渐变帧量化：平均误差 %.2f，最大误差 %d\n", mean, maxe);
        CHECK(mean < 4.0);
        CHECK(maxe <= 24);
    }
    std::remove(out.c_str());
    ls_image_free(&f1);
    ls_image_free(&f2);
    ls_image_free(&f3);
    ls_image_free(&wrong);
}

TEST(gif_large_frame_lzw_reset) {
    // 大帧、颜色多：LZW 码表多次写满重置
    const std::string out = temp_dir() + "/lemonshot-large.gif";
    const int W = 300, H = 200;
    ls_gif *g = ls_gif_create(out.c_str(), W, H, 3);
    CHECK(g != nullptr);
    if (!g) return;
    ls_image f = ls_image_alloc(W, H);
    uint32_t seed = 12345;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            seed = seed * 1664525u + 1013904223u;
            uint8_t *p = f.data + (size_t)y * f.stride + x * 4;
            const int v = (int)(seed >> 24) & 0x7F;
            p[0] = (uint8_t)(x * 255 / W);
            p[1] = (uint8_t)v;
            p[2] = (uint8_t)(y * 255 / H);
            p[3] = 255;
        }
    CHECK_EQ(ls_gif_add(g, &f, 7), 0);
    CHECK_EQ(ls_gif_finish(g), 0);
    CHECK_EQ(ls_gif_finish(g), -1);
    ls_gif_destroy(g);
    Decoded d = decode_gif(out);
    CHECK(d.ok);
    CHECK_EQ(d.loop, 3);
    CHECK_EQ(d.frames.size(), 1);
    if (!d.frames.empty()) {
        double sum = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const uint32_t a = d.frames[0].rgb[(size_t)y * W + x], e = rgb_of(f, x, y);
                for (int s = 0; s < 24; s += 8) sum += std::abs((int)((a >> s) & 255) - (int)((e >> s) & 255));
            }
        const double mean = sum / (W * H * 3);
        std::printf("    随机帧量化：平均误差 %.2f\n", mean);
        CHECK(mean < 12.0);
    }
    std::remove(out.c_str());
    ls_image_free(&f);
    CHECK(ls_gif_create("/nonexistent-dir/x.gif", 10, 10, 0) == nullptr);
}
