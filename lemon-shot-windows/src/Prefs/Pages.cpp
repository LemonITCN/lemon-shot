// 偏好设置各页（spec 11），结构与 Mac 端 Prefs/PrefsPages.swift 一致。
#include <shobjidl.h>

#include <cmath>
#include <cstring>

#include "App/App.h"
#include "App/Hotkeys.h"
#include "Capture/Export.h"
#include "Capture/StyleSettings.h"
#include "Pin/Pins.h"
#include "Prefs/Widgets.h"
#include "Record/Devices.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "Translate/Translate.h"
#include "UI/Toast.h"
#include "Util/Api.h"
#include "Util/Util.h"

namespace ls {
namespace prefs {

namespace {

// ---------------------------------------------------------------- 设置绑定

std::string ValueString(const Json *j) {
    if (!j) return std::string();
    if (j->IsString()) return j->AsString();
    if (j->IsBool()) return j->AsBool() ? "true" : "false";
    if (j->IsNumber()) {
        double n = j->AsNumber();
        if (n == std::floor(n)) return Format("%d", (int)n);
        return Format("%g", n);
    }
    return std::string();
}

// 写回时保持原来的类型（数字仍是数字）
Json JsonFor(const std::string &v, const Json *old) {
    if (old && old->IsNumber()) return Json::MakeNumber(atof(v.c_str()));
    if (old && old->IsBool()) return Json::MakeBool(v == "true");
    return Json::MakeString(v);
}

SwitchW *Toggle(const std::string &key, std::function<void(bool)> extra = nullptr) {
    return new SwitchW(settings::Bool(key), [key, extra](bool v) {
        settings::SetBool(key, v);
        if (extra) extra(v);
    });
}

DropdownW *Drop(const std::string &key, const std::vector<Option> &opts, float width = 220) {
    return new DropdownW(opts, ValueString(settings::Get(key)), width,
                         [key](const std::string &v) { settings::Set(key, JsonFor(v, settings::Get(key))); });
}

SegmentedW *Seg(const std::string &key, const std::vector<std::string> &values, const std::vector<std::wstring> &labels) {
    std::string cur = ValueString(settings::Get(key));
    int sel = 0;
    for (size_t i = 0; i < values.size(); ++i)
        if (values[i] == cur) sel = (int)i;
    return new SegmentedW(labels, sel, [key, values](int i) { settings::Set(key, JsonFor(values[(size_t)i], settings::Get(key))); });
}

SliderW *Slide(const std::string &key, double mn, double mx, double step, std::function<std::wstring(double)> fmt,
               float width = 180, double scale = 1) {
    return new SliderW(mn, mx, step, settings::Num(key) * scale, width, fmt, [key, scale](double v) {
        settings::SetNum(key, std::floor(v / scale * 1000.0 + 0.5) / 1000.0);
    });
}

StepperW *Step(const std::string &key, double mn, double mx, double step = 1, double scale = 1, float width = 112,
               std::function<std::wstring(double)> fmt = nullptr) {
    if (!fmt) fmt = [](double v) { return FormatW(L"%d", (int)std::lround(v)); };
    return new StepperW(mn, mx, step, std::floor(settings::Num(key) / scale + 0.5), width, fmt,
                        [key, scale](double v) { settings::SetNum(key, v * scale); });
}

ColorSwatchW *ColorBind(const std::string &key, bool alpha = false) {
    return new ColorSwatchW(settings::Str(key), alpha, [key](const std::string &v) { settings::SetStr(key, v); });
}

PathW *PathBind(const std::string &key) {
    return new PathW(settings::Str(key), 300, [key](const std::string &v) { settings::SetStr(key, v); });
}

TextFieldW *TextBind(const std::string &key, bool secure = false, float width = 260,
                     std::function<void(const std::wstring &)> extra = nullptr) {
    return new TextFieldW(Widen(settings::Str(key)), secure, width, [key, extra](const std::wstring &v) {
        settings::SetStr(key, Narrow(v));
        if (extra) extra(v);
    });
}

std::wstring Ms(double v) { return i18n::T("common.ms", {{"n", FormatW(L"%d", (int)v)}}); }
std::wstring Seconds(double v) { return i18n::T("common.seconds", {{"n", FormatW(L"%d", (int)v)}}); }
std::wstring Percent(double v) { return i18n::T("common.percent", {{"n", FormatW(L"%d", (int)std::lround(v))}}); }
// 长度（逻辑点，用时乘屏幕缩放）
std::wstring Points(double v) { return i18n::T("common.points", {{"n", FormatW(L"%d", (int)v)}}); }

std::vector<Option> Opts(const std::vector<std::string> &values, const std::string &keyPrefix) {
    std::vector<Option> o;
    for (auto &v : values) o.push_back(Option{v, T((keyPrefix + v).c_str()), true});
    return o;
}

// ---------------------------------------------------------------- 文件对话框

bool AskFile(bool save, const wchar_t *defaultName, std::wstring &out) {
    ComPtr<IFileDialog> dlg;
    HRESULT hr = save ? CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, dlg.PutVoid())
                      : CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, dlg.PutVoid());
    if (FAILED(hr)) return false;
    COMDLG_FILTERSPEC f = {L"JSON", L"*.json"};
    dlg->SetFileTypes(1, &f);
    dlg->SetDefaultExtension(L"json");
    if (defaultName) dlg->SetFileName(defaultName);
    if (FAILED(dlg->Show(GetActiveWindow()))) return false;
    ComPtr<IShellItem> item;
    PWSTR p = nullptr;
    if (FAILED(dlg->GetResult(item.Put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}

// ---------------------------------------------------------------- 各页

void General(FormPage *p) {
    p->AddRow(T("pref.general.language"),
              Drop("general.language", {{"system", T("pref.general.language.system"), true},
                                        {"zh-Hans", T("lang.zh-Hans"), true},
                                        {"en", T("lang.en"), true}}));
    p->AddRow(T("pref.general.launchAtLogin"), Toggle("general.launchAtLogin"));
    p->AddRow(T("pref.general.showStatusIcon.win"), Toggle("general.showStatusIcon"));
    p->AddRow(T("pref.general.playSound"), Toggle("general.playSound"));
    p->AddRow(T("pref.general.theme"), Seg("general.theme", {"system", "light", "dark"},
                                           {T("pref.general.theme.system"), T("pref.general.theme.light"),
                                            T("pref.general.theme.dark")}));
    p->AddButtons({
        new ButtonW(T("pref.general.export"), ButtonW::Normal, "download",
                    []() {
                        std::wstring path;
                        if (AskFile(true, L"LemonShot Settings.json", path) && !settings::ExportTo(path))
                            toast::Show(T("common.error"), "triangle-alert");
                    }),
        new ButtonW(T("pref.general.import"), ButtonW::Normal, "folder-open",
                    []() {
                        std::wstring path;
                        if (AskFile(false, nullptr, path) && !settings::ImportFrom(path))
                            toast::Show(T("common.error"), "triangle-alert");
                    }),
        new ButtonW(T("pref.general.reset"), ButtonW::Danger, "rotate-ccw",
                    []() {
                        if (MessageBoxW(GetActiveWindow(), T("pref.general.resetConfirm").c_str(), T("app.name").c_str(),
                                        MB_OKCANCEL | MB_ICONWARNING) == IDOK)
                            settings::Reset(std::string());
                    }),
    });
}

std::vector<Option> ActionOptions(const std::vector<std::string> &keys) { return Opts(keys, "pref.capture.action."); }

DropdownW *OpenWithDropdown() {
    std::string cur = settings::Str("capture.openWithApp");
    std::vector<Option> opts = {{"", T("pref.capture.openWithApp.default"), true},
                                {"folder", T("pref.capture.openWithApp.folder"), true}};
    if (!cur.empty() && cur != "folder") {
        std::wstring name = PathName(Widen(cur));
        size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos) name = name.substr(0, dot);
        opts.push_back({cur, name, true});
    }
    opts.push_back({"__choose__", T("common.choose"), true});
    DropdownW *d = new DropdownW(opts, cur, 220, nullptr);
    d->onChange = [d](const std::string &v) {
        if (v != "__choose__") {
            settings::SetStr("capture.openWithApp", v);
            return;
        }
        ComPtr<IFileOpenDialog> dlg;
        std::string chosen;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, dlg.PutVoid()))) {
            COMDLG_FILTERSPEC f = {L"*.exe", L"*.exe"};
            dlg->SetFileTypes(1, &f);
            ComPtr<IShellItem> item;
            PWSTR p = nullptr;
            if (SUCCEEDED(dlg->Show(GetActiveWindow())) && SUCCEEDED(dlg->GetResult(item.Put())) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                chosen = Narrow(std::wstring(p));
                CoTaskMemFree(p);
            }
        }
        if (!chosen.empty()) {
            settings::SetStr("capture.openWithApp", chosen);  // 页面会重建，列表里出现这个应用
        } else {
            d->value = settings::Str("capture.openWithApp");
            d->Redraw();
        }
    };
    return d;
}

