// WebP 编码（libwebp）。由 cmake/extras.cmake 或 scripts/build-macos.sh 单独编译，并给内核定义 LEMONSHOT_WITH_WEBP。
#include "lemonshot_extras.h"

#include <webp/encode.h>

#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int32_t ls_webp_encode(const ls_image *image, int32_t lossless, float quality, uint8_t **out, size_t *out_size) {
    if (out) *out = nullptr;
    if (out_size) *out_size = 0;
    if (!image || !image->data || image->width <= 0 || image->height <= 0 || !out || !out_size) return -1;
    if (image->width > WEBP_MAX_DIMENSION || image->height > WEBP_MAX_DIMENSION) return -1;
    // libwebp 要非预乘的 BGRA
    const int w = image->width, h = image->height;
    std::vector<uint8_t> buf((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t *s = image->data + (size_t)y * image->stride;
        uint8_t *d = buf.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x, s += 4, d += 4) {
            unsigned a = s[3];
            if (a == 255 || a == 0) {
                std::memcpy(d, s, 4);
            } else {
                d[0] = (uint8_t)((s[0] * 255 + a / 2) / a);
                d[1] = (uint8_t)((s[1] * 255 + a / 2) / a);
                d[2] = (uint8_t)((s[2] * 255 + a / 2) / a);
                d[3] = (uint8_t)a;
            }
        }
    }
    uint8_t *enc = nullptr;
    size_t size = lossless ? WebPEncodeLosslessBGRA(buf.data(), w, h, w * 4, &enc)
                           : WebPEncodeBGRA(buf.data(), w, h, w * 4, quality, &enc);
    if (!size || !enc) return -1;
    *out = (uint8_t *)std::malloc(size);
    if (!*out) {
        WebPFree(enc);
        return -1;
    }
    std::memcpy(*out, enc, size);
    *out_size = size;
    WebPFree(enc);
    return 0;
}

