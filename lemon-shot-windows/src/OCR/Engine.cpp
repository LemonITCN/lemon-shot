#include "OCR/Engine.h"

#include <windows.h>

#include <cstring>

#include "OCR/ToolUI.h"
#include "Settings/I18n.h"
#include "Util/Log.h"
#include "Util/Util.h"
#include "lemonshot_extras.h"
#include "../../res/resource.h"

namespace ls {
namespace ocr {

namespace {

// 只在后台线程上访问
tool::SerialQueue *g_queue = nullptr;  // 进程内常驻，不释放
ls_ocr *g_engine = nullptr;
bool g_engineTried = false;

bool ResourceSpan(int id, const void *&data, size_t &size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    if (!g) return false;
    data = LockResource(g);
    size = SizeofResource(nullptr, r);
    return data && size > 0;
}

ls_ocr *Engine() {
    if (g_engineTried) return g_engine;
    g_engineTried = true;
    double t0 = NowSeconds();
    const void *dp = nullptr, *db = nullptr, *rp = nullptr, *rb = nullptr, *dict = nullptr;
    size_t dps = 0, dbs = 0, rps = 0, rbs = 0, dicts = 0;
    if (!ResourceSpan(IDR_OCR_DET_PARAM, dp, dps) || !ResourceSpan(IDR_OCR_DET_BIN, db, dbs) ||
        !ResourceSpan(IDR_OCR_REC_PARAM, rp, rps) || !ResourceSpan(IDR_OCR_REC_BIN, rb, rbs) ||
        !ResourceSpan(IDR_OCR_DICT, dict, dicts)) {
        LS_LOG("ocr: 模型资源缺失");
        return nullptr;
    }
    g_engine = ls_ocr_create(dp, dps, db, dbs, rp, rps, rb, rbs, (const char *)dict, dicts, 0);
    LS_LOG("ocr: 模型加载%s，用时 %.0f ms", g_engine ? "完成" : "失败", (NowSeconds() - t0) * 1000.0);
    return g_engine;
}

void Run(const ls_image &img, Output &o) {
    double t0 = NowSeconds();
    if (char *codes = ls_barcode_decode(&img)) {
        for (const std::string &c : Split(std::string(codes), '\n')) {
            if (c.empty()) continue;
            bool dup = false;
            for (auto &e : o.codes)
                if (e == c) dup = true;
            if (!dup) o.codes.push_back(c);
        }
        ls_free(codes);
    }
    double t1 = NowSeconds();
    ls_ocr *eng = Engine();
    if (!eng) {
        o.error = kErrModel;
        o.ok = !o.codes.empty();
        return;
    }
    int32_t n = ls_ocr_run(eng, &img);
    if (n < 0) {
        // 图太小（内核要求至少 4×4）：当作没有文字
        if (img.width < 4 || img.height < 4) {
            o.ok = true;
            return;
        }
        o.error = kErrRun;
        o.ok = !o.codes.empty();
        return;
    }
    for (int32_t i = 0; i < n; ++i) {
        ls_text_line l;
        ls_ocr_line_at(eng, i, &l);
        Line line;
        line.text = l.text ? l.text : "";
        line.box = l.box;
        line.confidence = l.confidence;
        if (!line.text.empty()) o.lines.push_back(line);
    }
    o.ok = true;
    LS_LOG("ocr: %dx%d 识别 %d 行、%d 个码，二维码 %.0f ms，文字 %.0f ms", img.width, img.height, (int)o.lines.size(),
           (int)o.codes.size(), (t1 - t0) * 1000.0, (NowSeconds() - t1) * 1000.0);
}

}  // namespace

void RecognizeAsync(ls_image img, std::function<void(Output &)> done) {
    tool::InitDispatcher();
    if (!g_queue) g_queue = tool::SerialQueue::Create();
    if (!g_queue) {
        ls_image_free(&img);
        Output o;
        o.error = kErrRun;
        if (done) done(o);
        return;
    }
    g_queue->Post([img, done]() mutable {
        Output *o = new Output();
        Run(img, *o);
        ls_image_free(&img);
        tool::PostToUi([o, done]() {
            if (done) done(*o);
            delete o;
        });
    });
}

void RecognizeSync(const ls_image &img, Output &out) {
    out = Output();
    Run(img, out);
}

std::string LayoutText(const std::vector<Line> &lines, bool joinLines, bool asciiPunct) {
    if (lines.empty()) return std::string();
    std::vector<ls_text_line> arr(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        arr[i].text = lines[i].text.c_str();
        arr[i].box = lines[i].box;
        arr[i].confidence = lines[i].confidence;
    }
    int32_t flags = (joinLines ? LS_TEXT_JOIN_LINES : 0) | (asciiPunct ? LS_TEXT_ASCII_PUNCT : 0);
    char *p = ls_text_layout(arr.data(), (int32_t)arr.size(), flags);
    if (!p) return std::string();
    std::string s(p);
    ls_free(p);
    return s;
}

static std::string JoinCodes(const std::vector<std::string> &codes) {
    std::string s;
    for (size_t i = 0; i < codes.size(); ++i) {
        if (i) s += '\n';
        s += codes[i];
    }
    return s;
}

bool ShowsCodes(const Output &o, bool preferQR) { return !o.codes.empty() && (preferQR || o.lines.empty()); }

std::string Compose(const Output &o, bool joinLines, bool asciiPunct, bool preferQR) {
    if (preferQR && !o.codes.empty()) return JoinCodes(o.codes);
    std::string t = LayoutText(o.lines, joinLines, asciiPunct);
    if (t.empty()) t = JoinCodes(o.codes);
    return t;
}

bool IsLink(const std::string &s) {
    std::string t = s;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n' || t.back() == '\r' || t.back() == '\t')) t.pop_back();
    size_t b = 0;
    while (b < t.size() && (t[b] == ' ' || t[b] == '\t')) ++b;
    t = t.substr(b);
    if (t.find('\n') != std::string::npos || t.find(' ') != std::string::npos) return false;
    std::string low = ToLower(t);
    size_t hostStart = 0;
    if (low.compare(0, 7, "http://") == 0) hostStart = 7;
    else if (low.compare(0, 8, "https://") == 0) hostStart = 8;
    else return false;
    // 要有主机名
    return t.size() > hostStart && t[hostStart] != '/' && t[hostStart] != '?' && t[hostStart] != '#';
}

std::wstring ErrorText(int error) {
    if (error == kErrModel) return T("ocr.modelFailed");
    return T("common.error");
}

}  // namespace ocr
}  // namespace ls
