// 贴图库（spec 3）：设置目录 pins/ 下每张 <时间戳>.png + library.json（顺序、来源），内容哈希去重，超过 libraryMax 删最旧。
#pragma once

#include <vector>

#include "UI/Gfx.h"
#include "lemonshot.h"

namespace ls {
namespace pins {

void AddToLibrary(const ls_image &img, const char *source);

namespace library {

void Show();

struct Thumb {
    ComPtr<ID2D1Bitmap> bmp;
    float w = 0, h = 0;  // 图片原始宽高（像素），按比例放进卡片
};

struct View {
    int count = 0;
    int hover = -1;
    bool hoverTrash = false;
    bool hoverClear = false;
    float scrollY = 0;
};

// 窗口内容（屏幕与自测共用）：w×h 为客户区逻辑尺寸
void Draw(Canvas &c, float w, float h, const std::vector<Thumb> &thumbs, const View &v);

}  // namespace library
}  // namespace pins
}  // namespace ls
