// 截图翻译（spec 5）。窗口布局与 Mac 端 Translate/TranslateWindow.swift 一致；Windows 用原生标题栏，客户区从语言行开始。
#include "Translate/Translate.h"

#include <windows.h>

#include <cmath>
#include <vector>

#include "Capture/Capture.h"
#include "OCR/Engine.h"
#include "OCR/ToolUI.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "Translate/Http.h"
#include "UI/Toast.h"
#include "Util/Log.h"
#include "Util/Util.h"
#include "lemonshot_extras.h"

namespace ls {
namespace translate {

namespace {

const float kWinW = 560, kWinH = 420, kMinW = 520, kMinH = 360;
const char *kProviders[] = {"microsoft", "baidu", "tencent", "youdao", "deepl", "openai"};
const char *kTargets[] = {"zh-Hans", "zh-Hant", "en", "ja", "ko", "fr", "de", "es", "ru", "pt", "it", "ar", "th", "vi"};

std::vector<prefs::Option> TargetOptions() {
    std::vector<prefs::Option> v = {{"auto", T("translate.auto"), true}};
    for (auto *k : kTargets) v.push_back({k, T((std::string("lang.") + k).c_str()), true});
    return v;
}

std::vector<prefs::Option> ProviderOptions() {
    std::vector<prefs::Option> v;
    for (auto *k : kProviders) v.push_back({k, T((std::string("translate.provider.") + k).c_str()), true});
    return v;
}

std::wstring ProviderName(const std::string &p) { return T((std::string("translate.provider.") + p).c_str()); }

std::wstring NeedKeyMessage(const std::string &provider) { return i18n::T("translate.needKey", {{"provider", ProviderName(provider)}}); }

// ---------------------------------------------------------------- 翻译请求（后台线程）

enum OutcomeKind { kOk = 0, kNeedKey = 1, kFailed = 2 };

struct Outcome {
    int kind = kFailed;
    std::wstring text;  // 译文或错误说明
};

int64_t UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((t - 116444736000000000ULL) / 10000000ULL);
}

// 内核状态机：要发请求就发，直到完成或出错。provider/config/target 都在界面线程里取好。
Outcome Run(const std::string &provider, const std::string &config, const std::string &text, const std::string &target) {
    Outcome out;
    int32_t pid = ls_translate_provider_from_name(provider.c_str());
    if (pid < 0) {
        out.text = Widen(provider);
        return out;
    }
    ls_translator *tr = ls_translate_begin(pid, config.c_str(), text.c_str(), "auto", target.c_str(), UnixNow());
    if (!tr) return out;
    std::wstring netError;
    ls_http_request req;
    int32_t st = ls_translate_request(tr, &req);
    for (int guard = 0; st == LS_TR_SEND && guard < 8; ++guard) {
        std::string body = req.body && req.body_size > 0 ? std::string(req.body, (size_t)req.body_size) : std::string();
        http::Response r = http::Send(req.method ? req.method : "GET", req.url ? req.url : "", req.headers ? req.headers : "", body);
        if (r.status == 0 && !r.error.empty()) netError = r.error;
        ls_translate_response(tr, r.status, r.body.data(), (int32_t)r.body.size());
        st = ls_translate_request(tr, &req);
    }
    std::string result = ls_translate_result(tr) ? ls_translate_result(tr) : "";
    ls_translate_end(tr);
    if (st == LS_TR_DONE) {
        out.kind = kOk;
        out.text = Widen(result);
    } else if (result == "NEED_KEY") {
        out.kind = kNeedKey;
    } else if (result == "HTTP 0" && !netError.empty()) {
        // 没有拿到任何响应（断网、超时、证书）：显示系统给的原因
        out.text = netError;
    } else {
        out.text = Widen(result);
    }
    return out;
}

std::string ConfigJson() {
    const Json *j = settings::Get("translate");
    return j ? j->Dump(false) : std::string("{}");
}

// 后台翻译，回调在界面线程
void TranslateAsync(const std::wstring &text, const std::string &provider, const std::string &target, std::function<void(const Outcome &)> done) {
    tool::InitDispatcher();
    std::string cfg = ConfigJson();
    std::string utf8 = Narrow(text);
    bool started = tool::RunDetached([provider, cfg, utf8, target, done]() {
        Outcome *o = new Outcome(Run(provider, cfg, utf8, target));
        tool::PostToUi([o, done]() {
            if (done) done(*o);
            delete o;
        });
    });
    if (!started) {
        Outcome o;
        o.text = T("common.error");
        if (done) done(o);
    }
}

std::wstring Shorten(const std::wstring &s, size_t n) {
    std::wstring t = ReplaceAll(s, L"\n", L" ");
    if (t.size() > n) t = t.substr(0, n) + L"…";
    return t;
}

// ---------------------------------------------------------------- 窗口内容（屏幕与自测共用）

struct Content {
    prefs::Label *sourceLang;
    prefs::DropdownW *target, *provider;
    prefs::Label *sourceCaption, *resultCaption;
    tool::TextArea *source, *result;
    tool::Status *status;
    prefs::ButtonW *retranslate, *copy;
    RectF arrow, footer;

