// 长截图结果窗口（spec 6）：宽 = min(图宽, 屏宽 60%)，可滚动查看；复制、保存、贴图、关闭。
// 图可能有几万像素高：只把可见的那几行做成位图（D2D 位图有尺寸上限），图比窗口宽时先按显示宽度缩一份。
#include <cmath>

#include "LongShot/LongShot.h"
#include "OCR/ToolUI.h"
#include "Pin/Pins.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "Util/Api.h"
#include "Util/ImageIO.h"
#include "Util/Util.h"

namespace ls {
namespace longshot {

namespace {

class ResultWindow : public tool::ToolWindow {
public:
    ls_image img = {};    // 原图（复制、保存、贴图用）
    float scale = 1;
    ls_image shown = {};  // 显示用：按显示像素宽度缩小的一份（不需要缩时为空，直接用原图）
    float scrollY = 0;
    RectF imageRect, footer;
    prefs::ButtonW *copy, *save, *pin, *close;
    tool::Status *size;
    // 可见行的位图缓存
    ComPtr<ID2D1Bitmap> slice;
    int sliceY0 = -1, sliceY1 = -1;
    unsigned sliceGen = 0;

    ~ResultWindow() override {
        ls_image_free(&img);
        ls_image_free(&shown);
    }

    bool Create(ls_image image, float sc, HMONITOR mon) {
        img = image;
        scale = sc > 0 ? sc : 1;
        copy = (prefs::ButtonW *)root_.Add(new prefs::ButtonW(T("action.copy"), prefs::ButtonW::Primary));
        save = (prefs::ButtonW *)root_.Add(new prefs::ButtonW(T("action.save")));
        pin = (prefs::ButtonW *)root_.Add(new prefs::ButtonW(T("action.pin")));
        close = (prefs::ButtonW *)root_.Add(new prefs::ButtonW(T("common.close")));
        size = (tool::Status *)root_.Add(new tool::Status());
        size->text = i18n::T("longshot.height", {{"h", FormatW(L"%d", img.height)}});
        copy->action = [this]() { tool::CopyImage(img, scale); };
        save->action = [this]() { tool::SaveImage(img, scale); };
        pin->action = [this]() { PinIt(); };
        close->action = [this]() { Close(); };

        MONITORINFO mi = {};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(mon, &mi);
        float ms = api::MonitorDpi(mon) / 96.f;
        float visW = (mi.rcWork.right - mi.rcWork.left) / ms, visH = (mi.rcWork.bottom - mi.rcWork.top) / ms;
        float logicalW = img.width / scale, logicalH = img.height / scale;
        float w = std::round(std::fmax(420.f, std::fmin(logicalW, visW * 0.6f)));
        float shownH = logicalH * std::fmin(1.f, w / std::fmax(1.f, logicalW));
        float h = std::round(std::fmin(shownH + tool::kFooterH, visH * 0.85f));
        if (h < 300) h = 300;
        return Open(T("longshot.title"), w, h, 420, 300, true, mon);
    }

    void PinIt() {
        // 太高的图贴出来时缩到屏幕高度的 80% 以内
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(mon, &mi);
        float ms = api::MonitorDpi(mon) / 96.f;
        float visH = (mi.rcWork.bottom - mi.rcWork.top) / ms;
        float s = std::fmax(scale, img.height / std::fmax(1.f, visH * 0.8f));
        ls_image copyImg = CloneImage(img);
        if (!copyImg.data) return;
        int pw = (int)std::lround(img.width / s * ms), ph = (int)std::lround(img.height / s * ms);
        POINT origin = {(mi.rcWork.left + mi.rcWork.right - pw) / 2, (mi.rcWork.top + mi.rcWork.bottom - ph) / 2};
        pins::Create(copyImg, s, origin, "longshot");
        Close();
    }

    float ShownW() const { return std::fmin(imageRect.w, img.width / scale); }
    // 显示用图（像素）与它每像素的逻辑长度
    const ls_image &DisplayImage() const { return shown.data ? shown : img; }

