// 颜色换算、名称、HEX、自定义格式
#include "test.h"

TEST(color_srgb_to_p3_known_values) {
    const float red[3] = {1, 0, 0};
    float out[3];
    ls_color_convert(LS_SPACE_SRGB, LS_SPACE_P3, red, out);
    CHECK_NEAR(out[0], 0.9175, 0.002);
    CHECK_NEAR(out[1], 0.2003, 0.002);
    CHECK_NEAR(out[2], 0.1386, 0.002);
    const float green[3] = {0, 1, 0};
    ls_color_convert(LS_SPACE_SRGB, LS_SPACE_P3, green, out);
    CHECK_NEAR(out[0], 0.4584, 0.002);
    CHECK_NEAR(out[1], 0.9853, 0.002);
    CHECK_NEAR(out[2], 0.2983, 0.002);
    // P3 纯红超出 sRGB，夹到 0..1
    ls_color_convert(LS_SPACE_P3, LS_SPACE_SRGB, red, out);
    CHECK_NEAR(out[0], 1, 1e-6);
    CHECK_NEAR(out[1], 0, 1e-6);
    CHECK_NEAR(out[2], 0, 1e-6);
}

TEST(color_round_trips_and_neutrals) {
    const float c[3] = {0.5f, 0.4f, 0.3f};
    float a[3], b[3];
    for (int sp = 0; sp < 4; ++sp) {
        ls_color_convert(LS_SPACE_SRGB, sp, c, a);
        ls_color_convert(sp, LS_SPACE_SRGB, a, b);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(b[i], c[i], 1e-4);
    }
    // 白、黑在任何空间都不变；灰在 D65 空间之间保持中性
    const float white[3] = {1, 1, 1}, black[3] = {0, 0, 0}, gray[3] = {0.5f, 0.5f, 0.5f};
    for (int sp = 0; sp < 4; ++sp) {
        ls_color_convert(LS_SPACE_SRGB, sp, white, a);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(a[i], 1, 1e-4);
        ls_color_convert(LS_SPACE_SRGB, sp, black, a);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(a[i], 0, 1e-6);
    }
    ls_color_convert(LS_SPACE_SRGB, LS_SPACE_GENERIC, gray, a);
    CHECK_NEAR(a[0], 0.4247, 0.001); // (0.21404)^(1/1.8)
    CHECK_NEAR(a[1], a[0], 1e-4);
    CHECK_NEAR(a[2], a[0], 1e-4);
    ls_color_convert(LS_SPACE_SRGB, LS_SPACE_ADOBE, gray, a);
    CHECK_NEAR(a[0], 0.4961, 0.001); // (0.21404)^(256/563)
    // 同空间不变
    ls_color_convert(LS_SPACE_P3, LS_SPACE_P3, c, a);
    for (int i = 0; i < 3; ++i) CHECK_NEAR(a[i], c[i], 1e-5);
}

TEST(color_names) {
    char buf[64];
    int32_t n = ls_color_name(0x64, 0x95, 0xED, "zh-Hans", buf, sizeof buf);
    CHECK_STR(buf, "矢车菊蓝");
    CHECK_EQ(n, (int)std::strlen("矢车菊蓝"));
    ls_color_name(0x64, 0x95, 0xED, "en", buf, sizeof buf);
    CHECK_STR(buf, "Cornflower Blue");
    ls_color_name(0x66, 0x96, 0xEA, "zh-Hans", buf, sizeof buf);
    CHECK_STR(buf, "矢车菊蓝");
    ls_color_name(0, 0, 0, "zh-Hans", buf, sizeof buf);
    CHECK_STR(buf, "黑色");
    ls_color_name(255, 255, 255, "en", buf, sizeof buf);
    CHECK_STR(buf, "White");
    ls_color_name(250, 10, 5, "en", buf, sizeof buf);
    CHECK_STR(buf, "Red");
    // 缓冲区不够：按完整的 UTF-8 字符截断
    char small[5];
    n = ls_color_name(0x64, 0x95, 0xED, "zh-Hans", small, sizeof small);
    CHECK_EQ(n, 3);
    CHECK_STR(small, "矢");
    // 未知语言按英文
    ls_color_name(0x64, 0x95, 0xED, "ja", buf, sizeof buf);
    CHECK_STR(buf, "Cornflower Blue");
}

TEST(color_hex_and_format) {
    char buf[64];
    CHECK_EQ(ls_color_hex(0x2F, 0x6B, 0xFF, 1, 1, buf, sizeof buf), 7);
    CHECK_STR(buf, "#2F6BFF");
    ls_color_hex(0x2F, 0x6B, 0xFF, 0, 0, buf, sizeof buf);
    CHECK_STR(buf, "2f6bff");
    char tiny[4];
    ls_color_hex(0x2F, 0x6B, 0xFF, 1, 1, tiny, sizeof tiny);
    CHECK_STR(tiny, "#2F");
    ls_color_format("rgb(_R, _G, _B)", 47, 107, 255, 1, buf, sizeof buf);
    CHECK_STR(buf, "rgb(47, 107, 255)");
    ls_color_format("_r,_g,_b", 47, 107, 255, 1, buf, sizeof buf);
    CHECK_STR(buf, "0.184,0.420,1.000");
    ls_color_format("#_HEX", 47, 107, 255, 0, buf, sizeof buf);
    CHECK_STR(buf, "#2f6bff");
    ls_color_format("[_HEX] _R", 0, 0, 0, 1, buf, sizeof buf);
    CHECK_STR(buf, "[000000] 0");
    ls_color_format("UIColor(red: _r, green: _g, blue: _b, alpha: 1)", 255, 0, 128, 1, buf, sizeof buf);
    CHECK_STR(buf, "UIColor(red: 1.000, green: 0.000, blue: 0.502, alpha: 1)");
    ls_color_format("_x_", 1, 2, 3, 1, buf, sizeof buf);
    CHECK_STR(buf, "_x_");
}
