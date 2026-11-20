#include "OCR/Phase2SelfTest.h"

#include <cmath>
#include <cstring>

#include "LongShot/LongShot.h"
#include "OCR/Engine.h"
#include "OCR/OCR.h"
#include "OCR/ToolUI.h"
#include "Settings/I18n.h"
#include "Shell/Shell.h"
#include "Translate/Translate.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace selftest {

namespace {

// 假数据（测试内容，不是界面文案）
const wchar_t kOcrZh[] = L"Lemon Shot 截图工具\n支持文字识别、截图翻译、长截图和带壳截图。\n识别结果可以直接修改，复制后粘贴到任意位置。\n\n"
                         L"Version 1.0.0 · 2026-11-20\nhttps://lemonit.cn";
const wchar_t kOcrEn[] = L"Lemon Shot screenshot tool\nRecognize text, translate screenshots, capture scrolling pages and add device "
                         L"frames.\nEdit the result here, then paste it anywhere.\n\nVersion 1.0.0 · 2026-11-20\nhttps://lemonit.cn";
const wchar_t kTextEn[] = L"Lemon Shot captures any part of your screen, recognizes the text in it and translates it into your language in "
                          L"one step.";
const wchar_t kTextZh[] = L"Lemon Shot 可以截取屏幕的任意区域，识别其中的文字，并一步翻译成你的语言。";

ls_image CropLogical(const ls_image &src, RectF r, float scale) {
    ls_image out = {};
    int x0 = (int)std::floor(r.x * scale), y0 = (int)std::floor(r.y * scale);
    int x1 = (int)std::ceil(r.r() * scale), y1 = (int)std::ceil(r.b() * scale);
    x0 = Clamp(x0, 0, src.width);
    y0 = Clamp(y0, 0, src.height);
    x1 = Clamp(x1, 0, src.width);
    y1 = Clamp(y1, 0, src.height);
    if (x1 <= x0 || y1 <= y0) return out;
    ls_image_crop(&src, x0, y0, x1 - x0, y1 - y0, &out);
    return out;
}

// 长截图进行中：预览为区域内容竖向拼三份
ls_image LongShotState(const ls_image &screen, float scale) {
    RectF region(200, 150, 640, 400);
    ls_image part = CropLogical(screen, region, scale);
    if (!part.data) return part;
    ls_image page = ls_image_alloc(part.width, part.height * 3);
    if (page.data)
        for (int i = 0; i < 3; ++i) ls_image_draw(&page, &part, 0, i * part.height);
    ls_image_free(&part);
    ls_image out = longshot::RenderSelfTest(scale, screen, 1280, 800, region, page, page.height);
    ls_image_free(&page);
    return out;
}

// 离屏识别检查：渲染一段中英文，跑同一套识别流程，结果写进 ocr-check.txt
void OcrCheck(const std::wstring &dir) {
    const wchar_t *lines[] = {L"Lemon Shot 文字识别自测", L"这是第二行中文，用来检查阅读顺序。",
                              L"The quick brown fox jumps over the lazy dog.", L"价格：128 元（含税）"};
    ls_image img = tool::RenderOffscreen(760, 300, 2, [&](Canvas &c) {
        c.FillRect(RectF(0, 0, 760, 300), Color{1, 1, 1, 1});
        float y = 30;
        for (const wchar_t *l : lines) {
            TextStyle ts;
            ts.size = 22;
            ts.color = Color{0, 0, 0, 1};
            ts.v = VAlign::Top;
            c.Text(l, RectF(30, y, 700, 34), ts);
            y += 50;
        }
    });
    std::string report;
    if (!img.data) {
        report = "error: render\n";
    } else {
        double t0 = NowSeconds();
        ocr::Output o;
        ocr::RecognizeSync(img, o);
        double ms = (NowSeconds() - t0) * 1000.0;
        double t1 = NowSeconds();
        ocr::Output o2;
        ocr::RecognizeSync(img, o2);
        double ms2 = (NowSeconds() - t1) * 1000.0;
        report += Format("ok: %d  error: %d\n", o.ok ? 1 : 0, o.error);
        report += Format("lines: %d  codes: %d  time: %.0f ms (first, 含模型加载), %.0f ms (second)\n", (int)o.lines.size(),
                         (int)o.codes.size(), ms, ms2);
        for (auto &l : o.lines)
            report += Format("  [%.0f,%.0f %.0fx%.0f] %.3f %s\n", l.box.x, l.box.y, l.box.w, l.box.h, l.confidence, l.text.c_str());
        report += "--- layout ---\n" + ocr::LayoutText(o.lines, false, false) + "\n";
        report += "--- joinLines + asciiPunct ---\n" + ocr::LayoutText(o.lines, true, true) + "\n";
        ls_image_free(&img);
    }
    std::wstring path = PathJoin(dir, L"ocr-check.txt");
    WriteFileAtomic(path, report.data(), report.size());
}

}  // namespace

void RunPhase2(const ls_image &screen, float scale, const std::wstring &dir, const std::function<bool(const char *, ls_image &)> &write) {
    bool zh = i18n::IsChinese();
    {
        ls_image thumb = CropLogical(screen, RectF(240, 180, 600, 400), scale);
        ls_image img = ocr::RenderSelfTest(scale, thumb, zh ? kOcrZh : kOcrEn);
        ls_image_free(&thumb);
        write("ocr", img);
    }
    {
        ls_image img = translate::RenderSelfTest(scale, zh ? kTextEn : kTextZh, zh ? kTextZh : kTextEn);
        write("translate", img);
    }
    {
        ls_image img = LongShotState(screen, scale);
        write("longshot", img);
    }
    {
        ls_image img = shell::RenderSelfTest(scale, screen);
        write("shell", img);
    }
    OcrCheck(dir);
}

}  // namespace selftest
}  // namespace ls