void Capture(FormPage *p) {
    p->AddRow(T("pref.capture.detectWindows"), Toggle("capture.detectWindows"));
    p->AddRow(T("pref.capture.captureCursor"), Toggle("capture.captureCursor"));
    p->AddRow(T("pref.capture.showCrosshair"), Toggle("capture.showCrosshair"));
    p->AddRow(T("pref.capture.showMagnifier"), Toggle("capture.showMagnifier"));
    p->AddRow(T("pref.capture.showTips"), {Toggle("capture.showTips"), ColorBind("capture.tipsColor")});
    p->AddRow(T("pref.capture.rightClickExits"), Toggle("capture.rightClickExits"));
    p->AddRow(T("pref.capture.returnToPreviousApp"), Toggle("capture.returnToPreviousApp"));
    p->AddRow(T("pref.capture.toolbarPopupDelay"), Slide("capture.toolbarPopupDelayMs", 0, 1500, 50, Ms));
    p->AddRow(T("pref.capture.doubleClickAction"),
              Drop("capture.doubleClickAction", ActionOptions({"copy", "save", "pin", "none"})));
    p->AddRow(i18n::T("pref.capture.doubleModifierAction", {{"key", L"Alt"}}),
              Drop("capture.doubleModifierAction", ActionOptions({"copy", "save", "pin", "saveAndOpen", "none"})));
    p->AddRow(T("pref.capture.openWithApp"), OpenWithDropdown());
    Label *x = new Label(L"×", theme::Tok().fontSize.body, "textSecondary");
    x->frame = RectF(0, 0, 10, theme::Tok().window.controlHeight);
    std::wstring pxText = Trim(i18n::T("common.pixels", {{"n", L""}}));
    Label *px = new Label(pxText, theme::Tok().fontSize.body, "textSecondary");
    p->AddRow(T("pref.capture.preset"),
              {Step("capture.presetWidth", 16, 10000, 10), x, Step("capture.presetHeight", 16, 10000, 10), px});
    p->AddRow(T("pref.capture.delay"), Step("capture.delaySeconds", 1, 60, 1, 1, 112, Seconds));
    p->AddRow(T("pref.capture.delaySound"), Toggle("capture.delaySound"));

    p->AddGroup(T("pref.capture.window"));
    // 圆角不设上限（内核按图片短边的一半封顶）；阴影太大没有意义且模糊很慢，到 200
    p->AddRow(T("pref.capture.windowCorners"), {Toggle("capture.windowCorners"), Step("capture.windowCornerRadius", 0, 9999, 1, 1, 136, Points)},
              T("pref.unit.pointsInfo"));
    p->AddRow(T("pref.capture.windowShadow"), {Toggle("capture.windowShadow"), Step("capture.windowShadowRadius", 0, 200, 1, 1, 136, Points)},
              T("pref.unit.pointsInfo"));
    p->AddRow(T("pref.capture.shadowColor"), ColorBind("capture.windowShadowColor", true));

    p->AddGroup(T("pref.capture.multiWindow"));
    p->AddRow(T("pref.capture.windowCorners"), Toggle("capture.multiWindowCorners"));
    p->AddRow(T("pref.capture.windowShadow"), Toggle("capture.multiWindowShadow"));
}

