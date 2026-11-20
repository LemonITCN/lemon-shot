// 长截图拼接：用程序生成的长文档滑动截帧
#include "test.h"

#include <chrono>
#include <cstdlib>
#include <vector>

namespace {

struct Rng {
    uint32_t s;
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    int range(int n) { return (int)(next() % (uint32_t)n); }
};

// 像网页的长文档：白底，一行行「文字」（随机深色小块），行间空白，偶尔有色块
struct Doc {
    int W = 0, H = 0;
    std::vector<uint32_t> px; // 0xAARRGGBB
    uint32_t at(int x, int y) const { return px[(size_t)y * W + x]; }
};

Doc make_doc(int W, int H, uint32_t seed) {
    Doc d;
    d.W = W;
    d.H = H;
    d.px.assign((size_t)W * H, 0xFFFFFFFFu);
    Rng r{seed};
    int y = 0;
    while (y < H) {
        const int kind = r.range(10);
        if (kind == 0) {
            // 色块
            const int h = 20 + r.range(60);
            const uint32_t c = 0xFF000000u | (r.next() & 0xFFFFFF);
            const int x0 = r.range(W / 3), x1 = W - r.range(W / 3);
            for (int yy = y; yy < y + h && yy < H; ++yy)
                for (int x = x0; x < x1; ++x) d.px[(size_t)yy * W + x] = c;
            y += h + 10;
        } else {
            // 一行文字：高 16，字宽 6~9
            const int h = 12 + r.range(6);
            int x = 8 + r.range(16);
            while (x < W - 12) {
                const int gw = 5 + r.range(5);
                if (r.range(6)) {
                    const uint32_t c = 0xFF000000u | (uint32_t)(r.range(80) * 0x010101);
                    for (int yy = y; yy < y + h && yy < H; ++yy)
                        for (int xx = x; xx < x + gw; ++xx)
                            if (r.range(3)) d.px[(size_t)yy * W + xx] = c;
                }
                x += gw + 2 + (r.range(8) == 0 ? 8 : 0);
            }
            y += h + 6 + r.range(14); // 行间空白
        }
    }
    return d;
}

const int kHeader = 40, kFooter = 30;

// 帧：顶部固定页眉、底部固定页脚，中间显示文档 [s, s + H - header - footer)
ls_image make_frame(const Doc &d, int H, int s, int header, int footer, Rng *noise = nullptr) {
    ls_image im = ls_image_alloc(d.W, H);
    for (int y = 0; y < H; ++y) {
        uint32_t *row = (uint32_t *)(void *)(im.data + (size_t)y * im.stride);
        for (int x = 0; x < d.W; ++x) {
            uint32_t c;
            if (y < header)
                c = ((x / 20 + y / 10) % 2) ? 0xFF2F6BFFu : 0xFF1D3F9Fu;
            else if (y >= H - footer)
                c = (x % 50 < 25) ? 0xFFE0E0E0u : 0xFF909090u;
            else
                c = d.at(x, s + y - header);
            row[x] = c;
        }
    }
    if (noise) {
        for (size_t i = 0; i < (size_t)d.W * H * 4; ++i) {
            if (i % 4 == 3) continue;
            int v = im.data[i] + noise->range(5) - 2;
            im.data[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    return im;
}

int push(ls_stitcher *st, const ls_image &f, ls_stitch_result *r) { return ls_stitch_push(st, &f, r); }

// 比较拼接结果与期望：页眉 + 文档 [0, s_last + middle) + 页脚
int count_mismatch(const ls_image &out, const Doc &d, int H, int s_last, int header, int footer, int tol) {
    const int middle = H - header - footer;
    int bad = 0;
    ls_image hdr = make_frame(d, H, 0, header, footer);
    for (int y = 0; y < out.height; ++y) {
        const uint8_t *o = out.data + (size_t)y * out.stride;
        for (int x = 0; x < d.W; ++x) {
            uint32_t e;
            if (y < header) {
                e = *(const uint32_t *)(const void *)(hdr.data + (size_t)y * hdr.stride + x * 4);
            } else if (y < header + s_last + middle) {
                e = d.at(x, y - header);
            } else {
                e = *(const uint32_t *)(const void *)(hdr.data + (size_t)(y - (header + s_last + middle) + H - footer) * hdr.stride + x * 4);
            }
            const uint8_t *ep = (const uint8_t *)&e;
            for (int c = 0; c < 4; ++c)
                if (std::abs((int)o[x * 4 + c] - (int)ep[c]) > tol) {
                    ++bad;
                    break;
                }
        }
    }
    ls_image_free(&hdr);
    return bad;
}

}  // namespace

TEST(stitch_header_footer_sequence) {
    const Doc d = make_doc(400, 3000, 7);
    const int H = 300;
    ls_stitcher *st = ls_stitch_create(100000);
    ls_stitch_result r;
    struct Step {
        int s;
        int status;
        int added;
    } steps[] = {
        {0, LS_STITCH_FIRST, 300},      {30, LS_STITCH_APPENDED, 30},  {31, LS_STITCH_APPENDED, 1},
        {31, LS_STITCH_UNCHANGED, 0},   {331, LS_STITCH_NO_MATCH, 0}, // 一下滚太远
        {20, LS_STITCH_BACKWARD, 0},    {131, LS_STITCH_APPENDED, 100}, {250, LS_STITCH_APPENDED, 119},
        {250, LS_STITCH_UNCHANGED, 0},  {400, LS_STITCH_APPENDED, 150},
    };
    for (const Step &sp : steps) {
        ls_image f = make_frame(d, H, sp.s, kHeader, kFooter);
        push(st, f, &r);
        ls_image_free(&f);
        CHECK_EQ(r.status, sp.status);
        CHECK_EQ(r.added, sp.added);
        if (r.status != sp.status) std::printf("    s=%d 状态 %d 期望 %d\n", sp.s, r.status, sp.status);
    }
    CHECK_EQ(r.total_height, 400 + H);
    CHECK_EQ(ls_stitch_height(st), 400 + H);
    CHECK(r.header >= kHeader);
    CHECK(r.footer >= kFooter);
    ls_image out;
    CHECK_EQ(ls_stitch_render(st, &out), 0);
    CHECK_EQ(out.width, 400);
    CHECK_EQ(out.height, 400 + H);
    CHECK_EQ(count_mismatch(out, d, H, 400, kHeader, kFooter, 0), 0);
    // 缩略预览等比缩小
    ls_image pv;
    CHECK_EQ(ls_stitch_preview(st, 100, 200, &pv), 0);
    CHECK(pv.width <= 100 && pv.height <= 200);
    CHECK_NEAR((double)pv.width / pv.height, 400.0 / (400 + H), 0.02);
    ls_image_free(&pv);
    ls_image_free(&out);
    ls_stitch_destroy(st);
}

TEST(stitch_plain_scroll_and_noise) {
    // 没有页眉页脚；每帧带 ±2 的噪声（屏幕渲染的细小差异）
    const Doc d = make_doc(320, 2400, 99);
    const int H = 240;
    ls_stitcher *st = ls_stitch_create(100000);
    ls_stitch_result r;
    Rng noise{5};
    int s = 0;
    const int dys[] = {0, 40, 80, 80, 7, 120, 150, 3, 90};
    for (int dy : dys) {
        s += dy;
        ls_image f = make_frame(d, H, s, 0, 0, &noise);
        push(st, f, &r);
        ls_image_free(&f);
        CHECK_EQ(r.status, dy == 0 ? LS_STITCH_FIRST : LS_STITCH_APPENDED);
        CHECK_EQ(r.added, dy == 0 ? H : dy);
    }
    ls_image out;
    CHECK_EQ(ls_stitch_render(st, &out), 0);
    CHECK_EQ(out.height, s + H);
    CHECK_EQ(count_mismatch(out, d, H, s, 0, 0, 3), 0);
    ls_image_free(&out);
    ls_stitch_destroy(st);
}

TEST(stitch_max_height) {
    const Doc d = make_doc(200, 2000, 3);
    const int H = 300;
    ls_stitcher *st = ls_stitch_create(600);
    ls_stitch_result r;
    const int ss[] = {0, 100, 200, 350, 400};
    const int expect[] = {LS_STITCH_FIRST, LS_STITCH_APPENDED, LS_STITCH_APPENDED, LS_STITCH_MAX, LS_STITCH_MAX};
    for (int i = 0; i < 5; ++i) {
        ls_image f = make_frame(d, H, ss[i], kHeader, kFooter);
        push(st, f, &r);
        ls_image_free(&f);
        CHECK_EQ(r.status, expect[i]);
    }
    CHECK_EQ(r.added, 0);
    CHECK_EQ(r.total_height, 600);
    ls_image out;
    CHECK_EQ(ls_stitch_render(st, &out), 0);
    CHECK_EQ(out.height, 600);
    // 页脚仍在最底部
    ls_image f0 = make_frame(d, H, 0, kHeader, kFooter);
    CHECK(std::memcmp(out.data + (size_t)599 * out.stride, f0.data + (size_t)299 * f0.stride, 200 * 4) == 0);
    ls_image_free(&f0);
    ls_image_free(&out);
    // 尺寸不一致的帧被拒绝
    ls_image small = ls_image_alloc(100, 100);
    CHECK_EQ(ls_stitch_push(st, &small, &r), -1);
    ls_image_free(&small);
    ls_stitch_destroy(st);
}

TEST(stitch_performance_1600x1000) {
    const Doc d = make_doc(1600, 6000, 11);
    const int H = 1000;
    ls_stitcher *st = ls_stitch_create(60000);
    ls_stitch_result r;
    double worst = 0, total = 0;
    int n = 0, s = 0;
    for (int i = 0; i < 12; ++i) {
        ls_image f = make_frame(d, H, s, 60, 40);
        const auto t0 = std::chrono::steady_clock::now();
        push(st, f, &r);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ls_image_free(&f);
        if (i > 0) {
            worst = ms > worst ? ms : worst;
            total += ms;
            ++n;
            CHECK_EQ(r.status, LS_STITCH_APPENDED);
        }
        s += 160 + (i % 3) * 37;
    }
    std::printf("    1600x1000 每帧拼接：平均 %.1f ms，最慢 %.1f ms\n", total / n, worst);
#ifdef NDEBUG
    CHECK(worst < 30.0);
#endif
    ls_image out;
    CHECK_EQ(ls_stitch_render(st, &out), 0);
    s -= 160 + (11 % 3) * 37;
    CHECK_EQ(out.height, s + H);
    CHECK_EQ(count_mismatch(out, d, H, s, 60, 40, 0), 0);
    ls_image_free(&out);
    ls_stitch_destroy(st);
}

TEST(stitch_local_change_and_replaced_page) {
    const Doc d = make_doc(400, 2000, 21);
    const Doc other = make_doc(400, 2000, 77);
    const int H = 300;
    ls_stitcher *st = ls_stitch_create(100000);
    ls_stitch_result r;
    ls_image f = make_frame(d, H, 100, kHeader, kFooter);
    push(st, f, &r);
    CHECK_EQ(r.status, LS_STITCH_FIRST);
    // 中间一小块变化（闪烁的光标、动图）：视为没动
    ls_image g = make_frame(d, H, 100, kHeader, kFooter);
    for (int y = 150; y < 166; ++y)
        for (int x = 200; x < 202; ++x) {
            uint8_t *p = g.data + (size_t)y * g.stride + x * 4;
            p[0] = p[1] = p[2] = 0;
        }
    push(st, g, &r);
    CHECK_EQ(r.status, LS_STITCH_UNCHANGED);
    CHECK_EQ(r.added, 0);
    // 换了一页完全不同的内容：找不到重叠
    ls_image h = make_frame(other, H, 300, kHeader, kFooter);
    push(st, h, &r);
    CHECK_EQ(r.status, LS_STITCH_NO_MATCH);
    // 丢弃的帧不影响后续：接着原来的位置往下滚
    ls_image k = make_frame(d, H, 160, kHeader, kFooter);
    push(st, k, &r);
    CHECK_EQ(r.status, LS_STITCH_APPENDED);
    CHECK_EQ(r.added, 60);
    CHECK_EQ(ls_stitch_height(st), 360);
    ls_image_free(&f);
    ls_image_free(&g);
    ls_image_free(&h);
    ls_image_free(&k);
    ls_stitch_destroy(st);
}
