#include "Pin/PinLibrary.h"

#include <cmath>
#include <cstring>

#include "App/App.h"
#include "Pin/Pins.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Controls.h"
#include "UI/Window.h"
#include "Util/Api.h"
#include "Util/ImageIO.h"
#include "Util/Json.h"
#include "Util/Log.h"
#include "Util/Util.h"

namespace ls {
namespace pins {

namespace {

const float kWinW = 720, kWinH = 480, kHeader = 44, kPad = 20, kCardW = 160, kCardH = 120, kGap = 12;

std::wstring IndexPath() { return PathJoin(settings::PinsDir(), L"library.json"); }

Json LoadIndex() {
    std::string data;
    Json j;
    if (ReadFileBytes(IndexPath(), data) && Json::Parse(data, j) && j.IsObject()) {
        const Json *items = j.Find("items");
        if (items && items->IsArray()) return j;
    }
    j = Json::MakeObject();
    j.Set("items", Json::MakeArray());
    return j;
}

void SaveIndex(const Json &j) {
    std::string s = j.Dump(true);
    WriteFileAtomic(IndexPath(), s.data(), s.size());
}

std::string HashImage(const ls_image &img) {
    uint64_t h = 1469598103934665603ULL;
    for (int y = 0; y < img.height; ++y) h = Fnv1a64(img.data + (size_t)y * img.stride, (size_t)img.width * 4, h);
    h = Fnv1a64(&img.width, sizeof img.width, h);
    return Format("%08x%08x", (unsigned)(h >> 32), (unsigned)h);
}

}  // namespace

void AddToLibrary(const ls_image &img, const char *source) {
    if (settings::IsSelfTest() || !img.data) return;
    Json idx = LoadIndex();
    Json *items = idx.Find("items");
    std::string hash = HashImage(img);
    Json newItems = Json::MakeArray();
    Json found;
    bool have = false;
    for (size_t i = 0; i < items->Size(); ++i) {
        const Json &it = items->At(i);
        if (!have && it.GetString("hash", "") == hash) {
            found = it;
            have = true;
        } else {
            newItems.Push(it);
        }
    }
    if (!have) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::wstring name = FormatW(L"%04d%02d%02d-%02d%02d%02d-%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                                    t.wSecond, t.wMilliseconds);
        std::wstring path = UniquePath(settings::PinsDir(), name, L"png");
        if (!SaveImageFile(img, path, "png", 0, 1)) {
            LS_LOG("pin: 存入贴图库失败");
            return;
        }
        found = Json::MakeObject();
        found.Set("file", Json::MakeString(Narrow(PathName(path))));
        found.Set("hash", Json::MakeString(hash));
        found.Set("source", Json::MakeString(source ? source : "capture"));
        found.Set("width", Json::MakeNumber(img.width));
        found.Set("height", Json::MakeNumber(img.height));
    }
    found.Set("time", Json::MakeNumber((double)GetTickCount()));
    Json result = Json::MakeArray();
    result.Push(found);
    for (size_t i = 0; i < newItems.Size(); ++i) result.Push(newItems.At(i));
    // 超过上限删最旧
    int max = settings::Int("pin.libraryMax");
    if (max < 10) max = 10;
    Json trimmed = Json::MakeArray();
    for (size_t i = 0; i < result.Size(); ++i) {
        if ((int)i < max) {
            trimmed.Push(result.At(i));
        } else {
            std::wstring f = PathJoin(settings::PinsDir(), Widen(result.At(i).GetString("file", "")));
            DeleteFileW(f.c_str());
        }
    }
    idx.Set("items", trimmed);
    SaveIndex(idx);
}

namespace library {

namespace {

struct Layout {
    int cols = 1;
    float x0 = kPad;
};

Layout Grid(float w) {
    Layout l;
    l.cols = (int)((w - 2 * kPad + kGap) / (kCardW + kGap));
    if (l.cols < 1) l.cols = 1;
    float gridW = l.cols * kCardW + (l.cols - 1) * kGap;
    l.x0 = std::floor((w - gridW) / 2);
    return l;
}

RectF CardRect(float w, int i, float scrollY) {
    Layout l = Grid(w);
    int r = i / l.cols, c = i % l.cols;
    return RectF(l.x0 + c * (kCardW + kGap), kHeader + kPad + r * (kCardH + kGap) - scrollY, kCardW, kCardH);
}

RectF TrashRect(RectF card) { return RectF(card.r() - 30, card.y + 6, 24, 24); }

RectF ClearRect(float w) {
    float bw = ui::ButtonWidth(T("pin.libraryClear"), "trash-2");
    float ch = theme::Tok().window.controlHeight;
    return RectF(w - kPad - bw, (kHeader - ch) / 2, bw, ch);
}

float ContentHeight(float w, int count) {
    Layout l = Grid(w);
    int rows = (count + l.cols - 1) / l.cols;
    return kHeader + kPad * 2 + rows * kCardH + (rows > 0 ? (rows - 1) * kGap : 0);
}

}  // namespace

void Draw(Canvas &c, float w, float h, const std::vector<Thumb> &thumbs, const View &v) {
    const auto &u = theme::Ui();
    c.FillRect(RectF(0, 0, w, h), u.windowBg);
    // 卡片网格（在标题栏下方滚动）
    c.PushClip(RectF(0, kHeader, w, h - kHeader));
    for (int i = 0; i < v.count; ++i) {
        RectF r = CardRect(w, i, v.scrollY);
        if (r.b() < kHeader || r.y > h) continue;
        bool hv = i == v.hover;
        c.FillRound(r, 8, u.surface);
        c.StrokeRound(r.Inflate(-0.5f), 7.5f, hv ? u.accent : u.border, 1);
        if (i < (int)thumbs.size() && thumbs[(size_t)i].bmp) {
            const Thumb &t = thumbs[(size_t)i];
            RectF box = r.Inflate(-8);
            float k = std::fmin(box.w / t.w, box.h / t.h);
            float tw = t.w * k, th = t.h * k;
            RectF dst(box.cx() - tw / 2, box.cy() - th / 2, tw, th);
            c.Bitmap(t.bmp.Get(), dst, nullptr, 1.f, false);
        }
        if (hv) {
            RectF tr = TrashRect(r);
            c.FillRound(tr, 6, v.hoverTrash ? u.danger : theme::Cap().tipBg);
            c.Icon("trash-2", tr.cx(), tr.cy(), 14, Color{1, 1, 1, 1}, 1.75f);
        }
    }
    c.PopClip();
    if (v.count == 0) {
        TextStyle ts = ui::BodyText(u.textSecondary, HAlign::Center);
        c.Icon("images", w / 2, kHeader + (h - kHeader) / 2 - 22, 32, u.textTertiary, 1.5f);
        c.Text(T("pin.libraryEmpty"), RectF(0, kHeader + (h - kHeader) / 2, w, 24), ts);
    }
    // 标题栏：标题 + 数量，右侧「清空」
    c.FillRect(RectF(0, 0, w, kHeader), u.windowBg);
    c.FillRect(RectF(0, kHeader - 1, w, 1), u.divider);
    TextStyle title;
    title.size = theme::Tok().fontSize.title;
    title.weight = kSemibold;
    title.color = u.text;
    float tw = gfx::Measure(T("pin.library"), title.size, kSemibold).w;
    c.Text(T("pin.library"), RectF(kPad, 0, tw + 2, kHeader), title);
    TextStyle cnt = ui::BodyText(u.textSecondary);
    c.Text(i18n::T("pin.libraryCount", {{"n", FormatW(L"%d", v.count)}}), RectF(kPad + tw + 10, 0, 200, kHeader), cnt);
    ui::CtrlState cs;
    cs.hover = v.hoverClear;
    cs.disabled = v.count == 0;
    ui::DrawButton(c, ClearRect(w), T("pin.libraryClear"), false, cs, "trash-2");  // 同 Mac：带 trash-2 图标
}

namespace {

class LibraryWindow : public Window {
public:
    Json index;
    std::vector<Thumb> thumbs;
    View view;
    UINT dpi = 96;
    ComPtr<ID2D1HwndRenderTarget> rt;
    bool tracking = false;