void Annotate(FormPage *p) {
    p->AddRow(T("style.color"), new PaletteEditW());
    for (int t = 1; t <= 10; ++t) {
        p->AddRow(T(style::ToolNameKey(t)), new StyleRowW(t, false));
        if (t == LS_TOOL_MOSAIC) {
            // 马赛克的块大小、笔刷大小两行（一个控件画两行，第二行留空标签）
            StyleRowW *sliders = new StyleRowW(t, true);
            sliders->frame.h = theme::Tok().window.rowHeight * 2 + theme::Tok().window.rowGap;
            p->AddRow(L"", sliders);
        }
    }
}

void Save(FormPage *p) {
    p->AddRow(T("pref.save.enterAction"), Drop("save.enterAction", Opts({"copy", "save", "both"}, "pref.save.enterAction.")));
    p->AddRow(T("pref.save.alsoCopy"), Toggle("save.alsoCopyWhenSaving"));
    p->AddRow(T("pref.save.folder"), PathBind("save.folder"));
    p->AddRow(T("pref.save.askLocation"), Toggle("save.askLocation"));
    // Windows 没有 HEIC
    p->AddRow(T("pref.save.format"), Drop("save.format",
                                          {{"png", L"PNG", true},
                                           {"jpg", L"JPG", true},
                                           {"webp", L"WebP", true},
                                           {"tiff", L"TIFF", true},
                                           {"bmp", L"BMP", true},
                                           {"pdf", L"PDF", true}},
                                          140));
    if (settings::Str("save.format") == "jpg")
        p->AddRow(T("pref.save.jpegQuality"), Slide("save.jpegQuality", 10, 100, 1, Percent));
    p->AddRow(T("pref.save.resolution"), Seg("save.resolution", {"native", "standard"},
                                             {T("pref.save.resolution.native"), T("pref.save.resolution.standard")}));
    Label *example = new Label(L"", theme::Tok().fontSize.caption, "textSecondary");
    auto refresh = [example](const std::wstring &) {
        example->text = exporter::ExpandFileName(settings::Str("save.fileName")) + L"." + Widen(exporter::SaveFormat());
        example->Redraw();
    };
    refresh(L"");
    p->AddRow(T("pref.save.fileName"), TextBind("save.fileName", false, 300, refresh));
    p->AddValue(example);
    // 颜色描述文件：Windows 一律按 sRGB 处理（spec 2.8），这一项在 Windows 上不起作用，不显示
}