    explicit Content(prefs::Widget &root) {
        const auto &fs = theme::Tok().fontSize;
        sourceLang = (prefs::Label *)root.Add(new prefs::Label(T("translate.auto"), fs.body, "textSecondary"));
        target = (prefs::DropdownW *)root.Add(new prefs::DropdownW(TargetOptions(), settings::Str("translate.target"), 150, nullptr));
        provider = (prefs::DropdownW *)root.Add(new prefs::DropdownW(ProviderOptions(), settings::Str("translate.provider"), 220, nullptr));
        sourceCaption = (prefs::Label *)root.Add(new prefs::Label(T("translate.source"), fs.caption, "textSecondary", kMedium));
        source = (tool::TextArea *)root.Add(new tool::TextArea());
        resultCaption = (prefs::Label *)root.Add(new prefs::Label(T("translate.result"), fs.caption, "textSecondary", kMedium));
        result = (tool::TextArea *)root.Add(new tool::TextArea());
        status = (tool::Status *)root.Add(new tool::Status());
        retranslate = (prefs::ButtonW *)root.Add(new prefs::ButtonW(T("action.translate")));
        copy = (prefs::ButtonW *)root.Add(new prefs::ButtonW(T("translate.copyResult"), prefs::ButtonW::Primary));
    }

    void Layout(float w, float h) {
        float ch = theme::Tok().window.controlHeight;
        float cy = std::floor((tool::kToolRowH - ch) / 2);
        float lw = std::ceil(gfx::Measure(sourceLang->text, sourceLang->size, kRegular).w) + 2;
        sourceLang->frame = RectF(16, cy, lw, ch);
        arrow = RectF(sourceLang->frame.r() + 6, cy + (ch - 14) / 2, 14, 14);
        target->frame = RectF(arrow.r() + 6, cy, 150, ch);
        provider->frame = RectF(w - 16 - 220, cy, 220, ch);
        float top = tool::kToolRowH;
        float bottom = h - tool::kFooterH - 12;
        float capH = 20;
        float areaH = std::floor((bottom - top - 2 * capH - 10) / 2);
        sourceCaption->frame = RectF(18, top, w - 36, capH);
        source->frame = RectF(16, sourceCaption->frame.b(), w - 32, areaH);
        resultCaption->frame = RectF(18, source->frame.b() + 10, w - 36, capH);
        result->frame = RectF(16, resultCaption->frame.b(), w - 32, bottom - resultCaption->frame.b());
        footer = RectF(0, h - tool::kFooterH, w, tool::kFooterH);
        float left = tool::LayoutRight(footer, {copy, retranslate});
        status->frame = RectF(16, footer.cy() - 10, std::fmax(0.f, left - 12 - 16), 20);
    }