    void Open() {
        index = LoadIndex();
        if (!hwnd_) {
            POINT pt;
            GetCursorPos(&pt);
            HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            dpi = api::MonitorDpi(mon);
            MONITORINFO mi = {};
            mi.cbSize = sizeof mi;
            GetMonitorInfoW(mon, &mi);
            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT rc = {0, 0, (LONG)std::lround(kWinW * dpi / 96.f), (LONG)std::lround(kWinH * dpi / 96.f)};
            api::AdjustRectForDpi(&rc, style, 0, dpi);
            int w = rc.right - rc.left, h = rc.bottom - rc.top;
            int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
            if (!CreateWin(0, style, x, y, w, h, nullptr, T("pin.library").c_str())) return;
            api::SetDarkTitleBar(hwnd_, theme::IsDark());
            HICON big = app::AppIcon(32), small = app::AppIcon(16);
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)big);
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)small);
            dpi = api::WindowDpi(hwnd_);
        }
        ReloadThumbs();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    float S() const { return dpi / 96.f; }
    float W() const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        return rc.right / S();
    }
    float H() const {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        return rc.bottom / S();
    }

    void ReloadThumbs() {
        thumbs.clear();
        const Json *items = index.Find("items");
        view.count = items ? (int)items->Size() : 0;
        thumbs.resize((size_t)view.count);
        float mx = view.scrollY;
        (void)mx;
    }

