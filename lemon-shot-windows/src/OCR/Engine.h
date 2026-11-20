// 文字识别引擎（spec 4 Windows 部分）：内核 PP-OCRv5 + ncnn 识别文字，zxing-cpp 识别二维码/条码。
// 模型与字典以 RCDATA 编进 exe，首次使用时在后台线程创建并缓存；识别全部在同一条后台线程上做（内核对象不跨线程）。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "lemonshot.h"

namespace ls {
namespace ocr {

struct Line {
    std::string text;  // UTF-8
    ls_rect box;       // 图像像素坐标
    float confidence = 0;
};

enum EngineError { kErrNone = 0, kErrModel = 1, kErrRun = 2 };

struct Output {
    bool ok = false;
    int error = kErrNone;
    std::vector<Line> lines;
    std::vector<std::string> codes;  // 二维码/条码内容，从上到下
};

// 后台识别（文字 + 二维码），接管 img；done 在界面线程调用。必须在界面线程调用。
void RecognizeAsync(ls_image img, std::function<void(Output &)> done);

// 同步识别（自测用；不能和后台识别同时调用）
void RecognizeSync(const ls_image &img, Output &out);

// 内核整理：阅读顺序、合并换行、英文标点（UTF-8）
std::string LayoutText(const std::vector<Line> &lines, bool joinLines, bool asciiPunct);

// 一组结果显示成什么文本：优先二维码时只显示二维码内容；没有文字时退回二维码内容
std::string Compose(const Output &o, bool joinLines, bool asciiPunct, bool preferQR);
bool ShowsCodes(const Output &o, bool preferQR);

// 文本是不是一个可以打开的链接（单行 http/https）
bool IsLink(const std::string &s);

// 失败原因（界面文案）
std::wstring ErrorText(int error);

}  // namespace ocr
}  // namespace ls
