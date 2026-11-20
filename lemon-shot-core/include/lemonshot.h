/*
 * Lemon Shot 公共内核 C 接口
 *
 * Mac（Swift）与 Windows（C++）两端共用。凡是「两端必须结果一致」的逻辑都在这里：
 * 截图选区与标注编辑的交互状态机、标注几何、长截图拼接、马赛克/模糊/阴影等像素处理、
 * GIF 编码、颜色换算与命名、SVG 路径解析。界面绘制、系统捕获、编码、文字输入由各端原生实现。
 *
 * 约定
 * - 坐标：逻辑点（Mac point / Windows DIP），原点在屏幕左上角，y 向下。
 * - 像素图：BGRA8，预乘透明度，按行存储，stride 为每行字节数。
 * - 字符串：UTF-8。由内核返回的指针，在下一次修改同一对象之前一直有效，不需要释放。
 * - 线程：同一个对象只能在同一个线程上使用（各端都在界面线程使用 session）。
 */
#ifndef LEMONSHOT_H
#define LEMONSHOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 基础 */

typedef struct { float x, y; } ls_point;
typedef struct { float x, y, w, h; } ls_rect;
typedef struct { uint8_t r, g, b, a; } ls_color; /* 非预乘 */

typedef struct {
    uint8_t *data;
    int32_t width;
    int32_t height;
    int32_t stride;
} ls_image;

const char *ls_version(void);

/* 分配一张全透明的图；失败时 data 为空。用 ls_image_free 释放。 */
ls_image ls_image_alloc(int32_t width, int32_t height);
void ls_image_free(ls_image *image);

/* ------------------------------------------------------------------ 路径 */

typedef enum {
    LS_PATH_MOVE = 0,  /* pts[0] */
    LS_PATH_LINE = 1,  /* pts[0] */
    LS_PATH_CUBIC = 2, /* pts[0] pts[1] 控制点，pts[2] 终点 */
    LS_PATH_CLOSE = 3
} ls_path_verb;

typedef struct ls_path ls_path;

ls_path *ls_path_create(void);
void ls_path_destroy(ls_path *path);
int32_t ls_path_count(const ls_path *path);
ls_path_verb ls_path_get(const ls_path *path, int32_t index, ls_point pts[3]);

/* 解析 SVG path 的 d 属性（M L H V C S Q T A Z 及小写相对形式）。
 * 结果按 scale 缩放再平移 (dx, dy) 后追加到 out。返回 0 成功，-1 语法错误（已解析部分保留）。 */
int32_t ls_svg_path_parse(const char *d, float scale, float dx, float dy, ls_path *out);

/* ------------------------------------------------------------------ 截图会话：常量 */

typedef enum {
    LS_TOOL_NONE = 0, /* 不画标注：调整选区、选中/移动已有标注 */
    LS_TOOL_RECT = 1,
    LS_TOOL_ELLIPSE = 2,
    LS_TOOL_LINE = 3,
    LS_TOOL_ARROW = 4,
    LS_TOOL_PEN = 5,
    LS_TOOL_MOSAIC = 6,
    LS_TOOL_TEXT = 7,
    LS_TOOL_STEP = 8,
    LS_TOOL_HIGHLIGHT = 9,
    LS_TOOL_MAGNIFY = 10,
    LS_TOOL_COUNT = 11
} ls_tool;

/* 标注形状类型与工具同值；LS_SHAPE_MASK 是内核合成的「局部高亮遮罩」，永远排在最前面画。 */
#define LS_SHAPE_MASK 100

typedef enum {
    LS_PHASE_SELECTING = 0, /* 还没有选区：悬停高亮窗口，拖动框选 */
    LS_PHASE_SELECTED = 1   /* 已有选区：显示工具栏，可调整选区或标注 */
} ls_phase;

enum {
    LS_MOD_SHIFT = 1,
    LS_MOD_CTRL = 2,    /* Windows Ctrl / Mac Control */
    LS_MOD_ALT = 4,     /* Windows Alt / Mac Option */
    LS_MOD_CMD = 8,     /* Mac Command */
    LS_MOD_PRIMARY = 16 /* 平台主修饰键已按下：Mac Command、Windows Ctrl。两端必须同时置这一位 */
};