    void EnsureThumb(int i) {
        if (!rt || i < 0 || i >= (int)thumbs.size() || thumbs[(size_t)i].bmp) return;
        const Json *items = index.Find("items");
        std::wstring file = PathJoin(settings::PinsDir(), Widen(items->At((size_t)i).GetString("file", "")));
        ls_image img;
        if (!LoadImageFile(file, img)) return;
        float s = S();
        float k = std::fmin((kCardW - 16) * s / img.width, (kCardH - 16) * s / img.height);
        if (k > 1) k = 1;
        ls_image small;
        int tw = (int)std::lround(img.width * k), th = (int)std::lround(img.height * k);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        Thumb &t = thumbs[(size_t)i];
        t.w = (float)img.width;
        t.h = (float)img.height;
        if (k < 1 && ls_image_resize(&img, tw, th, &small) == 0 && small.data) {
            t.bmp = gfx::CreateBitmap(rt.Get(), small);
            ls_image_free(&small);
        } else {
            t.bmp = gfx::CreateBitmap(rt.Get(), img);
        }
        ls_image_free(&img);
    }

    void Paint() {
        if (!rt) {
            rt = gfx::CreateHwndTarget(hwnd_, dpi);
            if (!rt) return;
            for (auto &t : thumbs) t.bmp.Reset();
        }
        float w = W(), h = H();
        for (int i = 0; i < view.count; ++i) {
            RectF r = CardRect(w, i, view.scrollY);
            if (r.b() >= kHeader && r.y <= h) EnsureThumb(i);
        }
        rt->BeginDraw();
        {
            Canvas c(rt.Get());
            c.SetTransform(MatIdentity());
            Draw(c, w, h, thumbs, view);
        }
        if (rt->EndDraw(nullptr, nullptr) == (HRESULT)D2DERR_RECREATE_TARGET) {
            rt.Reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    int HitCard(float x, float y) {
        if (y < kHeader) return -1;
        for (int i = 0; i < view.count; ++i)
            if (CardRect(W(), i, view.scrollY).Contains(x, y)) return i;
        return -1;
    }

    void Remove(int i) {
        Json *items = index.Find("items");
        if (!items || i < 0 || i >= (int)items->Size()) return;
        DeleteFileW(PathJoin(settings::PinsDir(), Widen(items->At((size_t)i).GetString("file", ""))).c_str());
        Json rest = Json::MakeArray();
        for (size_t k = 0; k < items->Size(); ++k)
            if ((int)k != i) rest.Push(items->At(k));
        index.Set("items", rest);
        SaveIndex(index);
        thumbs.erase(thumbs.begin() + i);
        view.count = (int)rest.Size();
        view.hover = -1;
        ClampScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ClampScroll() {
        float maxScroll = ContentHeight(W(), view.count) - H();
        if (maxScroll < 0) maxScroll = 0;
        view.scrollY = Clamp(view.scrollY, 0.f, maxScroll);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        float s = S();
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            Paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (rt) {
                D2D1_SIZE_U sz = {LOWORD(lp), HIWORD(lp)};
                rt->Resize(&sz);
            }
            return 0;
        case WM_DPICHANGED: {
            dpi = HIWORD(wp);
            RECT *r = (RECT *)lp;
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (rt) rt->SetDpi((float)dpi, (float)dpi);
            for (auto &t : thumbs) t.bmp.Reset();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!tracking) {
                TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                tracking = true;
            }
            float x = MouseX(lp) / s, y = MouseY(lp) / s;
            int h = HitCard(x, y);
            bool trash = h >= 0 && TrashRect(CardRect(W(), h, view.scrollY)).Contains(x, y);
            bool clear = ClearRect(W()).Contains(x, y);
            if (h != view.hover || trash != view.hoverTrash || clear != view.hoverClear) {
                view.hover = h;
                view.hoverTrash = trash;
                view.hoverClear = clear;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            view.hover = -1;
            view.hoverTrash = view.hoverClear = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            float x = MouseX(lp) / s, y = MouseY(lp) / s;
            if (ClearRect(W()).Contains(x, y) && view.count > 0) {
                if (MessageBoxW(hwnd_, T("pin.libraryClearConfirm").c_str(), T("app.name").c_str(),
                                MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
                    while (view.count > 0) Remove(view.count - 1);
                }
                return 0;
            }
            int i = HitCard(x, y);
            if (i < 0) return 0;
            if (TrashRect(CardRect(W(), i, view.scrollY)).Contains(x, y)) {
                Remove(i);
                return 0;
            }
            const Json *items = index.Find("items");
            std::wstring file = PathJoin(settings::PinsDir(), Widen(items->At((size_t)i).GetString("file", "")));
            ls_image img;
            if (LoadImageFile(file, img)) {
                POINT pt;
                GetCursorPos(&pt);
                float sc = api::MonitorDpi(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)) / 96.f;
                POINT origin = {pt.x - img.width / 2, pt.y - img.height / 2};
                pins::Create(img, sc, origin, "library");
                index = LoadIndex();
                ReloadThumbs();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            view.scrollY -= (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 60.f;
            ClampScroll();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CLOSE: ShowWindow(hwnd_, SW_HIDE); return 0;
        }
        return Window::Handle(msg, wp, lp);
    }
};

LibraryWindow *g_lib = nullptr;

}  // namespace

void Show() {
    if (!g_lib) g_lib = new LibraryWindow();
    g_lib->Open();
}

}  // namespace library
}  // namespace pins
}  // namespace ls
