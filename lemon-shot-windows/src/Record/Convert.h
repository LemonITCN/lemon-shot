// 像素换算（采集线程上用）：BGRA 面积平均缩小、BGRA → NV12。
#pragma once

#include <cstdint>
#include <vector>

namespace ls {
namespace rec {

// 面积平均缩小（目标不大于源）；源与目标尺寸相同或只差 1 像素时直接裁剪。逐行流式处理，内存只占几行。
class Scaler {
public:
    void Setup(int sw, int sh, int dw, int dh);
    // src、dst 都是 BGRA；输出 alpha 置 255
    void Run(const uint8_t *src, int sstride, uint8_t *dst, int dstride);
    int SrcW() const { return sw_; }
    int SrcH() const { return sh_; }

private:
    struct Span {
        int start = 0;
        std::vector<uint16_t> w;  // 权重，和为 1 << 14
    };
    void HRow(const uint8_t *row, uint16_t *out);
    int sw_ = 0, sh_ = 0, dw_ = 0, dh_ = 0;
    int mode_ = 0;  // 0 裁剪，1 二倍，2 通用
    std::vector<Span> xs_, ys_;
    std::vector<uint16_t> rowA_, rowB_;
    std::vector<uint32_t> acc_;
};

// BGRA → NV12（有限范围 16–235）；w、h 必须为偶数
void BgraToNv12(const uint8_t *src, int stride, int w, int h, uint8_t *y, int ystride, uint8_t *uv, int uvstride, bool bt709);

}  // namespace rec
}  // namespace ls
