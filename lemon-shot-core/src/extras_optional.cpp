// 可选组件未启用时的空实现（OCR、二维码、WebP 需要 third_party，按平台在构建里打开对应宏）。
#include "lemonshot_extras.h"

#include <cstring>

#ifndef LEMONSHOT_WITH_OCR
extern "C" ls_ocr *ls_ocr_create(const void *, size_t, const void *, size_t, const void *, size_t, const void *, size_t, const char *, size_t, int32_t) {
    return nullptr;
}
extern "C" void ls_ocr_destroy(ls_ocr *) {}
extern "C" int32_t ls_ocr_run(ls_ocr *, const ls_image *) { return -1; }
extern "C" void ls_ocr_line_at(const ls_ocr *, int32_t, ls_text_line *out) { std::memset(out, 0, sizeof *out); }
#endif

#ifndef LEMONSHOT_WITH_QR
extern "C" char *ls_barcode_decode(const ls_image *) { return nullptr; }
#endif

#ifndef LEMONSHOT_WITH_WEBP
extern "C" int32_t ls_webp_encode(const ls_image *, int32_t, float, uint8_t **out, size_t *out_size) {
    if (out) *out = nullptr;
    if (out_size) *out_size = 0;
    return -1;
}
#endif