    void Layout(float w, float h) override {
        footer = RectF(0, h - tool::kFooterH, w, tool::kFooterH);
        tool::LayoutRight(footer, {copy, save, pin});
        SizeF cs = close->Preferred();
        close->frame = RectF(16, std::floor(footer.cy() - cs.h / 2), cs.w, cs.h);
        SizeF ss = size->Preferred();
        size->frame = RectF(close->frame.r() + 12, footer.cy() - 10, ss.w, 20);
        imageRect = RectF(0, 0, w, footer.y);
        // 图比显示宽：按显示像素宽度缩一份（面积平均）
        int targetW = (int)std::lround(ShownW() * S());
        if (targetW > 0 && targetW < img.width) {
            if (!shown.data || shown.width != targetW) {
                ls_image_free(&shown);
                int th = (int)std::lround((double)img.height * targetW / img.width);
                if (th < 1) th = 1;
                if (ls_image_resize(&img, targetW, th, &shown) != 0) shown = ls_image{};
                slice.Reset();
            }
        } else if (shown.data) {
            ls_image_free(&shown);
            slice.Reset();
        }
        ClampScroll();
    }

    float ContentH() const {
        float sw = ShownW();
        return img.width > 0 ? img.height * (sw / img.width) : 0;
    }

    void ClampScroll() {
        float maxS = ContentH() - imageRect.h;
        if (maxS < 0) maxS = 0;
        scrollY = Clamp(scrollY, 0.f, maxS);
    }

    bool OnWheel(float, float, float dy) override {
        scrollY -= dy * 80.f;
        ClampScroll();
        Invalidate();
        return true;
    }

    void OnTargetReset() override { slice.Reset(); }

    void DrawContent(Canvas &c, float w, float h) override {
        const auto &u = theme::Ui();
        c.Clear(u.windowBg);
        const ls_image &d = DisplayImage();
        float sw = ShownW();
        if (d.data && sw > 0) {
            float k = sw / d.width;  // 每个显示像素的逻辑长度
            float x0 = std::round(imageRect.cx() - sw / 2);
            int py0 = (int)std::floor(scrollY / k), py1 = (int)std::ceil((scrollY + imageRect.h) / k);
            if (py0 < 0) py0 = 0;
            if (py1 > d.height) py1 = d.height;
            if (py1 > py0) {
                if (!slice || py0 != sliceY0 || py1 != sliceY1 || sliceGen != tool::TargetGeneration()) {
                    slice = gfx::CreateBitmap(c.RT(), d.data + (size_t)py0 * d.stride, d.width, py1 - py0, d.stride);
                    sliceY0 = py0;
                    sliceY1 = py1;
                    sliceGen = tool::TargetGeneration();
                }
                c.PushClip(imageRect);
                if (slice) c.Bitmap(slice.Get(), RectF(x0, imageRect.y + py0 * k - scrollY, sw, (py1 - py0) * k), nullptr, 1.f, false);
                c.PopClip();
            }
            // 可滚动时右侧细滚动条
            float ch = ContentH();
            if (ch > imageRect.h + 1) {
                float track = imageRect.h - 8;
                float th = std::fmax(30.f, track * imageRect.h / ch);
                float ty = imageRect.y + 4 + (track - th) * (scrollY / (ch - imageRect.h));
                c.FillRound(RectF(w - 7, ty, 4, th), 2, WithAlpha(u.textTertiary, 0.6f));
            }
        }
        c.FillRect(RectF(footer.x, footer.y, footer.w, 1), u.divider);
        root_.Draw(c);
    }
};

}  // namespace

void ShowResult(ls_image img, float scale, HMONITOR monitor) {
    if (!img.data) return;
    if (!monitor) {
        POINT pt;
        GetCursorPos(&pt);
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }
    ResultWindow *w = new ResultWindow();
    if (!w->Create(img, scale, monitor)) {
        delete w;  // 析构释放 img
        return;
    }
    w->Present(true);
}

}  // namespace longshot
}  // namespace ls