enum { LS_BUTTON_LEFT = 0, LS_BUTTON_RIGHT = 1, LS_BUTTON_MIDDLE = 2 };

/* 按键：字母用大写 ASCII（'A'..'Z'），数字用 '0'..'9'，空格 32。
 * 两端按「美式键盘的物理键位」换算，切换输入法或键盘布局不影响。 */
enum {
    LS_KEY_ESCAPE = 256,
    LS_KEY_ENTER,
    LS_KEY_TAB,
    LS_KEY_BACKSPACE,
    LS_KEY_DELETE,
    LS_KEY_LEFT,
    LS_KEY_RIGHT,
    LS_KEY_UP,
    LS_KEY_DOWN
};

/* 输入函数的返回值：需要平台做什么（位掩码） */
enum {
    LS_EV_REDRAW = 1 << 0,     /* 重画 */
    LS_EV_CURSOR = 1 << 1,     /* 光标形状变了 */
    LS_EV_SELECTION = 1 << 2,  /* 选区变了（工具栏、尺寸提示要重新摆放） */
    LS_EV_TOOL = 1 << 3,       /* 当前工具或样式变了（工具栏、样式面板要刷新） */
    LS_EV_TEXT_BEGIN = 1 << 4, /* 打开原生文字输入框，见 ls_session_text_edit */
    LS_EV_TEXT_END = 1 << 5,   /* 关闭原生文字输入框 */
    LS_EV_ACTION = 1 << 6,     /* 有待执行的动作，用 ls_session_take_action 取 */
    LS_EV_HISTORY = 1 << 7     /* 撤销/重做可用状态变了 */
};

typedef enum {
    LS_ACTION_NONE = 0,
    LS_ACTION_CANCEL,        /* Esc、右键退出：关闭截图 */
    LS_ACTION_DONE,          /* 回车：按设置 save.enterAction 执行 */
    LS_ACTION_DOUBLE_CLICK,  /* 双击选区：按设置 capture.doubleClickAction 执行 */
    LS_ACTION_COPY,          /* 主修饰键+C */
    LS_ACTION_SAVE,          /* 主修饰键+S */
    LS_ACTION_PIN,
    LS_ACTION_OCR,
    LS_ACTION_TRANSLATE,
    LS_ACTION_LONGSHOT,
    LS_ACTION_RECORD,
    LS_ACTION_COPY_RGB,      /* 复制光标下颜色 */
    LS_ACTION_COPY_HEX,
    LS_ACTION_COPY_CUSTOM,
    LS_ACTION_LAST_AREA      /* 按了「上次区域」但还没有上次区域：平台可提示 */
} ls_action;

/* 截图时单键绑定的功能（键值见 shortcuts.json 的 capture 段） */
typedef enum {
    LS_BIND_TOOL_RECT = 1, /* 与 ls_tool 同值：LS_BIND_TOOL_xxx == LS_TOOL_xxx */
    LS_BIND_LONGSHOT = 20,
    LS_BIND_OCR,
    LS_BIND_TRANSLATE,
    LS_BIND_PIN,
    LS_BIND_RECORD,
    LS_BIND_LAST_AREA,
    LS_BIND_FULLSCREEN,
    LS_BIND_COPY_RGB,
    LS_BIND_COPY_HEX,
    LS_BIND_COPY_CUSTOM,
    LS_BIND_TOGGLE_TOOLBAR,
    LS_BIND_MAX
} ls_binding;

typedef enum {
    LS_CURSOR_ARROW = 0,
    LS_CURSOR_CROSSHAIR,
    LS_CURSOR_MOVE,
    LS_CURSOR_RESIZE_NS,
    LS_CURSOR_RESIZE_EW,
    LS_CURSOR_RESIZE_NWSE,
    LS_CURSOR_RESIZE_NESW,
    LS_CURSOR_IBEAM,
    LS_CURSOR_POINTER /* 可点击：悬停在已有标注上 */
} ls_cursor;

/* ------------------------------------------------------------------ 截图会话：样式 */

