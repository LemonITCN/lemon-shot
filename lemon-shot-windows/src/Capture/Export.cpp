#include "Capture/Export.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cmath>
#include <cstring>

#include "App/App.h"
#include "Capture/Annotations.h"
#include "Pin/Pins.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Gfx.h"
#include "UI/Toast.h"
#include "Util/ImageIO.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace exporter {

int OutputFromSetting(const std::string &v) {
    if (v == "copy") return OUT_COPY;
    if (v == "save") return OUT_SAVE;
    if (v == "both") return OUT_BOTH;
    if (v == "pin") return OUT_PIN;
    if (v == "saveAndOpen") return OUT_SAVE_OPEN;
    return OUT_NONE;
}

bool RenderSelection(const ls_session *s, const ls_image &base, float scale, ls_image &out) {
    out = ls_image{nullptr, 0, 0, 0};
    int x0, y0, w, h;
    ls_session_selection_pixels(s, &x0, &y0, &w, &h);
    if (w <= 0 || h <= 0) return false;
    Offscreen off;
    if (!off.Create(w, h, scale)) return false;
    TiledBitmap frozen;
    if (!frozen.Create(off.RT(), base)) return false;
    PatchCache patches;
    ls_session_state st;
    ls_session_get_state(s, &st);
    ID2D1RenderTarget *rt = off.RT();
    rt->BeginDraw();
    {
        Canvas c(rt);
        c.Clear(Color{0, 0, 0, 0});
        c.SetTransform(MatTranslate(-x0 / scale, -y0 / scale));
        float lw = base.width / scale, lh = base.height / scale;
        frozen.Draw(c, RectF(0, 0, lw, lh), RectF(0, 0, (float)base.width, (float)base.height), true);
        AnnotationContext ctx;
        ctx.base = &frozen;
        ctx.scale = scale;
        ctx.patches = &patches;
        // 裁剪到选区的像素边界
        DrawAnnotations(c, s, ctx, RectF(x0 / scale, y0 / scale, w / scale, h / scale));
    }
    if (FAILED(rt->EndDraw(nullptr, nullptr))) return false;
    return off.ReadPixels(out);
}

bool RenderWindowSelection(const ls_session *s, const ls_image &base, float scale, const ls_image &win, int fx, int fy, ls_image &out) {
    out = ls_image{nullptr, 0, 0, 0};
    if (!win.data || win.width <= 0 || win.height <= 0) return false;
    int x0, y0, w, h;
    ls_session_selection_pixels(s, &x0, &y0, &w, &h);
    if (w <= 0 || h <= 0) return false;
    Offscreen off;
    if (!off.Create(win.width, win.height, scale)) return false;
    TiledBitmap frozen, body;
    if (!frozen.Create(off.RT(), base) || !body.Create(off.RT(), win)) return false;
    PatchCache patches;
    ID2D1RenderTarget *rt = off.RT();
    rt->BeginDraw();
    {
        Canvas c(rt);
        c.Clear(Color{0, 0, 0, 0});
        body.Draw(c, RectF(0, 0, win.width / scale, win.height / scale), RectF(0, 0, (float)win.width, (float)win.height), true);
        // 标注按冻结画面的坐标画：平移到窗口左上角
        c.SetTransform(MatTranslate(-fx / scale, -fy / scale));
        AnnotationContext ctx;
        ctx.base = &frozen;
        ctx.scale = scale;
        ctx.patches = &patches;
        DrawAnnotations(c, s, ctx, RectF(x0 / scale, y0 / scale, w / scale, h / scale));
    }
    if (FAILED(rt->EndDraw(nullptr, nullptr))) return false;
    return off.ReadPixels(out);
}

std::wstring ExpandFileName(const std::string &tpl) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::wstring in = Widen(tpl), out;
    // 花括号里的 yyyy MM dd HH mm ss 都替换，其他字符原样保留：{yyyy-MM-dd HH.mm.ss} 这种组合也支持
    for (size_t i = 0; i < in.size();) {
        if (in[i] == L'{') {
            size_t e = in.find(L'}', i);
            if (e != std::wstring::npos) {
                std::wstring tok = in.substr(i + 1, e - i - 1);
                std::wstring r;
                for (size_t k = 0; k < tok.size();) {
                    auto take = [&](const wchar_t *pat, int v, int digits) {
                        size_t n = wcslen(pat);
                        if (tok.compare(k, n, pat) == 0) {
                            r += digits == 4 ? FormatW(L"%04d", v) : FormatW(L"%02d", v);
                            k += n;
                            return true;
                        }
                        return false;
                    };
                    if (take(L"yyyy", t.wYear, 4) || take(L"MM", t.wMonth, 2) || take(L"dd", t.wDay, 2) ||
                        take(L"HH", t.wHour, 2) || take(L"mm", t.wMinute, 2) || take(L"ss", t.wSecond, 2))
                        continue;
                    r += tok[k++];
                }
                out += r;
                i = e + 1;
                continue;
            }
        }
        out += in[i++];
    }
    return SanitizeFileName(out);
}

