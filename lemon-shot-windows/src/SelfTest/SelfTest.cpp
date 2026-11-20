// 自测截图（spec 1.1）：固定假数据，WIC 位图渲染目标离屏渲染，文件名即状态名。不创建任何窗口、不读写用户设置。
// 带标注的状态通过真正调用 ls_session_* 构造。场景（假屏幕、假窗口、标注位置、裁剪范围）与 Mac 端 SelfTest.swift 一致，便于并排对照。
#include "SelfTest/SelfTest.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include "App/Hotkeys.h"
#include "App/Tray.h"
#include "Capture/CaptureView.h"
#include "Capture/StyleSettings.h"
#include "Capture/Toolbar.h"
#include "OCR/Phase2SelfTest.h"
#include "Pin/PinLibrary.h"
#include "Pin/Pins.h"
#include "Prefs/Prefs.h"
#include "Record/RecordSelfTest.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Gfx.h"
#include "UI/Toast.h"
#include "Util/ImageIO.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace selftest {

namespace {

const float kScreenW = 1280, kScreenH = 800;
const RectF kSample(200, 150, 640, 400);
// 假窗口（z 序从上到下）：光标 (400,300) 落在第二个窗口里
const RectF kWindows[3] = {RectF(760, 90, 440, 300), RectF(240, 180, 600, 400), RectF(60, 60, 520, 360)};
const char *kAccents[3] = {"#2F6BFF", "#30A46C", "#F5A524"};

float g_scale = 2;
std::wstring g_dir;
int g_failed = 0, g_written = 0;

bool Write(const char *name, ls_image &img) {
    std::wstring path = PathJoin(g_dir, Widen(std::string(name)) + L".png");
    bool ok = img.data && SaveImageFile(img, path, "png", 0, g_scale);
    ls_image_free(&img);
    if (!ok) {
        LS_LOG("selftest: %s 渲染失败", name);
        ++g_failed;
        return false;
    }
    ++g_written;
    return true;
}

// 离屏渲染一张 w×h（逻辑点）的图；失败返回空图
ls_image Render(float w, float h, const std::function<void(Canvas &, ID2D1RenderTarget *)> &draw) {
    ls_image img = {};
    Offscreen off;
    if (!off.Create((int)std::ceil(w * g_scale), (int)std::ceil(h * g_scale), g_scale)) return img;
    ID2D1RenderTarget *rt = off.RT();
    rt->BeginDraw();
    {
        Canvas c(rt);
        c.Clear(Color{0, 0, 0, 0});
        draw(c, rt);
    }
    if (FAILED(rt->EndDraw(nullptr, nullptr))) return img;
    off.ReadPixels(img);
    return img;
}

// 按逻辑矩形裁剪（像素取整到外侧）
ls_image Crop(const ls_image &src, RectF r) {
    ls_image out = {};
    int x0 = (int)std::floor(r.x * g_scale), y0 = (int)std::floor(r.y * g_scale);
    int x1 = (int)std::ceil(r.r() * g_scale), y1 = (int)std::ceil(r.b() * g_scale);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > src.width) x1 = src.width;
    if (y1 > src.height) y1 = src.height;
    if (x1 <= x0 || y1 <= y0) return out;
    ls_image_crop(&src, x0, y0, x1 - x0, y1 - y0, &out);
    return out;
}

// ---------------------------------------------------------------- 假屏幕：灰白渐变 + 三个假窗口