/* 每种工具一份样式。不用的字段忽略。 */
typedef struct {
    ls_color color;
    float line_width;    /* 逻辑点：线宽；马赛克画笔=笔刷直径；序号=圆直径；放大镜=边框宽 */
    float opacity;       /* 0.05..1 */
    int32_t filled;      /* 矩形/椭圆实心 */
    int32_t variant;     /* 子样式：见 i18n 的 style.<tool>.<n> */
    float corner_radius; /* 矩形、局部高亮圆角 */
    float font_size;     /* 文字字号（逻辑点） */
    int32_t bold;
    float block_size;    /* 马赛克块大小（逻辑点）、模糊半径 */
    float zoom;          /* 放大镜倍数 */
    int32_t next_number; /* 序号工具下一个数字 */
} ls_style;

/* ------------------------------------------------------------------ 截图会话：创建 */

typedef struct ls_session ls_session;

typedef struct {
    float width, height;   /* 屏幕逻辑尺寸 */
    float scale;           /* 每逻辑点的像素数 */
    ls_image base;         /* 冻结画面；内核只借用，会话结束前平台不得释放或修改 */
    int32_t detect_windows;
    int32_t right_click_exits;
    int32_t lock_selection;        /* 贴图上标注：选区固定为整张图，不能移动/拉伸/重选 */
    int32_t has_initial_selection; /* 预设尺寸、上次区域、全屏等直接给出选区 */
    ls_rect initial_selection;
    float handle_hit;    /* 选区控制点命中尺寸，tokens capture.handleHitSize */
    float edge_hit;      /* 选区边命中宽度，tokens capture.edgeHitSize */
    float min_selection; /* tokens capture.minSelection */
} ls_session_config;

/* 默认配置（命中尺寸等取 tokens.json 默认值），平台再填 base/width/height/scale。 */
ls_session_config ls_session_default_config(void);

ls_session *ls_session_create(const ls_session_config *config);
void ls_session_destroy(ls_session *session);

typedef struct {
    int64_t id;   /* 平台窗口编号（Mac CGWindowID / Windows HWND） */
    ls_rect rect; /* 逻辑点，已换算到本屏坐标 */
} ls_window;

/* 窗口列表：按 z 顺序从最上层到最下层。悬停时选包含光标的最上层窗口；都不包含时高亮整屏。 */
void ls_session_set_windows(ls_session *s, const ls_window *windows, int32_t count);
/* 上次截图区域（本屏坐标）；没有就不调用。 */
void ls_session_set_last_area(ls_session *s, ls_rect rect);
void ls_session_bind_key(ls_session *s, ls_binding binding, int32_t key);

/* ------------------------------------------------------------------ 截图会话：输入 */

int32_t ls_session_mouse_down(ls_session *s, float x, float y, int32_t button, int32_t mods, int32_t click_count);
int32_t ls_session_mouse_move(ls_session *s, float x, float y, int32_t mods); /* 按住或悬停都调这个 */
int32_t ls_session_mouse_up(ls_session *s, float x, float y, int32_t button, int32_t mods);
/* dy > 0 表示向上滚（Mac 未经「自然滚动」反转的物理方向，Windows WHEEL_DELTA 为正）。单位：格（Windows 120 = 1 格，Mac 精确滚动除以 10）。 */
int32_t ls_session_scroll(ls_session *s, float x, float y, float dy, int32_t mods);
int32_t ls_session_key_down(ls_session *s, int32_t key, int32_t mods);
int32_t ls_session_key_up(ls_session *s, int32_t key, int32_t mods);
/* 修饰键单独变化（Shift 按下/松开会改变正在拖动的约束）。 */
int32_t ls_session_modifiers(ls_session *s, int32_t mods);
/* 鼠标离开本屏（多屏时移到别的屏）。 */
int32_t ls_session_mouse_exit(ls_session *s);

/* ------------------------------------------------------------------ 截图会话：命令（工具栏按钮） */

