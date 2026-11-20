// 文字识别：PP-OCRv5 mobile（ncnn）。检测用 DB 概率图 + 连通域外接框（截图里的文字几乎都是水平或竖直的，
// 不需要旋转矩形，省掉 OpenCV），识别用 CTC 贪心解码。由 cmake/extras.cmake 或 scripts/build-macos.sh 单独编译，并给内核定义 LEMONSHOT_WITH_OCR（Windows 端）。
#include "lemonshot_extras.h"

#include <net.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Box {
    int x0, y0, x1, y1; // 原图像素，左上含、右下不含
    float score;
};

struct Line {
    std::string text;
    ls_rect box;
    float confidence;
};

// 检测输入最长边上限：截图文字小，取得比相机场景大；超过就等比缩小
constexpr int kDetMaxSide = 1600;
constexpr float kBinThresh = 0.3f;
constexpr float kBoxThresh = 0.6f;
constexpr float kUnclipRatio = 1.5f;
constexpr int kRecHeight = 48;
constexpr int kRecMaxWidth = 48 * 40;
constexpr float kRecPad = 0.25f;  // 识别裁剪左右留白（行高倍数）
constexpr float kRecVPad = 0.15f; // 第二次识别的上下留白（行高倍数）

}  // namespace

struct ls_ocr {
    ncnn::Net det;
    ncnn::Net rec;
    std::vector<unsigned char> det_bin, rec_bin; // ncnn 从内存加载时要求数据在网络生命周期内有效
    std::vector<std::string> dict;
    std::vector<Line> lines;
};

namespace {

// BGRA（预乘）→ BGR 连续数据
std::vector<unsigned char> to_bgr(const ls_image *img, int x, int y, int w, int h) {
    std::vector<unsigned char> out((size_t)w * h * 3);
    for (int j = 0; j < h; ++j) {
        const uint8_t *s = img->data + (size_t)(y + j) * img->stride + (size_t)x * 4;
        unsigned char *d = out.data() + (size_t)j * w * 3;
        for (int i = 0; i < w; ++i, s += 4, d += 3) {
            unsigned a = s[3];
            if (a == 255 || a == 0) {
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            } else { // 透明部分按白底合成
                d[0] = (unsigned char)std::min(255u, s[0] + (255 - a));
                d[1] = (unsigned char)std::min(255u, s[1] + (255 - a));
                d[2] = (unsigned char)std::min(255u, s[2] + (255 - a));
            }
        }
    }
    return out;
}

std::vector<Box> detect(ls_ocr *o, const ls_image *img) {
    const int W = img->width, H = img->height;
    float scale = 1.f;
    int w = W, h = H;
    if (std::max(W, H) > kDetMaxSide) {
        scale = (float)kDetMaxSide / std::max(W, H);
        w = std::max(1, (int)std::lround(W * scale));
        h = std::max(1, (int)std::lround(H * scale));
    }
    std::vector<unsigned char> bgr = to_bgr(img, 0, 0, W, H);
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr.data(), ncnn::Mat::PIXEL_BGR, W, H, w, h);
    const int wpad = (w + 31) / 32 * 32 - w;
    const int hpad = (h + 31) / 32 * 32 - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2, wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 114.f);
    const float mean[3] = {0.485f * 255.f, 0.456f * 255.f, 0.406f * 255.f};
    const float norm[3] = {1 / 0.229f / 255.f, 1 / 0.224f / 255.f, 1 / 0.225f / 255.f};
    in_pad.substract_mean_normalize(mean, norm);
    ncnn::Extractor ex = o->det.create_extractor();
    ex.input("in0", in_pad);
    ncnn::Mat out;
    ex.extract("out0", out);
    const int ow = out.w, oh = out.h;
    const float *prob = out.channel(0);
    // 连通域（8 邻接）
    std::vector<int> label((size_t)ow * oh, 0);
    std::vector<int> stack;
    std::vector<Box> boxes;
    int next = 0;
    for (int y = 0; y < oh; ++y) {
        for (int x = 0; x < ow; ++x) {
            size_t idx = (size_t)y * ow + x;
            if (label[idx] || prob[idx] <= kBinThresh) continue;
            ++next;
            int x0 = x, y0 = y, x1 = x, y1 = y;
            double sum = 0;
            int count = 0;
            stack.clear();
            stack.push_back((int)idx);
            label[idx] = next;
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int cx = cur % ow, cy = cur / ow;
                sum += prob[cur];
                ++count;
                x0 = std::min(x0, cx); x1 = std::max(x1, cx);
                y0 = std::min(y0, cy); y1 = std::max(y1, cy);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= ow || ny >= oh) continue;
                        size_t ni = (size_t)ny * ow + nx;
                        if (label[ni] || prob[ni] <= kBinThresh) continue;
                        label[ni] = next;
                        stack.push_back((int)ni);
                    }
                }
            }
            float score = (float)(sum / count);
            int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            if (score < kBoxThresh || std::min(bw, bh) < 3) continue;
            // DB 的 unclip：按 面积×比例/周长 向外扩
            float d = (float)bw * bh * kUnclipRatio / (2.f * (bw + bh));
            float fx0 = x0 - d, fy0 = y0 - d, fx1 = x1 + 1 + d, fy1 = y1 + 1 + d;
            // 去掉填充、换回原图坐标
            fx0 = (fx0 - wpad / 2) / scale; fx1 = (fx1 - wpad / 2) / scale;
            fy0 = (fy0 - hpad / 2) / scale; fy1 = (fy1 - hpad / 2) / scale;
            Box b;
            b.x0 = std::max(0, (int)std::floor(fx0));
            b.y0 = std::max(0, (int)std::floor(fy0));
            b.x1 = std::min(W, (int)std::ceil(fx1));
            b.y1 = std::min(H, (int)std::ceil(fy1));
            b.score = score;
            if (b.x1 - b.x0 >= 4 && b.y1 - b.y0 >= 4) boxes.push_back(b);
        }
    }
    // 阅读顺序由 ls_text_layout 统一整理，这里只给一个确定的顺序
    std::sort(boxes.begin(), boxes.end(), [](const Box &a, const Box &b) {
        return a.y0 != b.y0 ? a.y0 < b.y0 : a.x0 < b.x0;
    });
    return boxes;
}

