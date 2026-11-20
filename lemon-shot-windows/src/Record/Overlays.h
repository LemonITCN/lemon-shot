// 进画面的浮层（spec 8.2）：画笔层、点击扩散圆、按键浮层。都是分层置顶窗口，不排除出画面，
// 由抓屏（DXGI / GDI CAPTUREBLT）自然录进去。摄像头画中画见 Camera.h。
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Record/RecordUI.h"
#include "UI/Gfx.h"
#include "UI/Window.h"

namespace ls {
namespace rec {

// 画笔：区域上的透明画板（底色 alpha = 1/255 才能接收鼠标），颜色取调色板第 1 色，线宽 4。
// Ctrl+Z 撤销一笔，Esc 退出（由调用方关掉并清除笔迹）。
class PenWindow : public Window {
public:
    std::function<void()> onEscape;
    ~PenWindow() override;
    bool Open(RECT region, UINT dpi, Color color);
    void Close();
    bool IsOpen() const { return hwnd_ != nullptr; }

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void FreeDib();
    void RedrawAll();
    void DrawLatest(bool finish);
    void Present(const RECT *dirty);
    RECT DirtyFor(const D2D1_POINT_2F *pts, int n) const;
    D2D1_POINT_2F Mid(D2D1_POINT_2F a, D2D1_POINT_2F b) const { return D2D1_POINT_2F{(a.x + b.x) / 2, (a.y + b.y) / 2}; }

    RECT region_ = {};
    float scale_ = 1;
    UINT dpi_ = 96;
    Color color_;
    Offscreen off_;
    HDC dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void *bits_ = nullptr;
    std::vector<std::vector<D2D1_POINT_2F>> strokes_;
    std::vector<D2D1_POINT_2F> cur_;
    bool drawing_ = false;
    HWND previous_ = nullptr;
};

// 点击扩散圆：每次按下在光标处画一个从 0.3 倍放大到 1 倍、同时淡出的圆（clickOverlay）
class ClickRipples : public Window {
public:
    ~ClickRipples() override;
    bool Init(HMONITOR mon);
    void Add(int x, int y, bool right);
    void Close();

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    struct Ripple : public Window {
        LayeredSurface surface;
        double start = 0;
        bool right = false;
        bool busy = false;
        POINT center = {};
        UINT dpi = 96;
    };
    void Render(Ripple &r, double t);
    RECT mon_ = {};
    UINT dpi_ = 96;
    std::vector<std::unique_ptr<Ripple>> pool_;
};

// 按键浮层：区域底部（record.keys.position）显示最近按键组合，fadeMs 后淡出
class KeysWindow : public Window {
public:
    bool Init(RECT region, UINT dpi);
    void OnKey(UINT vk, int mods, UINT scan, bool ext);
    void Close();
    // 组合文字（Ctrl Alt Shift Win + 键名，空格分隔）；hasModifier：算不算快捷键
    static std::wstring ComboText(UINT vk, int mods, UINT scan, bool ext, bool *hasModifier);

protected:
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    void Tick();
    void Render();
    struct Entry {
        std::wstring text;
        double shownAt;
        float alpha;
    };
    RECT region_ = {};
    UINT dpi_ = 96;
    KeysView view_;
    std::vector<Entry> entries_;
    LayeredSurface surface_;
    int maxKeys_ = 6;
    double fade_ = 1.5;
    bool onlyShortcuts_ = false, hideRepeats_ = true;
    bool timer_ = false;
};

}  // namespace rec
}  // namespace ls
