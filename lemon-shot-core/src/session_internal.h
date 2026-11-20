// 截图会话内部定义：标注模型、绘制缓存、马赛克处理缓存、会话状态。
#pragma once

#include "internal.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

namespace lsk {

// ---------------------------------------------------------------- 标注模型（撤销快照只复制这一层）

struct Shape {
    int32_t id = 0;
    int32_t type = 0; // ls_tool
    ls_style style{};
    ls_point p0{0, 0}, p1{0, 0}; // 见 ls_shape_view；文字 p0 = 排版框左上角；序号 p0 = 圆心
    ls_point ctrl{0, 0};         // 曲线箭头控制点（二次曲线）
    std::vector<ls_point> pts;   // 画笔、马赛克画笔
    std::string text;
    ls_rect text_rect{0, 0, 0, 0}; // 文字排版框（测量结果）
    int32_t number = 0;            // 序号
    uint64_t rev = 0;              // 几何/样式版本（全局唯一递增），绘制缓存据此判断要不要重建
};

struct Snapshot {
    std::vector<Shape> shapes;
    int32_t step_next = 1;
};

// ---------------------------------------------------------------- 马赛克：整张冻结画面按需分块处理

// 像素化块网格、模糊都对齐整张图，所以任意区域的结果 = 从块里拷出来，移动马赛克时不必重算。
struct TileSet {
    int kind = 0;    // 0 像素化，1 模糊
    int param = 0;   // 像素化块大小（像素）；模糊为 sigma*16
    int tile = 256;  // 块边长（像素化时为块大小的整数倍）
    int nx = 0, ny = 0;
    std::vector<ls_image> tiles;
    uint64_t last_use = 0;
    ~TileSet();
};

struct TileCache {
    const ls_image *base = nullptr;
    std::vector<std::unique_ptr<TileSet>> sets;
    uint64_t clock = 0;
    TileSet *get(int kind, int param);
    // 把处理后的 (x,y,w,h) 区域（像素，须在底图内）拷到 dst 的 (dx,dy)
    void gather(TileSet *ts, int x, int y, int w, int h, ls_image *dst, int dx, int dy);
};

// ---------------------------------------------------------------- 绘制缓存

struct ShapeGeom {
    uint64_t rev = 0;
    bool valid = false;
    ls_style vstyle{};
    ls_color fill_color{0, 0, 0, 0};
    ls_path fill, stroke;
    bool has_fill = false, has_stroke = false;
    int32_t dashed = 0;
    ls_rect bounds{0, 0, 0, 0};
    std::string text; // 序号的数字
    ls_rect text_rect{0, 0, 0, 0};
    ls_color text_color{0, 0, 0, 0}, text_background{0, 0, 0, 0}, text_outline{0, 0, 0, 0};
    float text_size = 0;
    ls_rect magnify_src{0, 0, 0, 0};
    float radius = 0;
    // 马赛克
    ls_image patch{nullptr, 0, 0, 0};
    ls_rect patch_rect{0, 0, 0, 0};
    int px_x = 0, px_y = 0;     // patch 左上角（像素）
    std::vector<uint8_t> mask;  // 画笔样式的笔刷透明度（与 patch 同尺寸）
    size_t mask_pts = 0;        // 已画进 mask 的点数（画笔增量更新）
    std::vector<ls_point> saved_pts; // 上次画 mask 时的点列，用来判断这次是不是只追加了新点
    int patch_kind = -1, patch_param = 0;
    float patch_brush = 0;
    bool patch_valid = false;

    ShapeGeom() = default;
    ShapeGeom(const ShapeGeom &) = delete;
    ShapeGeom &operator=(const ShapeGeom &) = delete;
    ~ShapeGeom() { ls_image_free(&patch); }
};

// ---------------------------------------------------------------- 几何（shapes.cpp）

inline ls_point pt(float x, float y) { return ls_point{x, y}; }
inline ls_rect rc(float x, float y, float w, float h) { return ls_rect{x, y, w, h}; }
ls_rect rect_from(ls_point a, ls_point b);
ls_rect rect_union(ls_rect a, ls_rect b);
bool rect_contains(ls_rect r, float x, float y);
float dist(ls_point a, ls_point b);
float dist_to_segment(ls_point p, ls_point a, ls_point b);
float luma(ls_color c);

bool is_rect_shape(const Shape &s);  // 矩形、椭圆、局部高亮、马赛克矩形样式：8 个控制点
bool is_brush_shape(const Shape &s); // 画笔、马赛克画笔样式：点列
bool is_line_shape(const Shape &s);  // 直线、箭头
float effective_width(const Shape &s);
float magnify_radius(const Shape &s);
ls_point curve_mid(const Shape &s);  // 曲线箭头 t=0.5 处（控制点手柄的位置）
void default_ctrl(Shape &s);

// edge_only：选着绘图工具时为 true，区域类标注只有边框附近（≤ 6pt）算命中
bool hit_shape(const Shape &s, ls_point p, bool edge_only);
int shape_handles(const Shape &s, ls_point out[9]);
int shape_handle_cursor(const Shape &s, int handle);
int rect_handle_cursor(int handle);   // 0 左上 1 上 2 右上 3 右 4 右下 5 下 6 左下 7 左
void rect_handle_points(ls_rect r, ls_point out[8]);
// 拖控制点改矩形：对边固定，拉过对边翻转。返回 (x0,y0,x1,y1) 形式的矩形（已规范化）
ls_rect drag_rect_handle(ls_rect orig, int handle, float dx, float dy);

void text_colors(const Shape &s, ls_color *color, ls_color *background, ls_color *outline);
void build_geom(const Shape &s, ShapeGeom &g);
void build_mask_geom(ls_rect selection, const std::vector<Shape> &shapes, ShapeGeom &g);
void build_patch(const Shape &s, ShapeGeom &g, TileCache &tc, float scale, bool drawing);

}  // namespace lsk