void Pin(FormPage *p) {
    p->AddRow(T("pref.pin.corners"), {Toggle("pin.corners"), Step("pin.cornerRadius", 0, 9999, 1, 1, 136, Points)}, T("pref.unit.pointsInfo"));
    p->AddRow(T("pref.pin.shadow"), Toggle("pin.shadow"));
    p->AddRow(T("pref.pin.opacity"), Slide("pin.opacity", 10, 100, 5, Percent, 180, 100));
    p->AddRow(T("pref.pin.doubleClickCloses"), Toggle("pin.doubleClickCloses"));
    // Win7 / 8.1 没有虚拟桌面：不显示；改了立刻作用到已打开的贴图
    if (api::IsWin10OrLater()) p->AddRow(T("pref.pin.allDesktops"), Toggle("pin.allDesktops", [](bool v) { pins::ApplyAllDesktops(v); }));
    p->AddRow(T("pref.pin.showButtons"), Toggle("pin.showButtons"));
    p->AddRow(T("pref.pin.libraryMax"), Step("pin.libraryMax", 10, 500, 10, 1, 112, [](double v) {
                  return i18n::T("common.items", {{"n", FormatW(L"%d", (int)v)}});
              }));
}

std::wstring CustomExample() {
    char buf[128] = {0};
    ls_color_format(settings::Str("color.customFormat").c_str(), 47, 107, 255, settings::Bool("color.hexUppercase") ? 1 : 0,
                    buf, sizeof buf);
    return Widen(std::string(buf));
}

