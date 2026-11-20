// 像素处理
#include "test.h"

namespace {

uint8_t *px(const ls_image &im, int x, int y) { return im.data + (size_t)y * im.stride + x * 4; }

void set(ls_image &im, int x, int y, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    uint8_t *p = px(im, x, y);
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = a;
}

ls_image make_pattern(int w, int h) {
    ls_image im = ls_image_alloc(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) set(im, x, y, (uint8_t)(x * 7 + y), (uint8_t)(y * 5), (uint8_t)(x * 3 + y * 2), 255);
    return im;
}

}  // namespace

TEST(image_alloc_and_free) {
    ls_image im = ls_image_alloc(3, 2);
    CHECK(im.data != nullptr);
    CHECK_EQ(im.stride, 12);
    CHECK_EQ(px(im, 2, 1)[3], 0);
    ls_image_free(&im);
    CHECK(im.data == nullptr);
    ls_image bad = ls_image_alloc(0, 5);
    CHECK(bad.data == nullptr);
}

TEST(image_pixelate_blocks_aligned_to_image) {
    ls_image src = make_pattern(8, 8);
    ls_image out;
    CHECK_EQ(ls_pixelate(&src, 0, 0, 8, 8, 4, &out), 0);
    // 左上块 4x4 的平均
    int sum = 0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) sum += px(src, x, y)[0];
    CHECK_EQ(px(out, 0, 0)[0], (sum + 8) / 16);
    CHECK_EQ(px(out, 3, 3)[0], px(out, 0, 0)[0]);
    CHECK_EQ(px(out, 0, 0)[3], 255);
    // 区域 (2,2,4,4) 的块边界仍然对齐整张图：(2,2) 属于左上块，(4,4) 属于右下块
    ls_image part;
    CHECK_EQ(ls_pixelate(&src, 2, 2, 4, 4, 4, &part), 0);
    CHECK_EQ(part.width, 4);
    CHECK_EQ(px(part, 0, 0)[0], px(out, 2, 2)[0]);
    CHECK_EQ(px(part, 2, 2)[0], px(out, 4, 4)[0]);
    CHECK_EQ(px(part, 1, 2)[1], px(out, 3, 4)[1]);
    // 超出原图的部分按边缘夹取
    ls_image edge;
    CHECK_EQ(ls_pixelate(&src, 6, 6, 4, 4, 4, &edge), 0);
    CHECK_EQ(px(edge, 3, 3)[0], px(out, 7, 7)[0]);
    ls_image_free(&src);
    ls_image_free(&out);
    ls_image_free(&part);
    ls_image_free(&edge);
}

TEST(image_blur) {
    // 纯色不变
    ls_image flat = ls_image_alloc(40, 30);
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 40; ++x) set(flat, x, y, 10, 20, 30, 255);
    ls_image out;
    CHECK_EQ(ls_blur(&flat, 5, 5, 20, 10, 6, &out), 0);
    bool same = true;
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) same = same && px(out, x, y)[0] == 10 && px(out, x, y)[2] == 30 && px(out, x, y)[3] == 255;
    CHECK(same);
    ls_image_free(&out);
    // 单个亮点对称扩散，总量大致守恒
    ls_image dot = ls_image_alloc(41, 41);
    for (int y = 0; y < 41; ++y)
        for (int x = 0; x < 41; ++x) set(dot, x, y, 0, 0, 0, 255);
    set(dot, 20, 20, 255, 255, 255, 255);
    CHECK_EQ(ls_blur(&dot, 0, 0, 41, 41, 3, &out), 0);
    CHECK(px(out, 20, 20)[0] > px(out, 22, 20)[0]);
    CHECK(px(out, 22, 20)[0] > 0);
    CHECK_NEAR(px(out, 17, 20)[0], px(out, 23, 20)[0], 1);
    CHECK_NEAR(px(out, 20, 17)[0], px(out, 20, 23)[0], 1);
    int total = 0;
    for (int y = 0; y < 41; ++y)
        for (int x = 0; x < 41; ++x) total += px(out, x, y)[0];
    CHECK_NEAR(total, 255, 60);
    // 区域模糊与整图模糊同一位置的结果相同（区域边缘用真实像素而不是夹取）
    ls_image big = make_pattern(64, 64);
    ls_image whole, part;
    ls_blur(&big, 0, 0, 64, 64, 4, &whole);
    ls_blur(&big, 20, 24, 10, 8, 4, &part);
    bool equal = true;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 10; ++x)
            for (int c = 0; c < 4; ++c) equal = equal && px(part, x, y)[c] == px(whole, 20 + x, 24 + y)[c];
    CHECK(equal);
    ls_image_free(&flat);
    ls_image_free(&dot);
    ls_image_free(&out);
    ls_image_free(&big);
    ls_image_free(&whole);
    ls_image_free(&part);
}

