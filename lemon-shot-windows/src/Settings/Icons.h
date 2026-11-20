// 图标：icons.json 里的 Lucide SVG 路径（24 网格），用 ls_svg_path_parse 解析后缓存成 D2D 路径几何。
#pragma once

struct ID2D1Geometry;

namespace ls {
namespace icons {

void Init();
// 名称不存在时返回 nullptr；几何坐标为 24×24 网格
ID2D1Geometry *Get(const char *name);
void Shutdown();

}  // namespace icons
}  // namespace ls