void recognize_once(ls_ocr *o, const ls_image *img, const Box &b0, float vpad, std::string &text, float &confidence) {
    Box b = b0;
    const int vp = (int)std::lround((b0.y1 - b0.y0) * vpad);
    b.y0 = std::max(0, b0.y0 - vp);
    b.y1 = std::min(img->height, b0.y1 + vp);
    int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    std::vector<unsigned char> crop = to_bgr(img, b.x0, b.y0, bw, bh);
    // 左右各留 PAD 倍行高的白边（取裁剪区左右边缘颜色）：贴边的首尾字符和紧凑的中文更不容易丢
    const int pad = (int)std::lround(bh * kRecPad);
    std::vector<unsigned char> bgr((size_t)(bw + 2 * pad) * bh * 3);
    for (int y = 0; y < bh; ++y) {
        const unsigned char *s = &crop[(size_t)y * bw * 3];
        unsigned char *d = &bgr[(size_t)y * (bw + 2 * pad) * 3];
        for (int x = 0; x < pad; ++x) std::memcpy(d + x * 3, s, 3);
        std::memcpy(d + pad * 3, s, (size_t)bw * 3);
        for (int x = 0; x < pad; ++x) std::memcpy(d + (pad + bw + x) * 3, s + (bw - 1) * 3, 3);
    }
    bw += 2 * pad;
    // 竖排文字：逆时针转 90°（与 PaddleOCR 一致）
    bool vertical = bh >= bw * 3 / 2 && bh > 2 * kRecHeight / 3;
    if (vertical) {
        std::vector<unsigned char> rot(bgr.size());
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                // 新图宽 bh、高 bw：新(x', y') = (y, bw-1-x)
                const unsigned char *s = &bgr[((size_t)y * bw + x) * 3];
                unsigned char *d = &rot[((size_t)(bw - 1 - x) * bh + y) * 3];
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            }
        bgr.swap(rot);
        std::swap(bw, bh);
    }
    int tw = std::max(16, std::min(kRecMaxWidth, (int)std::lround((double)bw * kRecHeight / bh)));
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr.data(), ncnn::Mat::PIXEL_BGR, bw, bh, tw, kRecHeight);
    const float mean[3] = {127.5f, 127.5f, 127.5f};
    const float norm[3] = {1 / 127.5f, 1 / 127.5f, 1 / 127.5f};
    in.substract_mean_normalize(mean, norm);
    ncnn::Extractor ex = o->rec.create_extractor();
    ex.input("in0", in);
    ncnn::Mat out;
    ex.extract("out0", out);
    int last = 0;
    double conf = 0;
    int n = 0;
    text.clear();
    for (int i = 0; i < out.h; ++i) {
        const float *p = out.row(i);
        int idx = 0;
        float best = -1e9f;
        for (int j = 0; j < out.w; ++j)
            if (p[j] > best) { best = p[j]; idx = j; }
        if (idx == last) continue;
        last = idx;
        if (idx <= 0) continue;
        conf += best;
        ++n;
        size_t id = (size_t)idx - 1;
        if (id >= o->dict.size()) {
            if (!text.empty() && text.back() != ' ') text += ' ';
        } else {
            text += o->dict[id];
        }
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    confidence = n ? (float)(conf / n) : 0.f;
}