int32_t ls_session_set_tool(ls_session *s, int32_t tool);       /* 再点同一个工具 = 回到 LS_TOOL_NONE */
int32_t ls_session_get_tool(const ls_session *s);
void ls_session_get_style(const ls_session *s, int32_t tool, ls_style *out);
/* 设置某工具的样式；如果当前选中了同类标注，同时改掉它（可撤销）。 */
int32_t ls_session_set_style(ls_session *s, int32_t tool, const ls_style *style);
int32_t ls_session_undo(ls_session *s);
int32_t ls_session_redo(ls_session *s);
int32_t ls_session_delete_selected(ls_session *s);
int32_t ls_session_set_selection(ls_session *s, ls_rect rect);  /* 提示条里改宽高、平台给定区域 */
int32_t ls_session_select_fullscreen(ls_session *s);
int32_t ls_session_set_toolbar_visible(ls_session *s, int32_t visible);
ls_action ls_session_take_action(ls_session *s); /* 依次取出待执行动作，取完返回 LS_ACTION_NONE */

/* ------------------------------------------------------------------ 截图会话：读取状态 */

typedef struct {
    int32_t phase;
    int32_t has_selection;
    ls_rect selection;
    int32_t has_hover;
    ls_rect hover;               /* 悬停高亮（窗口或整屏），只在 SELECTING 阶段 */
    int32_t dragging_selection;  /* 正在拖出或拉伸选区 */
    int32_t dragging_shape;      /* 正在画或移动标注 */
    float mouse_x, mouse_y;
    int32_t mouse_inside;
    int32_t cursor;
    int32_t tool;
    int32_t show_magnifier;      /* 应显示放大镜：选区阶段与拖动选区时 */
    int32_t ratio_index;         /* 0 自由，1..9 见 shortcuts.json ratioLock */
    float ratio;                 /* 锁定比例 w/h，0 表示自由 */
    int32_t toolbar_visible;
    int32_t can_undo, can_redo;
    int32_t selected_shape;      /* 选中标注的 id，没有为 0 */
    int32_t editing_text;        /* 正在编辑文字的标注 id，没有为 0 */
    int32_t window_count;        /* Shift 多选的窗口数，0 表示普通选区 */
    int32_t shape_count;
} ls_session_state;

void ls_session_get_state(const ls_session *s, ls_session_state *out);

/* 选区的像素矩形（四舍五入规则两端一致，导出时用它裁剪）。 */
void ls_session_selection_pixels(const ls_session *s, int32_t *x, int32_t *y, int32_t *w, int32_t *h);
/* 多选窗口的编号，按选择顺序。返回总数。 */
int32_t ls_session_selected_windows(const ls_session *s, int64_t *ids, int32_t capacity);
/* 读冻结画面某个逻辑点下的像素颜色（非预乘）。 */
ls_color ls_session_pixel_at(const ls_session *s, float x, float y);

/* 摆放：传入控件的逻辑尺寸，返回本屏坐标下的位置。两端都用它，位置规则才一致。 */
ls_rect ls_session_place_toolbar(const ls_session *s, float w, float h, float gap, float margin);
ls_rect ls_session_place_size_tip(const ls_session *s, float w, float h, float gap);
ls_rect ls_session_place_magnifier(const ls_session *s, float w, float h, float offset);
/* 样式面板：左边缘 = anchor_x - 16（anchor_x 为工具按钮中心），夹在屏幕内；工具栏在选区下方时放工具栏下方，放不下放上方，反之亦然。 */
ls_rect ls_session_place_popover(const ls_session *s, ls_rect toolbar, float anchor_x, float w, float h, float gap, float margin);

/* ------------------------------------------------------------------ 截图会话：标注绘制数据 */

typedef struct {
    int32_t id;
    int32_t type;        /* ls_tool 或 LS_SHAPE_MASK */
    int32_t selected;
    int32_t hovered;
    int32_t editing;     /* 文字正在原生输入框里编辑：平台不要画它的文字 */
    ls_style style;
    ls_rect bounds;      /* 外接框（含线宽），用于局部重画 */
    ls_point p0, p1;     /* 直线/箭头起止点，矩形/椭圆/高亮/马赛克的对角，放大镜圆心与边缘点 */
    ls_point ctrl;       /* 曲线箭头控制点 */
    const ls_path *fill;   /* 用 fill_color（见结构末尾）按不透明度填充；可空。奇偶填充规则 */
    const ls_path *stroke; /* 用 style.color 按 line_width 描边，圆头圆角；可空 */
    int32_t dashed;        /* stroke 画虚线：线段长 = 3*线宽，间隔 = 2*线宽 */
    /* 文字、序号 */
    const char *text;
    ls_rect text_rect;     /* 文字排版框：文字左上对齐（序号为居中） */
    ls_color text_color;
    ls_color text_background; /* a = 0 表示没有底色 */
    ls_color text_outline;    /* a = 0 表示没有描边；描边宽 = font_size / 8 */
    float text_size;
    /* 马赛克、模糊：已处理好的像素图（已按笔刷形状带透明度），画在 patch_rect */
    const ls_image *patch;
    ls_rect patch_rect;
    /* 放大镜：把冻结画面的 magnify_src（逻辑点）画进以 p0 为圆心、radius 为半径的圆里，再用 stroke 描边 */
    ls_rect magnify_src;
    float radius;
    /* fill 的填充色（非预乘），平台填充 fill 时用它而不是 style.color：
     * 一般等于 style.color；LS_SHAPE_MASK 为黑色；空心序号（序号样式 1）为白色。 */
    ls_color fill_color;
} ls_shape_view;