void ColorPage(FormPage *p) {
    p->AddRow(T("pref.color.space"), Drop("color.space", Opts({"native", "srgb", "p3", "adobe", "generic"}, "pref.color.space.")));
    p->AddRow(T("pref.color.hexPrefix"), Toggle("color.hexPrefix"));
    p->AddRow(T("pref.color.hexUppercase"), Toggle("color.hexUppercase"));
    Label *example = new Label(L"", theme::Tok().fontSize.caption, "textSecondary");
    example->text = CustomExample();
    p->AddRow(T("pref.color.customFormat"), std::vector<Widget *>{TextBind("color.customFormat", false, 260,
                                                                           [example](const std::wstring &) {
                                                                               example->text = CustomExample();
                                                                               example->Redraw();
                                                                           })},
              T("pref.color.customFormatHint"));
    p->AddValue(example);
}

const char *kProviders[] = {"microsoft", "baidu", "tencent", "youdao", "deepl", "openai"};
const char *kTargets[] = {"zh-Hans", "zh-Hant", "en", "ja", "ko", "fr", "de", "es", "ru", "pt", "it", "ar", "th", "vi"};

void Ocr(FormPage *p) {
    p->AddGroup(T("ocr.title"));
    p->AddRow(T("pref.ocr.preferQRCode"), Toggle("ocr.preferQRCode"));
    p->AddRow(T("pref.ocr.joinLines"), Toggle("ocr.joinLines"));
    p->AddRow(T("pref.ocr.forceAscii"), Toggle("ocr.forceAsciiPunctuation"));
    p->AddRow(T("pref.ocr.continuous"), Toggle("ocr.continuous"));
    p->AddRow(T("pref.ocr.keepOnTop"), Toggle("ocr.keepOnTop"));
    p->AddRow(T("pref.ocr.closeAfterCopy"), Toggle("ocr.closeAfterCopy"));
    p->AddGroup(T("pref.ocr.translate"));
    std::vector<Option> prov;
    for (auto *k : kProviders) prov.push_back({k, T((std::string("translate.provider.") + k).c_str()), true});
    p->AddRow(T("translate.provider"), Drop("translate.provider", prov, 260));
    std::vector<Option> targets = {{"auto", T("translate.auto"), true}};
    for (auto *k : kTargets) targets.push_back({k, T((std::string("lang.") + k).c_str()), true});
    p->AddRow(T("pref.translate.target"), Drop("translate.target", targets));
    std::string provider = settings::Str("translate.provider");
    struct Field {
        const char *key, *label;
        bool secure;
    };
    std::vector<Field> fields;
    if (provider == "baidu") fields = {{"appId", "pref.translate.appId", false}, {"secret", "pref.translate.secret", true}};
    else if (provider == "tencent")
        fields = {{"secretId", "pref.translate.secretId", false},
                  {"secretKey", "pref.translate.secretKey", true},
                  {"region", "pref.translate.region", false}};
    else if (provider == "youdao")
        fields = {{"appKey", "pref.translate.appKey", false}, {"appSecret", "pref.translate.appSecret", true}};
    else if (provider == "deepl") fields = {{"authKey", "pref.translate.authKey", true}};
    else if (provider == "openai")
        fields = {{"baseUrl", "pref.translate.baseUrl", false},
                  {"apiKey", "pref.translate.apiKey", true},
                  {"model", "pref.translate.model", false}};
    for (auto &f : fields) p->AddRow(T(f.label), TextBind("translate." + provider + "." + f.key, f.secure, 300));
    p->AddRow(L"", new ButtonW(T("translate.test"), ButtonW::Normal, nullptr, [provider]() { translate::TestProvider(provider); }));
}