std::wstring SaveFolder() { return settings::ExpandPath(settings::Str("save.folder")); }

std::string SaveFormat() {
    std::string f = settings::Str("save.format");
    // heic Windows 不支持：退回 png（webp 用内核附带的 libwebp，见 ImageIO）
    if (f == "heic" || f.empty()) return "png";
    if (f == "jpeg") return "jpg";
    return f;
}

static bool AskPath(const std::wstring &folder, const std::wstring &name, const std::string &fmt, std::wstring &out) {
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileSaveDialog, dlg.PutVoid())))
        return false;
    std::wstring ext = Widen(fmt);
    std::wstring spec = L"*." + ext;
    std::wstring label = ext;
    for (auto &ch : label) ch = (wchar_t)towupper(ch);
    COMDLG_FILTERSPEC filter = {label.c_str(), spec.c_str()};
    dlg->SetFileTypes(1, &filter);
    dlg->SetDefaultExtension(ext.c_str());
    dlg->SetFileName(name.c_str());
    ComPtr<IShellItem> folderItem;
    if (DirExists(folder) && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_IShellItem, folderItem.PutVoid())))
        dlg->SetFolder(folderItem.Get());
    HWND owner = app::MainHwnd();
    SetForegroundWindow(owner);
    if (FAILED(dlg->Show(owner))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(item.Put()))) return false;
    PWSTR p = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
    out = p;
    CoTaskMemFree(p);
    return true;
}

bool SaveImage(const ls_image &img, float scale, bool ask, std::wstring &path, std::wstring &error) {
    std::string fmt = SaveFormat();
    // WebP 单边最大 16383 像素（长截图常超过）：退回 PNG
    if (fmt == "webp" && (img.width > 16383 || img.height > 16383)) fmt = "png";
    std::wstring folder = SaveFolder();
    std::wstring name = ExpandFileName(settings::Str("save.fileName"));
    if (ask) {
        if (!AskPath(folder, name, fmt, path)) return false;
    } else {
        if (!EnsureDir(folder)) {
            toast::Show(T("notify.folderMissing"), "triangle-alert");
            error.clear();
            return false;
        }
        path = UniquePath(folder, name, Widen(fmt));
    }
    if (!SaveImageFile(img, path, fmt, settings::Int("save.jpegQuality"), scale)) {
        error = FormatW(L"%lu", GetLastError());
        return false;
    }
    return true;
}

static void OpenWithApp(const std::wstring &path) {
    std::string app = settings::Str("capture.openWithApp");
    if (app.empty()) {
        ShellOpen(path);
    } else if (app == "folder") {
        RevealInFolder(path);
    } else {
        std::wstring args = L"\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", Widen(app).c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void Deliver(ls_image img, float scale, int output, POINT originPx) {
    if (!img.data || output == OUT_NONE) {
        ls_image_free(&img);
        return;
    }
    if (output == OUT_PIN) {
        pins::Create(img, scale, originPx, "capture");  // 接管所有权
        return;
    }
    // 分辨率：standard 且 scale > 1 时缩到 1 倍
    float outScale = scale;
    if (settings::Str("save.resolution") == "standard" && scale > 1.01f) {
        ls_image small;
        int w = (int)std::lround(img.width / scale), h = (int)std::lround(img.height / scale);
        if (w > 0 && h > 0 && ls_image_resize(&img, w, h, &small) == 0 && small.data) {
            ls_image_free(&img);
            img = small;
            outScale = 1;
        }
    }
    if (settings::Bool("general.playSound")) app::PlayShutter();
    bool copy = output == OUT_COPY || output == OUT_BOTH;
    bool save = output == OUT_SAVE || output == OUT_BOTH || output == OUT_SAVE_OPEN;
    if (save && settings::Bool("save.alsoCopyWhenSaving")) copy = true;
    bool copied = false;
    if (copy) copied = CopyImageToClipboard(app::MainHwnd(), img);
    if (save) {
        std::wstring path, err;
        bool ask = settings::Bool("save.askLocation");
        if (SaveImage(img, outScale, ask, path, err)) {
            toast::Action a;
            a.kind = toast::kReveal;
            a.path = path;
            toast::Show(T("notify.saved"), "circle-check", a);
            if (output == OUT_SAVE_OPEN) OpenWithApp(path);
        } else if (!err.empty()) {
            toast::Show(i18n::T("notify.saveFailed", {{"error", err}}), "triangle-alert");
        }
    } else if (copied) {
        toast::Show(T("notify.copied"));
    }
    ls_image_free(&img);
}

}  // namespace exporter
}  // namespace ls
