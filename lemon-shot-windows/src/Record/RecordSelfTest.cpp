#include "Record/RecordSelfTest.h"

#include <cmath>

#include "Audio/Audio.h"
#include "Capture/CaptureView.h"
#include "Record/Hooks.h"
#include "Record/Overlays.h"
#include "Record/RecordUI.h"
#include "Record/SetupPanel.h"
#include "Settings/Settings.h"
#include "Util/ComPtr.h"

namespace ls {
namespace rec {

namespace {

// 按逻辑矩形裁假屏幕（像素取整到外侧）
ls_image CropScreen(const ls_image &src, float scale, RectF r) {
    ls_image out = {};
    int x0 = (int)std::floor(r.x * scale), y0 = (int)std::floor(r.y * scale);
    int x1 = (int)std::ceil(r.r() * scale), y1 = (int)std::ceil(r.b() * scale);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > src.width) x1 = src.width;
    if (y1 > src.height) y1 = src.height;
    if (x1 <= x0 || y1 <= y0) return out;
    ls_image_crop(&src, x0, y0, x1 - x0, y1 - y0, &out);
    return out;
}

void DrawBackground(Canvas &c, ID2D1RenderTarget *rt, const ls_image &bg, float w, float h) {
    if (!bg.data) return;
    ComPtr<ID2D1Bitmap> b = gfx::CreateBitmap(rt, bg);
    c.Bitmap(b.Get(), RectF(0, 0, w, h), nullptr);
}

}  // namespace

void SelfTest(const ls_image &screen, float scale, float screenW, float screenH, const std::function<ls_session *()> &selected,
              const RenderFn &render, const WriteFn &write) {
    // record-setup：录屏模式截图层，选区 (200,150,640,400)，下方设置面板，左下角提示
    {
        ls_session *s = selected();
        ls_image img = render(screenW, screenH, [&](Canvas &c, ID2D1RenderTarget *rt) {
            TiledBitmap frozen;
            frozen.Create(rt, screen);
            PatchCache patches;
            SetupPanel panel(nullptr, scale, (UINT)std::lround(96 * scale), nullptr, nullptr);
            CaptureFrame f;
            f.session = s;
            f.frozen = &frozen;
            f.scale = scale;
            f.width = screenW;
            f.height = screenH;
            f.patches = &patches;
            f.opt = CaptureOptions::FromSettings();
            f.opt.recordMode = true;
            CaptureUi ui;
            ui.recordPanel = &panel;
            RenderCapture(c, f, ui);
        });
        write("record-setup", img);
        ls_session_destroy(s);
    }
    // record-bar：控制条（计时 00:12），连同上方区域下边缘的红色虚线边框
    {
        BarView v;
        v.clock = L"00:12";
        SizeF bs = v.Size();
        const float pad = 24;
        float w = bs.w + 2 * pad, h = bs.h + 2 * pad + 20;
        ls_image bg = CropScreen(screen, scale, RectF(300, 330, w, h));
        ls_image img = render(w, h, [&](Canvas &c, ID2D1RenderTarget *rt) {
            DrawBackground(c, rt, bg, w, h);
            float bw = theme::Tok().recordBar.regionBorderWidth;
            ComPtr<ID2D1StrokeStyle> dash;
            const float dashes[2] = {3.f, 2.f};  // 6/4（以线宽 2 为单位）
            D2D1_STROKE_STYLE_PROPERTIES p = {D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER,
                                              10.f, D2D1_DASH_STYLE_CUSTOM, 0.f};
            gfx::D2D()->CreateStrokeStyle(&p, dashes, 2, dash.Put());
            c.Line(0, 12 + bw / 2, w, 12 + bw / 2, theme::Tok().recordBar.regionBorder, bw, dash.Get());
            v.Draw(c, RectF(pad, pad + 20, bs.w, bs.h));
        });
        ls_image_free(&bg);
        write("record-bar", img);
    }
    // record-keys：按键浮层 Ctrl Shift 4，底部居中、中号、外观跟随主题，画布 520×200
    {
        const float w = 520, h = 200;
        KeysView v;
        v.position = "bottomCenter";
        v.size = "medium";
        v.dark = theme::IsDark();
        v.items.push_back({KeysWindow::ComboText('4', kModCtrl | kModShift, 0, false, nullptr), 1.f});
        ls_image bg = CropScreen(screen, scale, RectF(260, 200, w, h));
        ls_image img = render(w, h, [&](Canvas &c, ID2D1RenderTarget *rt) {
            DrawBackground(c, rt, bg, w, h);
            v.Draw(c, RectF(0, 0, w, h));
        });
        ls_image_free(&bg);
        write("record-keys", img);
    }
    // audio：录音面板（录音中 00:07，有电平）
    {
        SizeF ps = audio::PanelSize();
        const float pad = 24;
        float w = ps.w + 2 * pad, h = ps.h + 2 * pad;
        audio::PanelView v;
        v.mic = settings::Bool("audio.microphone");
        v.system = true;
        v.recording = true;
        v.clock = L"00:07";
        v.level = audio::Meter(0.18f);
        std::string fmt = settings::Str("audio.format");
        v.format = fmt == "wav" ? L"WAV" : (fmt == "mp3" ? L"MP3" : L"M4A");
        ls_image bg = CropScreen(screen, scale, RectF(840, 60, w, h));
        ls_image img = render(w, h, [&](Canvas &c, ID2D1RenderTarget *rt) {
            DrawBackground(c, rt, bg, w, h);
            audio::DrawPanel(c, RectF(pad, pad, ps.w, ps.h), v);
        });
        ls_image_free(&bg);
        write("audio", img);
    }
}

}  // namespace rec
}  // namespace ls