// 设备列表：「默认」+ 系统里的麦克风/摄像头；设置里存的设备现在不在（拔掉了、从另一台电脑导入）时按默认处理
std::vector<Option> DeviceOptions(bool audio, const char *key) {
    std::vector<Option> out = {{"", T("record.default"), true}};
    std::string cur = settings::Str(key);
    bool found = cur.empty();
    for (auto &d : audio ? rec::Microphones() : rec::Cameras()) {
        out.push_back({d.id, d.name, true});
        if (d.id == cur) found = true;
    }
    if (!found) out.push_back({cur, T("record.default"), true});
    return out;
}

void Record(FormPage *p) {
    p->AddRow(T("pref.record.folder"), PathBind("record.folder"));
    p->AddRow(T("record.format"), Seg("record.format", {"mp4", "gif"}, {L"MP4", L"GIF"}));
    p->AddRow(T("record.quality"), Seg("record.quality", {"ultra", "high", "standard"},
                                       {T("record.quality.ultra"), T("record.quality.high"), T("record.quality.standard")}));
    p->AddRow(T("record.fps"), Seg("record.fps", {"15", "24", "30", "60"}, {L"15", L"24", L"30", L"60"}));
    p->AddRow(T("pref.record.gifFps"), Step("record.gifFps", 5, 30, 1, 1, 112, [](double v) {
                  return i18n::T("common.fps", {{"n", FormatW(L"%d", (int)v)}});
              }));
    p->AddRow(T("pref.record.gifScale"), Seg("record.gifScale", {"1", "0.75", "0.5"},
                                             {Percent(100), Percent(75), Percent(50)}),
              T("pref.record.gifScaleInfo"));
    p->AddRow(T("pref.record.microphoneDevice"), Drop("record.microphoneDevice", DeviceOptions(true, "record.microphoneDevice"), 260));
    p->AddRow(T("record.systemAudio"), Toggle("record.systemAudio"));
    p->AddRow(T("pref.record.cameraDevice"), Drop("record.cameraDevice", DeviceOptions(false, "record.cameraDevice"), 260));
    p->AddRow(T("pref.record.cameraShape"), Seg("record.cameraShape", {"circle", "rounded"},
                                                {T("pref.record.cameraShape.circle"), T("pref.record.cameraShape.rounded")}));
    p->AddRow(T("record.showCursor"), Toggle("record.showCursor"));
    p->AddRow(T("record.showClicks"), Toggle("record.showClicks"));

    p->AddGroup(T("pref.record.keys"));
    p->AddRow(T("record.showKeys"), Toggle("record.showKeys"));
    p->AddRow(T("pref.record.keys.theme"), Seg("record.keys.theme", {"auto", "light", "dark"},
                                               {T("common.auto"), T("pref.general.theme.light"), T("pref.general.theme.dark")}));
    p->AddRow(T("pref.record.keys.size"), Seg("record.keys.size", {"small", "medium", "large"},
                                              {T("pref.record.keys.size.small"), T("pref.record.keys.size.medium"),
                                               T("pref.record.keys.size.large")}));
    p->AddRow(T("pref.record.keys.fade"), Slide("record.keys.fadeMs", 500, 5000, 100, Ms));
    p->AddRow(T("pref.record.keys.max"), Step("record.keys.maxKeys", 1, 10, 1, 1, 112, [](double v) {
                  return i18n::T("common.count", {{"n", FormatW(L"%d", (int)v)}});
              }));
    p->AddRow(T("pref.record.keys.onlyShortcuts"), Toggle("record.keys.onlyShortcuts"));
    p->AddRow(T("pref.record.keys.hideRepeats"), Toggle("record.keys.hideRepeats"));
    p->AddRow(T("pref.record.keys.position"),
              Drop("record.keys.position", Opts({"bottomCenter", "bottomLeft", "bottomRight", "topCenter"},
                                                "pref.record.keys.position.")));

    p->AddGroup(L"");
    p->AddRow(T("record.countdown"), Step("record.countdown", 0, 10, 1, 1, 112, Seconds));
    p->AddRow(T("record.maxDuration"), Step("record.maxDurationSec", 0, 180, 1, 60, 150, [](double v) {
                  return v == 0 ? T("record.unlimited") : i18n::T("common.minutes", {{"n", FormatW(L"%d", (int)v)}});
              }));
    p->AddRow(T("pref.record.afterAction"), Drop("record.afterAction", Opts({"none", "play", "reveal"}, "pref.record.afterAction.")));
}