/* 绘制顺序就是索引顺序。返回的指针在下一次修改会话前有效。 */
int32_t ls_session_shape_count(const ls_session *s);
void ls_session_shape_at(const ls_session *s, int32_t index, ls_shape_view *out);
/* 选中标注的控制点（逻辑点），返回个数（最多 9）。 */
int32_t ls_session_shape_handles(const ls_session *s, ls_point *out, int32_t capacity);

/* ------------------------------------------------------------------ 截图会话：文字 */

/* 平台提供的文字测量：返回逻辑点宽高（多行按 \n 分行，行高由平台字体决定）。 */
typedef void (*ls_measure_text_fn)(void *ctx, const char *utf8, float font_size, int32_t bold, float *out_w, float *out_h);
void ls_session_set_text_measurer(ls_session *s, ls_measure_text_fn fn, void *ctx);

typedef struct {
    int32_t id;
    ls_rect rect;          /* 输入框位置（逻辑点） */
    const char *text;
    float font_size;
    int32_t bold;
    ls_color color;
    ls_color background;   /* a = 0 无 */
    ls_color outline;      /* a = 0 无 */
} ls_text_edit;

/* 收到 LS_EV_TEXT_BEGIN 后读取。返回 0 表示当前没有在编辑。 */
int32_t ls_session_text_edit(const ls_session *s, ls_text_edit *out);
/* 输入框内容每次变化都调用（内核据此重算外接框）。 */
int32_t ls_session_set_text(ls_session *s, const char *utf8);
/* 结束编辑（点外面、Esc、失焦）。内容为空则删除该标注。 */
int32_t ls_session_end_text(ls_session *s);

/* ------------------------------------------------------------------ 像素处理 */

/* 把 src 的像素矩形 (x,y,w,h) 做像素化，块大小 block 像素，结果写入新分配的 out（w×h）。 */
int32_t ls_pixelate(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, int32_t block, ls_image *out);
/* 高斯模糊近似（三次盒式模糊），radius 像素。边缘按夹取处理。 */
int32_t ls_blur(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, float radius, ls_image *out);
/* 裁剪、缩放、旋转、翻转：结果写入新分配的 out。 */
int32_t ls_image_crop(const ls_image *src, int32_t x, int32_t y, int32_t w, int32_t h, ls_image *out);
int32_t ls_image_resize(const ls_image *src, int32_t w, int32_t h, ls_image *out); /* 缩小用面积平均，放大用双线性 */
int32_t ls_image_rotate90(const ls_image *src, int32_t clockwise, ls_image *out);
int32_t ls_image_flip(const ls_image *src, int32_t horizontal, ls_image *out);
/* 圆角 + 投影：radius/shadow_radius/offset_y 均为像素。shadow_radius 按 CSS box-shadow 的模糊半径理解（约 2σ）。
 * 有投影时输出四周各留 pad = ceil(shadow_radius + |shadow_offset_y|) 像素透明边，原图左上角位于 (pad, pad)；
 * 没有投影时不留边。shadow_radius 为 0 只做圆角；radius 为 0 只做投影。 */
int32_t ls_image_frame(const ls_image *src, float radius, float shadow_radius, float shadow_offset_y, ls_color shadow, ls_image *out);
/* 把 src 以 (x,y) 画到 dst 上（source-over，预乘）。 */
void ls_image_draw(ls_image *dst, const ls_image *src, int32_t x, int32_t y);