bool MakeFakeScreen(ls_image &out) {
    out = Render(kScreenW, kScreenH, [](Canvas &c, ID2D1RenderTarget *rt) {
        ComPtr<ID2D1GradientStopCollection> stops;
        D2D1_GRADIENT_STOP gs[2] = {{0, D2C(ParseColor("#E4E5EA"))}, {1, D2C(ParseColor("#F7F7F9"))}};
        rt->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, stops.Put());
        ComPtr<ID2D1LinearGradientBrush> grad;
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp = {{0, 0}, {0, kScreenH}};
        if (stops) rt->CreateLinearGradientBrush(&lp, nullptr, stops.Get(), grad.Put());
        D2D1_RECT_F all = {0, 0, kScreenW, kScreenH};
        if (grad) rt->FillRectangle(&all, grad.Get());
        for (int i = 2; i >= 0; --i) {
            RectF r = kWindows[i];
            Color accent = ParseColor(kAccents[i]);
            c.Shadow(r, 10, 24, 8, Color{0, 0, 0, 0.18f});
            c.FillRound(r, 10, Color{1, 1, 1, 1});
            ComPtr<ID2D1RoundedRectangleGeometry> clip;
            D2D1_ROUNDED_RECT rr = {r.D2(), 10, 10};
            gfx::D2D()->CreateRoundedRectangleGeometry(&rr, clip.Put());
            c.PushGeometryMask(clip.Get());
            c.FillRect(RectF(r.x, r.y, r.w, 30), ParseColor("#F0F0F3"));
            const char *dots[3] = {"#FF5F57", "#FEBC2E", "#28C840"};
            for (int j = 0; j < 3; ++j) c.FillEllipse(r.x + 18 + j * 18, r.y + 15, 6, 6, ParseColor(dots[j]));
            c.FillRound(RectF(r.x + 20, r.y + 48, r.w * 0.45f, 18), 4, accent);
            const float widths[5] = {0.92f, 0.78f, 0.86f, 0.6f, 0.7f};
            float y = r.y + 82;
            for (int k = 0; y < r.b() - 30; ++k, y += 22)
                c.FillRound(RectF(r.x + 20, y, (r.w - 40) * widths[k % 5], 10), 5, ParseColor("#D9DBE1"));
            const float bars[5] = {0.5f, 0.8f, 0.35f, 0.65f, 0.9f};
            for (int b = 0; b < 5; ++b) {
                float bh = bars[b] * 70;
                c.FillRound(RectF(r.r() - 150 + b * 24, r.b() - 30 - bh, 16, bh), 3, WithAlpha(accent, 0.75f));
            }
            c.PopLayer();
            c.StrokeRound(r.Inflate(-0.5f), 9.5f, Color{0, 0, 0, 0.12f}, 1);
        }
    });
    if (!out.data) return false;
    for (int y = 0; y < out.height; ++y) {
        uint8_t *p = out.data + (size_t)y * out.stride + 3;
        for (int x = 0; x < out.width; ++x, p += 4) *p = 255;
    }
    return true;
}

// ---------------------------------------------------------------- 会话与场景

ls_session *NewSession(const ls_image &base) {
    ls_session_config cfg = ls_session_default_config();
    cfg.width = kScreenW;
    cfg.height = kScreenH;
    cfg.scale = g_scale;
    cfg.base = base;
    cfg.detect_windows = 1;
    cfg.right_click_exits = 0;
    cfg.handle_hit = theme::Tok().capture.handleHitSize;
    cfg.edge_hit = theme::Tok().capture.edgeHitSize;
    cfg.min_selection = theme::Tok().capture.minSelection;
    ls_session *s = ls_session_create(&cfg);
    ls_window wins[3];
    for (int i = 0; i < 3; ++i) wins[i] = ls_window{(int64_t)(i + 1), kWindows[i].Ls()};
    ls_session_set_windows(s, wins, 3);
    hotkeys::BindSession(s);
    style::ApplyAll(s);
    ls_session_set_text_measurer(s, &gfx::SessionMeasure, nullptr);
    return s;
}

ls_session *SelectedSession(const ls_image &base) {
    ls_session *s = NewSession(base);
    ls_session_mouse_move(s, 400, 300, 0);
    ls_session_set_selection(s, kSample.Ls());
    ls_session_mouse_move(s, 520, 360, 0);
    return s;
}

struct Scene {
    ls_image img = {};
    CaptureUi ui;
};

Scene Draw(ls_session *s, const ls_image &base, CaptureUi ui) {
    Scene sc;
    sc.img = Render(kScreenW, kScreenH, [&](Canvas &c, ID2D1RenderTarget *rt) {
        TiledBitmap frozen;
        frozen.Create(rt, base);
        PatchCache patches;
        CaptureFrame f;
        f.session = s;
        f.frozen = &frozen;
        f.scale = g_scale;
        f.width = kScreenW;
        f.height = kScreenH;
        f.patches = &patches;
        f.opt = CaptureOptions::FromSettings();
        RenderCapture(c, f, ui);
    });
    sc.ui = ui;
    return sc;
}