void Audio(FormPage *p) {
    p->AddRow(T("pref.audio.folder"), PathBind("audio.folder"));
    // MP3：Windows 8+ 才有 Media Foundation 的 MP3 编码器
    p->AddRow(T("pref.audio.format"), Drop("audio.format",
                                           {{"m4a", L"M4A (AAC)", true}, {"wav", L"WAV", true}, {"mp3", L"MP3", api::IsWin8OrLater()}},
                                           160));
    p->AddRow(T("pref.record.microphoneDevice"), Drop("audio.microphoneDevice", DeviceOptions(true, "audio.microphoneDevice"), 260));
    p->AddRow(T("record.systemAudio"), Toggle("audio.systemAudio"));
}

void Shortcuts(FormPage *p) {
    p->AddGroup(T("pref.shortcuts.global"));
    for (auto &id : hotkeys::GlobalIds()) {
        HotkeyW *h = new HotkeyW(hotkeys::Global(id), false, 170, [id](const std::string &v) { hotkeys::SetGlobal(id, v); });
        h->conflict = hotkeys::IsFailed(id) || hotkeys::IsDuplicate(id);
        std::vector<Widget *> row = {h};
        if (hotkeys::IsFailed(id)) row.push_back(new Label(T("pref.shortcuts.conflict"), theme::Tok().fontSize.caption + 1, "danger"));
        p->AddRow(T(("shortcut." + id).c_str()), row);
    }
    p->AddGroup(T("pref.shortcuts.capture"));
    for (auto &id : hotkeys::CaptureIds())
        p->AddRow(T(("shortcut." + id).c_str()),
                  new HotkeyW(hotkeys::CaptureKey(id), true, 110, [id](const std::string &v) { hotkeys::SetCaptureKey(id, v); }));
    p->AddButtons({new ButtonW(T("pref.shortcuts.reset"), ButtonW::Normal, "rotate-ccw", []() { hotkeys::ResetAll(); })});
}