// ---------------------------------------------------------------- 会话

enum {
    LSK_DRAG_NONE = 0,
    LSK_DRAG_PENDING, // 按下了，还没超过拖动阈值
    LSK_DRAG_NEW_SEL,
    LSK_DRAG_RESIZE_SEL,
    LSK_DRAG_MOVE_SEL,
    LSK_DRAG_NEW_SHAPE,
    LSK_DRAG_MOVE_SHAPE,
    LSK_DRAG_RESHAPE,
    LSK_DRAG_IGNORE // 这一次按下不再处理（双击、右键放弃等）
};

enum {
    LSK_T_NONE = 0,
    LSK_T_NEW_SEL,
    LSK_T_RESIZE_SEL,
    LSK_T_MOVE_SEL,
    LSK_T_NEW_SHAPE,
    LSK_T_MOVE_SHAPE,
    LSK_T_RESHAPE,
    LSK_T_CLICK // 只认单击（空白处取消选中、多选窗口切换等）
};

struct ls_session {
    ls_session_config cfg{};
    float W = 0, H = 0, scale = 1;

    int phase = LS_PHASE_SELECTING;
    bool has_sel = false;
    ls_rect sel{0, 0, 0, 0};
    std::vector<ls_window> windows;
    std::vector<int64_t> multi; // Shift 多选的窗口，按选择顺序
    bool has_last = false;
    ls_rect last_area{0, 0, 0, 0};

    bool has_hover = false;
    ls_rect hover{0, 0, 0, 0};
    bool hover_is_window = false;
    int64_t hover_id = 0;

    float mx = 0, my = 0;
    bool inside = false;
    int mods = 0;
    int cursor = LS_CURSOR_CROSSHAIR;
    int tool = LS_TOOL_NONE;
    ls_style styles[LS_TOOL_COUNT];
    int ratio_index = 0;
    bool toolbar_visible = true;
    int32_t bindings[LS_BIND_MAX];

    std::vector<lsk::Shape> shapes;
    int32_t next_id = 1;
    uint64_t rev_counter = 0;
    uint64_t shapes_version = 1;
    uint64_t sel_version = 1;
    uint64_t style_version = 1;
    int32_t selected_id = 0, hovered_id = 0, editing_id = 0;

    std::vector<lsk::Snapshot> undo_stack, redo_stack;
    int merge_kind = 0;
    int32_t merge_id = 0;
    float wheel_acc = 0;
    int wheel_kind = 0;
    int32_t wheel_id = 0;

    // 按下与拖动
    bool pressed = false;
    ls_point press_pt{0, 0};
    int press_mods = 0;
    int press_target = LSK_T_NONE;
    int press_handle = -1;
    int32_t press_shape = 0;
    ls_rect press_sel{0, 0, 0, 0};
    lsk::Shape press_orig;
    int press_cursor = LS_CURSOR_ARROW;
    int drag = LSK_DRAG_NONE;
    lsk::Snapshot drag_snap;
    int32_t drawing_id = 0;
    int shift_anchor = -1; // 画笔按住 Shift 拉直线时的起点下标

    // 文字
    ls_measure_text_fn measurer = nullptr;
    void *measurer_ctx = nullptr;
    lsk::Snapshot text_snap;
    bool text_is_new = false;
    bool text_dirty = false;
    std::string text_orig;

    std::deque<int> actions;

    // 绘制缓存（读取函数是 const，缓存可变）
    mutable std::unordered_map<int32_t, std::unique_ptr<lsk::ShapeGeom>> geoms;
    mutable lsk::ShapeGeom mask_geom;
    mutable uint64_t mask_shapes_version = 0, mask_sel_version = 0;
    mutable lsk::TileCache tiles;
};
