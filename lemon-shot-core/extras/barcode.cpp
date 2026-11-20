// 二维码/条码识别（zxing-cpp）。由 cmake/extras.cmake 或 scripts/build-macos.sh 单独编译，并给内核定义 LEMONSHOT_WITH_QR（Windows 端；Mac 用 Vision）。
// zxing-cpp 内部可能抛异常，本文件单独以 -fexceptions 编译并在边界上全部截住。
#include "lemonshot_extras.h"

#include "ReadBarcode.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" char *ls_barcode_decode(const ls_image *image) {
    if (!image || !image->data || image->width <= 0 || image->height <= 0) return nullptr;
    try {
        ZXing::ImageView view(image->data, image->width, image->height, ZXing::ImageFormat::BGRA, image->stride);
        ZXing::ReaderOptions opts;
        opts.setTryHarder(true);
        opts.setTryRotate(true);
        opts.setTryInvert(true);
        ZXing::Barcodes found = ZXing::ReadBarcodes(view, opts);
        std::vector<std::pair<int, std::string>> items;
        for (const auto &b : found) {
            if (!b.isValid()) continue;
            std::string t = b.text();
            if (t.empty()) continue;
            items.emplace_back(b.position().topLeft().y, t);
        }
        if (items.empty()) return nullptr;
        std::stable_sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        std::string out;
        for (const auto &it : items) {
            if (!out.empty()) out += '\n';
            out += it.second;
        }
        char *p = (char *)std::malloc(out.size() + 1);
        if (!p) return nullptr;
        std::memcpy(p, out.c_str(), out.size() + 1);
        return p;
    } catch (...) {
        return nullptr;
    }
}