// a 的码点序列是否为 b 的子序列（b 只比 a 多字、不改字）
bool is_subsequence(const std::string &a, const std::string &b) {
    auto next_cp = [](const std::string &s, size_t &i) {
        size_t start = i++;
        while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) ++i;
        return s.substr(start, i - start);
    };
    size_t i = 0, j = 0;
    while (i < a.size()) {
        std::string ca = next_cp(a, i);
        bool found = false;
        while (j < b.size()) {
            if (next_cp(b, j) == ca) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

// CTC 识别对裁剪高度很敏感，常见错误是漏掉笔画少的字（小、文、一）。用两种竖向留白各识别一次：
// 一个结果只是另一个漏了字时取完整的那个，否则取平均置信度高的。
void recognize(ls_ocr *o, const ls_image *img, const Box &b, Line &line) {
    std::string t1, t2;
    float c1 = 0, c2 = 0;
    recognize_once(o, img, b, 0.f, t1, c1);
    recognize_once(o, img, b, kRecVPad, t2, c2);
    if (t1 == t2) line.text = t1, line.confidence = std::max(c1, c2);
    else if (is_subsequence(t1, t2)) line.text = t2, line.confidence = c2;
    else if (is_subsequence(t2, t1)) line.text = t1, line.confidence = c1;
    else if (c2 > c1) line.text = t2, line.confidence = c2;
    else line.text = t1, line.confidence = c1;
    line.box = ls_rect{(float)b.x0, (float)b.y0, (float)(b.x1 - b.x0), (float)(b.y1 - b.y0)};
}

}  // namespace

extern "C" ls_ocr *ls_ocr_create(const void *det_param, size_t det_param_size, const void *det_bin, size_t det_bin_size,
                                 const void *rec_param, size_t rec_param_size, const void *rec_bin, size_t rec_bin_size,
                                 const char *dict, size_t dict_size, int32_t threads) {
    if (!det_param || !det_bin || !rec_param || !rec_bin || !dict) return nullptr;
    ls_ocr *o = new ls_ocr();
    int nt = threads > 0 ? threads : (int)std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (ncnn::Net *net : {&o->det, &o->rec}) {
        net->opt.use_vulkan_compute = false;
        net->opt.lightmode = true;
        net->opt.num_threads = nt;
    }
    std::string dp((const char *)det_param, det_param_size), rp((const char *)rec_param, rec_param_size);
    o->det_bin.assign((const unsigned char *)det_bin, (const unsigned char *)det_bin + det_bin_size);
    o->rec_bin.assign((const unsigned char *)rec_bin, (const unsigned char *)rec_bin + rec_bin_size);
    if (o->det.load_param_mem(dp.c_str()) != 0 || o->rec.load_param_mem(rp.c_str()) != 0 ||
        o->det.load_model(o->det_bin.data()) <= 0 || o->rec.load_model(o->rec_bin.data()) <= 0) {
        delete o;
        return nullptr;
    }
    // 字典：每行一个字符（去掉 \r）
    size_t start = 0;
    for (size_t i = 0; i <= dict_size; ++i) {
        if (i == dict_size || dict[i] == '\n') {
            size_t end = i;
            if (end > start && dict[end - 1] == '\r') --end;
            if (i < dict_size || end > start) o->dict.emplace_back(dict + start, end - start);
            start = i + 1;
        }
    }
    return o;
}

extern "C" void ls_ocr_destroy(ls_ocr *ocr) { delete ocr; }

extern "C" int32_t ls_ocr_run(ls_ocr *ocr, const ls_image *image) {
    if (!ocr || !image || !image->data || image->width < 4 || image->height < 4) return -1;
    ocr->lines.clear();
    std::vector<Box> boxes = detect(ocr, image);
    for (const Box &b : boxes) {
        Line line;
        recognize(ocr, image, b, line);
        if (!line.text.empty()) ocr->lines.push_back(line);
    }
    return (int32_t)ocr->lines.size();
}

extern "C" void ls_ocr_line_at(const ls_ocr *ocr, int32_t index, ls_text_line *out) {
    const Line &l = ocr->lines[(size_t)index];
    out->text = l.text.c_str();
    out->box = l.box;
    out->confidence = l.confidence;
}