TEST(image_crop) {
    ls_image src = make_pattern(10, 10);
    ls_image out;
    CHECK_EQ(ls_image_crop(&src, 2, 3, 4, 5, &out), 0);
    CHECK_EQ(out.width, 4);
    CHECK_EQ(out.height, 5);
    CHECK(std::memcmp(px(out, 0, 0), px(src, 2, 3), 4) == 0);
    CHECK(std::memcmp(px(out, 3, 4), px(src, 5, 7), 4) == 0);
    ls_image_free(&out);
    // 超出部分透明
    CHECK_EQ(ls_image_crop(&src, 8, 8, 4, 4, &out), 0);
    CHECK(std::memcmp(px(out, 1, 1), px(src, 9, 9), 4) == 0);
    CHECK_EQ(px(out, 2, 2)[3], 0);
    ls_image_free(&out);
    ls_image_free(&src);
}

TEST(image_resize) {
    // 缩小：面积平均
    ls_image src = ls_image_alloc(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) set(src, x, y, (uint8_t)(x * 40 + y * 10), 0, 0, 255);
    ls_image out;
    CHECK_EQ(ls_image_resize(&src, 2, 2, &out), 0);
    CHECK_EQ(px(out, 0, 0)[0], (0 + 40 + 10 + 50) / 4);
    CHECK_EQ(px(out, 1, 1)[0], (100 + 140 + 110 + 150) / 4);
    CHECK_EQ(px(out, 1, 1)[3], 255);
    ls_image_free(&out);
    // 非整数倍缩小
    CHECK_EQ(ls_image_resize(&src, 3, 3, &out), 0);
    CHECK(px(out, 0, 0)[0] < px(out, 2, 0)[0]);
    ls_image_free(&out);
    // 放大：双线性
    ls_image two = ls_image_alloc(2, 1);
    set(two, 0, 0, 0, 0, 0, 255);
    set(two, 1, 0, 200, 0, 0, 255);
    CHECK_EQ(ls_image_resize(&two, 4, 1, &out), 0);
    CHECK_EQ(px(out, 0, 0)[0], 0);
    CHECK_EQ(px(out, 1, 0)[0], 50);
    CHECK_EQ(px(out, 2, 0)[0], 150);
    CHECK_EQ(px(out, 3, 0)[0], 200);
    ls_image_free(&out);
    // 等大不变
    ls_image pat = make_pattern(13, 7);
    CHECK_EQ(ls_image_resize(&pat, 13, 7, &out), 0);
    bool same = true;
    for (int y = 0; y < 7; ++y) same = same && std::memcmp(px(out, 0, y), px(pat, 0, y), 13 * 4) == 0;
    CHECK(same);
    ls_image_free(&out);
    ls_image_free(&pat);
    ls_image_free(&two);
    ls_image_free(&src);
}