/* ------------------------------------------------------------------ 长截图拼接 */

typedef struct ls_stitcher ls_stitcher;

typedef enum {
    LS_STITCH_FIRST = 0,     /* 第一帧 */
    LS_STITCH_APPENDED = 1,  /* 接上了，新增 added 行 */
    LS_STITCH_UNCHANGED = 2, /* 画面没动 */
    LS_STITCH_NO_MATCH = 3,  /* 找不到重叠（滚太快或内容变化），这一帧丢弃 */
    LS_STITCH_BACKWARD = 4,  /* 往回滚了，这一帧丢弃 */
    LS_STITCH_MAX = 5        /* 达到最大高度，不再追加 */
} ls_stitch_status;

typedef struct {
    int32_t status;
    int32_t added;        /* 本帧新增的像素行 */
    int32_t total_height; /* 拼接结果总高度（像素） */
    int32_t header;       /* 识别出的固定页眉高度（像素） */
    int32_t footer;       /* 识别出的固定页脚高度（像素） */
} ls_stitch_result;

/* 每一帧必须同宽同高。max_height 为结果最大高度（像素）。 */
ls_stitcher *ls_stitch_create(int32_t max_height);
void ls_stitch_destroy(ls_stitcher *st);
int32_t ls_stitch_push(ls_stitcher *st, const ls_image *frame, ls_stitch_result *out); /* 内核复制帧数据 */
int32_t ls_stitch_height(const ls_stitcher *st);
/* 合成完整结果（页眉只出现一次在顶部，页脚在底部）。out 新分配。 */
int32_t ls_stitch_render(const ls_stitcher *st, ls_image *out);
/* 合成缩略预览，等比缩放到不超过 max_w×max_h。 */
int32_t ls_stitch_preview(const ls_stitcher *st, int32_t max_w, int32_t max_h, ls_image *out);

/* ------------------------------------------------------------------ GIF 编码 */

typedef struct ls_gif ls_gif;

/* path 为 UTF-8 路径；loop 为 0 表示无限循环。 */
ls_gif *ls_gif_create(const char *path, int32_t width, int32_t height, int32_t loop);
/* 追加一帧；delay_cs 为本帧显示时长（百分之一秒）。帧尺寸必须等于创建时的尺寸。 */
int32_t ls_gif_add(ls_gif *gif, const ls_image *frame, int32_t delay_cs);
/* 写文件尾并关闭；返回 0 成功。之后仍需 ls_gif_destroy。 */
int32_t ls_gif_finish(ls_gif *gif);
void ls_gif_destroy(ls_gif *gif);

/* ------------------------------------------------------------------ 颜色 */

typedef enum {
    LS_SPACE_SRGB = 0,
    LS_SPACE_P3 = 1,      /* Display P3 */
    LS_SPACE_ADOBE = 2,   /* Adobe RGB (1998) */
    LS_SPACE_GENERIC = 3  /* Apple 通用 RGB（伽马 1.8） */
} ls_color_space;

/* 0..1 的非线性分量在色彩空间之间换算（D65 白点，Generic 用 D65 适配后的近似）。超出范围会被夹取。 */
void ls_color_convert(int32_t from, int32_t to, const float in[3], float out[3]);
/* 最接近的颜色名称（CSS 命名色，CIE Lab 距离）。lang 为 "zh-Hans" 或 "en"。返回写入的字节数。 */
int32_t ls_color_name(uint8_t r, uint8_t g, uint8_t b, const char *lang, char *buf, int32_t capacity);
/* HEX：prefix 是否带 #，upper 是否大写。 */
int32_t ls_color_hex(uint8_t r, uint8_t g, uint8_t b, int32_t prefix, int32_t upper, char *buf, int32_t capacity);
/* 自定义格式：_R _G _B → 0-255，_r _g _b → 0-1（三位小数），_HEX → 不带 # 的十六进制（按 upper）。 */
int32_t ls_color_format(const char *pattern, uint8_t r, uint8_t g, uint8_t b, int32_t upper, char *buf, int32_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* LEMONSHOT_H */