void DragLine(ls_session *s, float x0, float y0, float x1, float y1, int steps = 6) {
    ls_session_mouse_move(s, x0, y0, 0);
    ls_session_mouse_down(s, x0, y0, LS_BUTTON_LEFT, 0, 1);
    for (int i = 1; i <= steps; ++i) {
        float t = (float)i / steps;
        ls_session_mouse_move(s, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 0);
    }
    ls_session_mouse_up(s, x1, y1, LS_BUTTON_LEFT, 0);
}

void Click(ls_session *s, float x, float y) {
    ls_session_mouse_move(s, x, y, 0);
    ls_session_mouse_down(s, x, y, LS_BUTTON_LEFT, 0, 1);
    ls_session_mouse_up(s, x, y, LS_BUTTON_LEFT, 0);
}

// 先取消选中（无工具时单击选区内空白处），否则改样式会连带改掉刚画好的同类标注
void Use(ls_session *s, int tool, const std::function<void(ls_style &)> &f = nullptr) {
    ls_session_state st;
    ls_session_get_state(s, &st);
    if (st.selected_shape) {
        ls_session_set_tool(s, 0);
        Click(s, 820, 530);
    }
    ls_style style;
    ls_session_get_style(s, tool, &style);
    if (f) f(style);
    ls_session_set_style(s, tool, &style);
    if (ls_session_get_tool(s) != tool) ls_session_set_tool(s, tool);
}

ls_color Pal(int i) { return style::ToLs(style::Palette()[(size_t)i]); }

void Annotate(ls_session *s) {
    // 第一行：矩形、椭圆、直线、放大镜、局部高亮
    Use(s, LS_TOOL_RECT);
    DragLine(s, 220, 172, 330, 245);
    Use(s, LS_TOOL_ELLIPSE, [](ls_style &st) { st.color = Pal(5); });
    DragLine(s, 350, 175, 455, 245);
    Use(s, LS_TOOL_LINE, [](ls_style &st) { st.color = Pal(3); });
    DragLine(s, 475, 240, 560, 180);
    Use(s, LS_TOOL_MAGNIFY);
    DragLine(s, 620, 212, 656, 212);
    Use(s, LS_TOOL_HIGHLIGHT);
    DragLine(s, 690, 172, 820, 250);
    // 第二行：四种箭头、画笔、荧光笔
    for (int v = 0; v < 4; ++v) {
        Use(s, LS_TOOL_ARROW, [v](ls_style &st) {
            st.variant = v;
            st.color = Pal(v == 3 ? 6 : 0);
        });
        float x = 222 + v * 68.f;
        DragLine(s, x, 345, x + 48, 278);
    }
    Use(s, LS_TOOL_PEN, [](ls_style &st) {
        st.variant = 0;
        st.color = Pal(7);
    });
    ls_session_mouse_move(s, 500, 312, 0);
    ls_session_mouse_down(s, 500, 312, LS_BUTTON_LEFT, 0, 1);
    for (int i = 1; i <= 24; ++i) ls_session_mouse_move(s, 500 + i * 5.f, 312 + std::sin(i / 3.f) * 22, 0);
    ls_session_mouse_up(s, 620, 312 + std::sin(8.f) * 22, LS_BUTTON_LEFT, 0);
    Use(s, LS_TOOL_PEN, [](ls_style &st) {
        st.variant = 1;
        st.color = Pal(2);
    });
    DragLine(s, 650, 300, 815, 300, 12);
    // 第三行：三种文字、序号 1-3
    for (int v = 0; v < 3; ++v) {
        Use(s, LS_TOOL_TEXT, [v](ls_style &st) {
            st.variant = v;
            st.font_size = 18;
            st.color = Pal(v == 2 ? 5 : 0);
        });
        Click(s, 225 + v * 140.f, 395);
        ls_text_edit te;
        if (ls_session_text_edit(s, &te)) {
            std::string label = Narrow(T((std::string("style.text.") + (char)('0' + v)).c_str())) + " Lemon";
            ls_session_set_text(s, label.c_str());
            ls_session_end_text(s);
        }
    }
    Use(s, LS_TOOL_STEP, [](ls_style &st) { st.next_number = 1; });
    for (int i = 0; i < 3; ++i) Click(s, 690 + i * 50.f, 395);
    // 第四行：像素马赛克
    Use(s, LS_TOOL_MOSAIC, [](ls_style &st) {
        st.variant = 0;
        st.block_size = 10;
    });
    DragLine(s, 250, 455, 470, 535);
    // 选中矩形：回到无工具，单击矩形的边
    ls_session_set_tool(s, 0);
    Click(s, 220, 210);
    ls_session_mouse_move(s, 600, 500, 0);
}