    void DrawChrome(Canvas &c) {
        c.FillRect(RectF(footer.x, footer.y, footer.w, 1), theme::Ui().divider);
        c.Icon("chevron-right", arrow.cx(), arrow.cy(), 14, theme::Ui().textTertiary, 1.75f);
    }
};

// ---------------------------------------------------------------- 窗口

class TranslateWindow;
TranslateWindow *g_win = nullptr;

class TranslateWindow : public tool::ToolWindow {
public:
    Content *ui = nullptr;
    HWND sourceEdit = nullptr, resultEdit = nullptr;
    ~TranslateWindow() override { delete ui; }
    int gen = 0;

    bool Create() {
        ui = new Content(root_);
        if (!Open(T("translate.title"), kWinW, kWinH, kMinW, kMinH, true)) return false;
        sourceEdit = CreateTextEdit(false);
        resultEdit = CreateTextEdit(true);
        ui->target->onChange = [this](const std::string &v) {
            settings::SetStr("translate.target", v);
            RunTranslate();
        };
        ui->provider->onChange = [this](const std::string &v) {
            settings::SetStr("translate.provider", v);
            RunTranslate();
        };
        ui->retranslate->action = [this]() { RunTranslate(); };
        ui->copy->action = [this]() { tool::CopyText(EditText(resultEdit)); };
        ui->source->onActivate = [this]() {
            ui->source->overlay.clear();
            Relayout();
            if (sourceEdit) SetFocus(sourceEdit);
        };
        settings::AddListener(&TranslateWindow::SettingsChanged, this);
        return true;
    }

    static void SettingsChanged(void *ctx, const std::string &path) {
        TranslateWindow *w = (TranslateWindow *)ctx;
        if (!w->hwnd_ || !(path.empty() || StartsWith(path, "translate"))) return;
        w->ui->target->value = settings::Str("translate.target");
        w->ui->provider->value = settings::Str("translate.provider");
        w->Invalidate();
    }

    void Layout(float w, float h) override {
        ui->Layout(w, h);
        PlaceEdit(sourceEdit, ui->source->frame, ui->source->overlay.empty());
        PlaceEdit(resultEdit, ui->result->frame, ui->result->overlay.empty());
    }

    void DrawContent(Canvas &c, float w, float h) override {
        c.Clear(theme::Ui().windowBg);
        ui->DrawChrome(c);
        root_.Draw(c);
    }

    bool OnEditKey(HWND e, UINT vk) override {
        // 原文里 Ctrl+回车：重新翻译
        if (e == sourceEdit && vk == VK_RETURN && GetKeyState(VK_CONTROL) < 0) {
            RunTranslate();
            return true;
        }
        return false;
    }

    void OnEditChange(HWND e) override {
        if (e == sourceEdit && !ui->source->overlay.empty()) {
            ui->source->overlay.clear();
            Relayout();
        }
    }

    void OnClosed() override {
        settings::RemoveListener(&TranslateWindow::SettingsChanged, this);
        ++gen;
        if (g_win == this) g_win = nullptr;
    }

    void SetStatus(const std::wstring &s, bool danger) {
        ui->status->text = s;
        ui->status->icon = danger ? "triangle-alert" : nullptr;
        ui->status->danger = danger;
        Invalidate();
    }

    void FromImage(ls_image img) {
        int my = ++gen;
        SetEditText(sourceEdit, L"");
        SetEditText(resultEdit, L"");
        ui->source->overlay = T("ocr.recognizing");
        ui->result->overlay.clear();
        SetStatus(T("ocr.recognizing"), false);
        Relayout();
        ocr::RecognizeAsync(img, [my](ocr::Output &o) {
            TranslateWindow *w = g_win;
            if (!w || !w->IsOpen() || w->gen != my) return;
            w->ui->source->overlay.clear();
            if (!o.ok) {
                w->Relayout();
                w->SetStatus(i18n::T("ocr.failed", {{"error", ocr::ErrorText(o.error)}}), true);
                return;
            }
            // 翻译时同一段落的行合并，译文更通顺
            std::string text = ocr::LayoutText(o.lines, true, false);
            if (text.empty()) {
                for (size_t i = 0; i < o.codes.size(); ++i) {
                    if (i) text += '\n';
                    text += o.codes[i];
                }
            }
            w->SetEditText(w->sourceEdit, Widen(text));
            if (Trim(Widen(text)).empty()) {
                w->ui->source->overlay = T("ocr.empty");
                w->Relayout();
                w->SetStatus(L"", false);
                return;
            }
            w->Relayout();
            w->RunTranslate();
        });
    }