TEST(image_rotate_flip) {
    ls_image src = make_pattern(3, 2);
    ls_image cw, ccw, h, v;
    CHECK_EQ(ls_image_rotate90(&src, 1, &cw), 0);
    CHECK_EQ(cw.width, 2);
    CHECK_EQ(cw.height, 3);
    // 顺时针：原左下角到左上角
    CHECK(std::memcmp(px(cw, 0, 0), px(src, 0, 1), 4) == 0);
    CHECK(std::memcmp(px(cw, 1, 0), px(src, 0, 0), 4) == 0);
    CHECK(std::memcmp(px(cw, 1, 2), px(src, 2, 0), 4) == 0);
    CHECK_EQ(ls_image_rotate90(&src, 0, &ccw), 0);
    // 逆时针：原右上角到左上角
    CHECK(std::memcmp(px(ccw, 0, 0), px(src, 2, 0), 4) == 0);
    CHECK(std::memcmp(px(ccw, 1, 2), px(src, 0, 1), 4) == 0);
    CHECK_EQ(ls_image_flip(&src, 1, &h), 0);
    CHECK(std::memcmp(px(h, 0, 0), px(src, 2, 0), 4) == 0);
    CHECK_EQ(ls_image_flip(&src, 0, &v), 0);
    CHECK(std::memcmp(px(v, 1, 0), px(src, 1, 1), 4) == 0);
    ls_image_free(&src);
    ls_image_free(&cw);
    ls_image_free(&ccw);
    ls_image_free(&h);
    ls_image_free(&v);
}

TEST(image_frame_corners_and_shadow) {
    ls_image src = ls_image_alloc(40, 30);
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 40; ++x) set(src, x, y, 200, 150, 100, 255);
    ls_image out;
    // 只做圆角：尺寸不变，角上透明，中间不变
    ls_color none{0, 0, 0, 0};
    CHECK_EQ(ls_image_frame(&src, 8, 0, 0, none, &out), 0);
    CHECK_EQ(out.width, 40);
    CHECK_EQ(out.height, 30);
    CHECK_EQ(px(out, 0, 0)[3], 0);
    CHECK_EQ(px(out, 39, 29)[3], 0);
    CHECK_EQ(px(out, 20, 15)[3], 255);
    CHECK_EQ(px(out, 20, 0)[3], 255);
    CHECK(px(out, 2, 2)[3] > 0 && px(out, 2, 2)[3] < 255);
    // 预乘：颜色不超过透明度
    CHECK(px(out, 2, 2)[0] <= px(out, 2, 2)[3]);
    ls_image_free(&out);
    // 投影：四周留 shadow + |offset| 的边，下方比上方深
    ls_color shadow{0, 0, 0, 128};
    CHECK_EQ(ls_image_frame(&src, 6, 10, 4, shadow, &out), 0);
    const int pad = 14;
    CHECK_EQ(out.width, 40 + 2 * pad);
    CHECK_EQ(out.height, 30 + 2 * pad);
    CHECK_EQ(px(out, pad + 20, pad + 15)[3], 255);
    const int below = px(out, pad + 20, pad + 30 + 3)[3];
    const int above = px(out, pad + 20, pad - 3)[3];
    CHECK(below > 0);
    CHECK(below > above);
    CHECK_EQ(px(out, 0, 0)[3], 0);
    CHECK(px(out, pad + 20, pad + 30 + 3)[0] == 0); // 黑色投影
    ls_image_free(&out);
    // 没有圆角只有投影
    CHECK_EQ(ls_image_frame(&src, 0, 6, 0, shadow, &out), 0);
    CHECK_EQ(px(out, 6, 6)[3], 255);
    ls_image_free(&out);
    ls_image_free(&src);
}

TEST(image_draw_source_over) {
    ls_image dst = ls_image_alloc(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) set(dst, x, y, 0, 0, 0, 255);
    ls_image src = ls_image_alloc(2, 2);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) set(src, x, y, 128, 128, 128, 128); // 50% 白（预乘）
    set(src, 1, 1, 0, 0, 255, 255);                                      // 不透明红
    ls_image_draw(&dst, &src, 3, 3);                                     // 只有左上像素落在 dst 内
    CHECK_EQ(px(dst, 3, 3)[0], 128);
    CHECK_EQ(px(dst, 3, 3)[3], 255);
    CHECK_EQ(px(dst, 2, 2)[0], 0);
    ls_image_draw(&dst, &src, 0, 0);
    CHECK_EQ(px(dst, 1, 1)[2], 255);
    CHECK_EQ(px(dst, 1, 1)[0], 0);
    ls_image_draw(&dst, &src, -1, -1); // 只有右下的红像素落在 (0,0)
    CHECK_EQ(px(dst, 0, 0)[2], 255);
    ls_image_free(&dst);
    ls_image_free(&src);
}