bool MakeLibraryThumbs(const ls_image &screen, std::vector<ls_image> &out) {
    const RectF regions[6] = {RectF(240, 180, 600, 400), RectF(760, 90, 440, 300), RectF(60, 60, 520, 360),
                              RectF(250, 220, 200, 300), RectF(0, 0, 1280, 800), RectF(900, 200, 300, 120)};
    for (auto &r : regions) {
        ls_image im = Crop(screen, r);
        if (im.data) out.push_back(im);
    }
    return !out.empty();
}

}  // namespace

int Run(const std::wstring &dir, float scale) {
    g_scale = scale > 0 ? scale : 2;
    g_dir = dir;
    if (!EnsureDir(dir)) {
        LS_LOG("selftest: 无法创建输出目录");
        return 4;
    }
    double t0 = NowSeconds();
    ls_image screen;
    if (!MakeFakeScreen(screen)) {
        LS_LOG("selftest: 假屏幕生成失败");
        return 5;
    }

    // 截图层
    {
        ls_session *s = NewSession(screen);
        ls_session_mouse_move(s, 400, 300, 0);
        Scene sc = Draw(s, screen, CaptureUi());
        Write("capture-selecting", sc.img);
        ls_session_destroy(s);
    }
    {
        ls_session *s = SelectedSession(screen);
        Scene sc = Draw(s, screen, CaptureUi());
        Write("capture-selected", sc.img);
        ls_session_destroy(s);
    }
    {
        ls_session *s = SelectedSession(screen);
        Annotate(s);
        Scene sc = Draw(s, screen, CaptureUi());
        Write("capture-annotated", sc.img);
        ls_session_destroy(s);
    }
    for (int t = 1; t <= 10; ++t) {
        ls_session *s = SelectedSession(screen);
        ls_session_set_tool(s, t);
        Scene sc = Draw(s, screen, CaptureUi());
        RectF r = sc.ui.hasToolbar ? sc.ui.toolbar.rect : RectF(0, 0, kScreenW, kScreenH);
        if (sc.ui.hasPopover) {
            RectF p = sc.ui.popoverRect;
            float x0 = std::fmin(r.x, p.x), y0 = std::fmin(r.y, p.y);
            r = RectF(x0, y0, std::fmax(r.r(), p.r()) - x0, std::fmax(r.b(), p.b()) - y0);
        }
        ls_image cropped = Crop(sc.img, r.Inflate(24));
        ls_image_free(&sc.img);
        std::string name = std::string("popover-") + style::ToolKey(t);
        Write(name.c_str(), cropped);
        ls_session_destroy(s);
    }
    {
        ls_session *s = SelectedSession(screen);
        CaptureUi ui;
        ui.hoverBtn = TB_SAVE;
        ui.tooltipBtn = TB_SAVE;
        Scene sc = Draw(s, screen, ui);
        RectF r = sc.ui.hasToolbar ? sc.ui.toolbar.rect : RectF(0, 0, kScreenW, kScreenH);
        if (sc.ui.hasToolbar) {
            RectF b = sc.ui.toolbar.ButtonRect(TB_SAVE);
            RectF t = ToolbarTooltipRect(b, ToolbarTooltip(TB_SAVE), sc.ui.toolbarBelow, kScreenW, kScreenH);
            float x0 = std::fmin(r.x, t.x), y0 = std::fmin(r.y, t.y);
            r = RectF(x0, y0, std::fmax(r.r(), t.r()) - x0, std::fmax(r.b(), t.b()) - y0);
        }
        ls_image cropped = Crop(sc.img, r.Inflate(24));
        ls_image_free(&sc.img);
        Write("toolbar-hover", cropped);
        ls_session_destroy(s);
    }

    // 贴图：截图里的一块，带按钮（第 2 个悬停）、圆角、阴影，背后是假屏幕
    {
        ls_image pic = Crop(screen, RectF(230, 170, 420, 280));
        RectF sz = pins::PinPreviewSize(420, 280);
        ls_image bg = Crop(screen, RectF(600, 300, sz.w, sz.h));
        ls_image img = Render(sz.w, sz.h, [&](Canvas &c, ID2D1RenderTarget *rt) {
            if (bg.data) {
                ComPtr<ID2D1Bitmap> b = gfx::CreateBitmap(rt, bg);
                c.Bitmap(b.Get(), RectF(0, 0, sz.w, sz.h), nullptr);
            }
            ComPtr<ID2D1Bitmap> p = gfx::CreateBitmap(rt, pic);
            pins::DrawPinPreview(c, p.Get(), 420, 280, true, true, 1);
        });
        ls_image_free(&pic);
        ls_image_free(&bg);
        Write("pin", img);
    }
    // 贴图库：6 张假图（截图里的几块），第 2 张悬停
    {
        std::vector<ls_image> thumbs;
        MakeLibraryThumbs(screen, thumbs);
        ls_image img = Render(720, 480, [&](Canvas &c, ID2D1RenderTarget *rt) {
            std::vector<pins::library::Thumb> list;
            for (auto &im : thumbs) {
                pins::library::Thumb t;
                t.bmp = gfx::CreateBitmap(rt, im);
                t.w = (float)im.width;
                t.h = (float)im.height;
                list.push_back(t);
            }
            pins::library::View v;
            v.count = (int)list.size();
            v.hover = 1;
            pins::library::Draw(c, 720, 480, list, v);
        });
        for (auto &im : thumbs) ls_image_free(&im);
        Write("pin-library", img);
    }
    // 文字识别、翻译、长截图、带壳（另写 ocr-check.txt：离屏跑一遍真实识别流程）
    RunPhase2(screen, g_scale, g_dir, [](const char *name, ls_image &img) { return Write(name, img); });
    // 录屏设置面板、控制条、按键浮层、录音面板
    rec::SelfTest(
        screen, g_scale, kScreenW, kScreenH, [&]() { return SelectedSession(screen); },
        [](float w, float h, const rec::DrawFn &draw) { return Render(w, h, draw); },
        [](const char *name, ls_image &img) { return Write(name, img); });
    // 提示：四周留 24pt，背后是假屏幕
    {
        std::wstring text = T("notify.copied");
        RectF ts = toast::Measure(text);
        float w = ts.w + 48, h = ts.h + 48;
        ls_image bg = Crop(screen, RectF(300, 260, w, h));
        ls_image img = Render(w, h, [&](Canvas &c, ID2D1RenderTarget *rt) {
            if (bg.data) {
                ComPtr<ID2D1Bitmap> b = gfx::CreateBitmap(rt, bg);
                c.Bitmap(b.Get(), RectF(0, 0, w, h), nullptr);
            }
            toast::Draw(c, RectF(24, 24, ts.w, ts.h), text, "circle-check");
        });
        ls_image_free(&bg);
        Write("toast", img);
    }
    // 托盘菜单（自绘一份，结构同系统菜单）
    {
        RectF ms = tray::MenuPreviewSize();
        ls_image img = Render(ms.w + 48, ms.h + 48, [&](Canvas &c, ID2D1RenderTarget *) {
            c.FillRect(RectF(0, 0, ms.w + 48, ms.h + 48), theme::Ui().windowBg);
            tray::DrawMenuPreview(c, RectF(24, 24, ms.w, ms.h));
        });
        Write("menu", img);
    }
    // 偏好设置每一页
    for (auto &page : prefs::PageIds()) {
        SizeF sz = prefs::PageSize(page);
        ls_image img = Render(sz.w, sz.h, [&](Canvas &c, ID2D1RenderTarget *) { prefs::RenderPage(c, page); });
        std::string name = "prefs-" + page;
        Write(name.c_str(), img);
    }
    ls_image_free(&screen);
    LS_LOG("selftest: 输出 %d 张到目录，失败 %d 项，用时 %.1f 秒", g_written, g_failed, NowSeconds() - t0);
    return g_failed ? 1 : 0;
}

}  // namespace selftest
}  // namespace ls