    void FromText(const std::wstring &text) {
        ++gen;
        ui->source->overlay.clear();
        SetEditText(sourceEdit, text);
        Relayout();
        RunTranslate();
    }

    void RunTranslate() {
        int my = ++gen;
        std::wstring text = EditText(sourceEdit);
        if (Trim(text).empty()) return;
        std::string provider = settings::Str("translate.provider");
        std::string target = settings::Str("translate.target");
        SetEditText(resultEdit, L"");
        ui->result->overlay = T("translate.translating");
        SetStatus(T("translate.translating"), false);
        Relayout();
        TranslateAsync(text, provider, target, [my, provider](const Outcome &o) {
            TranslateWindow *w = g_win;
            if (!w || !w->IsOpen() || w->gen != my) return;
            w->ui->result->overlay.clear();
            if (o.kind == kOk) {
                w->SetEditText(w->resultEdit, o.text);
                w->SetStatus(L"", false);
            } else if (o.kind == kNeedKey) {
                w->SetStatus(NeedKeyMessage(provider), true);
            } else {
                w->SetStatus(i18n::T("translate.failed", {{"error", o.text.empty() ? T("common.error") : o.text}}), true);
            }
            w->Relayout();
        });
    }
};

TranslateWindow *Obtain() {
    if (g_win && g_win->IsOpen()) {
        g_win->Present(true);
        return g_win;
    }
    TranslateWindow *w = new TranslateWindow();
    if (!w->Create()) {
        delete w;
        return nullptr;
    }
    g_win = w;
    w->Present(true);
    return w;
}

}  // namespace

void Start() { capture::Start(capture::Mode::Translate); }

void FromImage(ls_image image, float) {
    if (!image.data) return;
    TranslateWindow *w = Obtain();
    if (!w) {
        ls_image_free(&image);
        return;
    }
    w->FromImage(image);
}

void FromText(const std::wstring &text) {
    TranslateWindow *w = Obtain();
    if (w) w->FromText(text);
}

void TestProvider(const std::string &provider) {
    TranslateAsync(L"Hello", provider, "zh-Hans", [provider](const Outcome &o) {
        if (o.kind == kOk)
            toast::Show(T("translate.testOk"));
        else if (o.kind == kNeedKey)
            toast::Show(NeedKeyMessage(provider), "triangle-alert");
        else
            toast::Show(i18n::T("translate.failed", {{"error", Shorten(o.text.empty() ? T("common.error") : o.text, 60)}}),
                        "triangle-alert");
    });
}

SizeF SelfTestSize() { return SizeF{kWinW, kWinH}; }

ls_image RenderSelfTest(float scale, const std::wstring &sourceText, const std::wstring &resultText) {
    prefs::Widget root;
    Content ui(root);
    ui.source->drawText = true;
    ui.source->text = sourceText;
    ui.result->drawText = true;
    ui.result->text = resultText;
    root.frame = RectF(0, 0, kWinW, kWinH);
    ui.Layout(kWinW, kWinH);
    return tool::RenderOffscreen(kWinW, kWinH, scale, [&](Canvas &c) {
        c.FillRect(RectF(0, 0, kWinW, kWinH), theme::Ui().windowBg);
        ui.DrawChrome(c);
        root.Draw(c);
    });
}

}  // namespace translate
}  // namespace ls