// 麦克风/摄像头隐私开关（Win10+）：任一层为 Deny 视为未授权
bool PrivacyAllowed(const wchar_t *cap) {
    std::wstring base = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\") + cap;
    auto denied = [](HKEY root, const std::wstring &path) {
        wchar_t buf[32] = {0};
        DWORD size = sizeof buf, type = 0;
        if (RegGetValueW(root, path.c_str(), L"Value", RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS) return false;
        return _wcsicmp(buf, L"Deny") == 0;
    };
    return !denied(HKEY_LOCAL_MACHINE, base) && !denied(HKEY_CURRENT_USER, base) &&
           !denied(HKEY_CURRENT_USER, base + L"\\NonPackaged");
}

void Permissions(FormPage *p) {
    p->AddGroup(T("perm.intro"), T("perm.restartHint"));
    struct Kind {
        const char *title, *desc;
        const wchar_t *privacy;  // 有隐私开关的能力
        const wchar_t *settingsUri;
    } kinds[] = {
        {"perm.screen", "perm.screen.desc", nullptr, nullptr},
        {"perm.accessibility", "perm.accessibility.desc", nullptr, nullptr},
        {"perm.input", "perm.input.desc", nullptr, nullptr},
        {"perm.microphone", "perm.microphone.desc", L"microphone", L"ms-settings:privacy-microphone"},
        {"perm.camera", "perm.camera.desc", L"webcam", L"ms-settings:privacy-webcam"},
        {"perm.systemAudio", "perm.systemAudio.desc", nullptr, nullptr},
    };
    for (auto &k : kinds) {
        std::vector<Widget *> row;
        // 自测时固定显示「无需授权」，输出与机器无关
        if (k.privacy && api::IsWin10OrLater() && !settings::IsSelfTest()) {
            bool ok = PrivacyAllowed(k.privacy);
            Label *status = new Label(T(ok ? "perm.granted" : "perm.denied"), theme::Tok().fontSize.body,
                                      ok ? "success" : "textSecondary", kMedium);
            status->frame.w = 80;
            row.push_back(status);
            if (!ok) {
                std::wstring uri = k.settingsUri;
                row.push_back(new ButtonW(T("perm.grant"), ButtonW::Normal, nullptr, [uri]() { ShellOpen(uri); }));
            }
        } else {
            Label *status = new Label(T("perm.notNeeded"), theme::Tok().fontSize.body, "success", kMedium);
            status->frame.w = std::fmax(status->frame.w, 80.f);
            row.push_back(status);
        }
        p->AddRow(T(k.title), row, T(k.desc));
    }
}

void About(FormPage *p) {
    p->AddCustom(new AboutW(), 230);
    CenteredRowW *row = new CenteredRowW();
    auto link = [](const char *key, const char *icon, const wchar_t *url) {
        std::wstring u = url;
        return new ButtonW(T(key), ButtonW::Normal, icon, [u]() { ShellOpen(u); });
    };
    row->Add(link("about.website", "globe", L"https://lemonit.cn"));
    row->Add(link("about.source", "external-link", L"https://github.com/LemonITCN/lemon-shot"));
    row->Add(link("about.license", "scroll-text", L"https://github.com/LemonITCN/lemon-shot/blob/main/LICENSE"));
    row->Add(new ButtonW(T("about.thirdParty"), ButtonW::Normal, "layers", []() {
        std::wstring text = L"Lucide icons — ISC\nlibwebp — BSD-3-Clause\nzxing-cpp — Apache-2.0\n"
                            L"ncnn — BSD-3-Clause\nPaddleOCR models — Apache-2.0";
        MessageBoxW(GetActiveWindow(), text.c_str(), T("about.thirdParty").c_str(), MB_OK | MB_ICONINFORMATION);
    }));
    p->AddCustom(row, theme::Tok().window.controlHeight);
}

}  // namespace

const std::vector<PageInfo> &Pages() {
    static const std::vector<PageInfo> pages = {
        {"general", "settings", "pref.tab.general"},        {"capture", "crop", "pref.tab.capture"},
        {"annotate", "pencil", "pref.tab.annotate"},        {"save", "download", "pref.tab.save"},
        {"pin", "pin", "pref.tab.pin"},                     {"color", "pipette", "pref.tab.color"},
        {"ocr", "scan-text", "pref.tab.ocr"},               {"record", "video", "pref.tab.record"},
        {"audio", "mic", "pref.tab.audio"},                 {"shortcuts", "keyboard", "pref.tab.shortcuts"},
        {"permissions", "shield-check", "pref.tab.permissions"}, {"about", "info", "pref.tab.about"},
    };
    return pages;
}

bool NeedsRebuild(const std::string &page, const std::string &key) {
    if (key.empty() || StartsWith(key, "general.language") || StartsWith(key, "shortcuts")) return true;
    if (page == "save") return key == "save.format";
    if (page == "ocr") return key == "translate.provider";
    if (page == "annotate") return key == "annotate.palette";
    if (page == "capture") return key == "capture.openWithApp";
    return false;
}

FormPage *BuildPage(const std::string &id) {
    const char *titleKey = "pref.tab.general";
    for (auto &p : Pages())
        if (id == p.id) titleKey = p.titleKey;
    FormPage *p = new FormPage(T(titleKey));
    if (id == "general") General(p);
    else if (id == "capture") Capture(p);
    else if (id == "annotate") Annotate(p);
    else if (id == "save") Save(p);
    else if (id == "pin") Pin(p);
    else if (id == "color") ColorPage(p);
    else if (id == "ocr") Ocr(p);
    else if (id == "record") Record(p);
    else if (id == "audio") Audio(p);
    else if (id == "shortcuts") Shortcuts(p);
    else if (id == "permissions") Permissions(p);
    else if (id == "about") About(p);
    return p;
}

}  // namespace prefs
}  // namespace ls
