// 偏好设置的自绘控件与排版（与 Mac 端 Prefs/Form.swift、UI/Controls.swift 逐项对应）。
// 坐标为内容区逻辑点；屏幕窗口与自测共用同一份绘制代码（自测时 host 的窗口为空）。
#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "UI/Gfx.h"

namespace ls {
namespace prefs {

class Widget;

// 控件的宿主：偏好设置窗口实现；自测时为离屏宿主
class Host {
public:
    virtual ~Host() = default;
    virtual void Invalidate() = 0;
    virtual HWND Hwnd() = 0;
    // 内容区逻辑矩形 → 屏幕像素矩形（弹出层定位用）
    virtual RECT ScreenRect(RectF r) = 0;
    virtual UINT Dpi() = 0;
    // 原生输入框（支持输入法）：done = true 表示结束
    virtual void BeginEdit(RectF r, const std::wstring &text, bool secure, bool center,
                           std::function<void(const std::wstring &, bool done)> cb) = 0;
    virtual void EndEdit() = 0;
    virtual void SetFocusWidget(Widget *w) = 0;
    virtual Widget *FocusWidget() = 0;
    virtual void RequestRebuild() = 0;
};

class Widget {
public:
    RectF frame;
    bool hidden = false;
    bool hovered = false;
    bool pressed = false;
    bool enabled = true;
    Host *host = nullptr;
    std::vector<std::unique_ptr<Widget>> children;

    virtual ~Widget() = default;
    virtual SizeF Preferred() { return SizeF{frame.w, frame.h}; }
    virtual void Layout() {}
    virtual void Draw(Canvas &c) {
        for (auto &ch : children)
            if (!ch->hidden) ch->Draw(c);
    }
    virtual bool Interactive() { return false; }
    virtual Widget *HitTest(float x, float y);
    virtual void OnMouseDown(float, float) {}
    virtual void OnMouseDrag(float, float) {}
    virtual void OnMouseUp(float, float, bool) {}
    virtual void OnMouseMove(float, float) {}
    virtual void OnMouseLeave() {}
    virtual bool OnWheel(float) { return false; }
    virtual bool AcceptsFocus() { return false; }
    virtual void OnFocus(bool) {}
    virtual bool OnKey(UINT, bool) { return false; }
    virtual LPCWSTR Cursor() { return MAKEINTRESOURCEW(32512); }
    void SetHost(Host *h);
    void Redraw() {
        if (host) host->Invalidate();
    }
    Widget *Add(Widget *w) {
        children.emplace_back(w);
        return w;
    }
};

// ---------------------------------------------------------------- 基础控件

class Label : public Widget {
public:
    std::wstring text;
    float size;
    int weight;
    Color *colorOverride = nullptr;
    const char *colorKey;  // "text" "textSecondary" "danger" "success" "textTertiary"
    Label(const std::wstring &t, float fontSize, const char *colorKey_, int w = kRegular);
    void Draw(Canvas &c) override;
    static Color KeyColor(const char *key);
};

class Gap : public Widget {
public:
    Gap(float w, float h) { frame = RectF(0, 0, w, h); }
};

class ButtonW : public Widget {
public:
    enum Kind { Normal, Primary, Danger };
    std::wstring title;
    Kind kind;
    const char *icon;
    std::function<void()> action;
    ButtonW(const std::wstring &t, Kind k = Normal, const char *ic = nullptr, std::function<void()> a = nullptr);
    SizeF Preferred() override;
    bool Interactive() override { return enabled; }
    void Draw(Canvas &c) override;
    void OnMouseUp(float, float, bool inside) override;
};

class SwitchW : public Widget {
public:
    bool on;
    std::function<void(bool)> onChange;
    SwitchW(bool v, std::function<void(bool)> cb);
    SizeF Preferred() override;
    bool Interactive() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseUp(float, float, bool inside) override;
};

class SegmentedW : public Widget {
public:
    std::vector<std::wstring> items;
    int selected;
    int hoverIndex = -1;
    std::function<void(int)> onChange;
    SegmentedW(const std::vector<std::wstring> &it, int sel, std::function<void(int)> cb);
    SizeF Preferred() override;
    bool Interactive() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseMove(float x, float y) override;
    void OnMouseLeave() override;
    void OnMouseUp(float x, float y, bool inside) override;
    RectF Seg(int i);
};

struct Option {
    std::string value;
    std::wstring label;
    bool enabled = true;
};

class DropdownW : public Widget {
public:
    std::vector<Option> options;
    std::string value;
    std::function<void(const std::string &)> onChange;
    bool open = false;
    double closedAt = 0;
    DropdownW(const std::vector<Option> &o, const std::string &v, float width, std::function<void(const std::string &)> cb);
    bool Interactive() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseUp(float, float, bool inside) override;
    std::wstring CurrentLabel();
};

class SliderW : public Widget {
public:
    double minV, maxV, step, value;
    float trackWidth, valueWidth = 52;
    std::function<std::wstring(double)> format;
    std::function<void(double)> onChange;
    SliderW(double mn, double mx, double st, double v, float trackW, std::function<std::wstring(double)> fmt,
            std::function<void(double)> cb);
    SizeF Preferred() override;
    bool Interactive() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseDown(float x, float y) override { SetFrom(x); }
    void OnMouseDrag(float x, float y) override { SetFrom(x); }
    bool OnWheel(float dy) override;
    RectF Track();
    void SetFrom(float x);
};

class StepperW : public Widget {
public:
    double minV, maxV, step, value;
    std::function<std::wstring(double)> format;
    std::function<void(double)> onChange;
    int hotPart = 0;
    bool editing = false;
    StepperW(double mn, double mx, double st, double v, float width, std::function<std::wstring(double)> fmt,
             std::function<void(double)> cb);
    bool Interactive() override { return enabled; }
    void Draw(Canvas &c) override;
    void OnMouseMove(float x, float y) override;
    void OnMouseLeave() override {
        hotPart = 0;
        Redraw();
    }
    void OnMouseUp(float x, float y, bool inside) override;
    bool OnWheel(float dy) override;
    void Bump(double d);
    RectF Minus() { return RectF(frame.x, frame.y, 28, frame.h); }
    RectF Plus() { return RectF(frame.r() - 28, frame.y, 28, frame.h); }
    RectF ValueRect() { return RectF(frame.x + 28, frame.y, frame.w - 56, frame.h); }
};

class TextFieldW : public Widget {
public:
    std::wstring text, placeholder;
    bool secure;
    bool editing = false;
    std::function<void(const std::wstring &)> onChange;
    TextFieldW(const std::wstring &t, bool sec, float width, std::function<void(const std::wstring &)> cb);
    bool Interactive() override { return true; }
    LPCWSTR Cursor() override { return MAKEINTRESOURCEW(32513); }
    void Draw(Canvas &c) override;
    void OnMouseUp(float, float, bool inside) override;
};

class InfoW : public Widget {
public:
    std::wstring text;
    explicit InfoW(const std::wstring &t) : text(t) { frame = RectF(0, 0, 18, 18); }
    bool Interactive() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseMove(float, float) override;
    void OnMouseLeave() override;
};

class ColorSwatchW : public Widget {
public:
    std::string hex;
    bool alpha;
    std::function<void(const std::string &)> onChange;
    ColorSwatchW(const std::string &h, bool allowAlpha, std::function<void(const std::string &)> cb);
    bool Interactive() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseUp(float, float, bool inside) override;
};

class PathW : public Widget {
public:
    std::string path;
    std::function<void(const std::string &)> onChange;
    ButtonW *button;
    PathW(const std::string &p, float width, std::function<void(const std::string &)> cb);
    void Layout() override;
    void Draw(Canvas &c) override;
    RectF Box();
    static std::wstring ShortName(const std::string &raw);
};

class HotkeyW : public Widget {
public:
    std::string shortcut;
    bool conflict = false;
    bool singleKey;
    bool recording = false;
    bool invalid = false;
    UINT liveMods = 0;
    std::function<void(const std::string &)> onChange;
    HotkeyW(const std::string &s, bool single, float width, std::function<void(const std::string &)> cb);
    bool Interactive() override { return true; }
    bool AcceptsFocus() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseUp(float x, float y, bool inside) override;
    void OnFocus(bool f) override;
    bool OnKey(UINT vk, bool down) override;
    RectF ClearRect() { return RectF(frame.r() - 26, frame.cy() - 10, 20, 20); }
    void Stop();
};

// 调色板 10 色逐个可改
class PaletteEditW : public Widget {
public:
    int hover = -1;
    PaletteEditW();
    bool Interactive() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseMove(float x, float y) override;
    void OnMouseLeave() override {
        hover = -1;
        Redraw();
    }
    void OnMouseUp(float x, float y, bool inside) override;
    RectF Swatch(int i) { return RectF(frame.x + i * 32.f, frame.cy() - 12, 24, 24); }
};

// 一个工具的样式控件（与截图样式面板第 1 行一致），写回 annotate.<tool>
class StyleRowW : public Widget {
public:
    int tool;
    bool secondRow;  // 马赛克的滑块行
    int hover = -1;
    int pressedItem = -1;
    StyleRowW(int t, bool second);
    SizeF Preferred() override;
    bool Interactive() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseMove(float x, float y) override;
    void OnMouseLeave() override {
        hover = -1;
        Redraw();
    }
    void OnMouseDown(float x, float y) override;
    void OnMouseDrag(float x, float y) override;
    void OnMouseUp(float, float, bool) override;
};

class AboutW : public Widget {
public:
    void Draw(Canvas &c) override;
};

class CenteredRowW : public Widget {
public:
    void Layout() override;
};

// 应用图标（矢量自绘）
void DrawAppIcon(Canvas &c, RectF r);

// ---------------------------------------------------------------- 排版（页标题 + 分组 + 左标签右控件）

class FormPage : public Widget {
public:
    enum Kind { Group, Row, Value, Buttons, Custom };
    struct Item {
        Kind kind;
        std::wstring text;
        std::vector<Widget *> controls;
        InfoW *info = nullptr;
        float height = 0;
    };
    std::wstring title;
    std::vector<Item> items;

    explicit FormPage(const std::wstring &t) : title(t) {}
    void AddGroup(const std::wstring &t, const std::wstring &info = std::wstring());
    void AddRow(const std::wstring &label, std::vector<Widget *> controls, const std::wstring &info = std::wstring());
    void AddRow(const std::wstring &label, Widget *control, const std::wstring &info = std::wstring()) {
        AddRow(label, std::vector<Widget *>{control}, info);
    }
    void AddValue(Label *l);
    void AddButtons(std::vector<Widget *> bs);
    void AddCustom(Widget *w, float h);
    float LayoutPage(float width);  // 返回内容高度
    void Draw(Canvas &c) override;

private:
    struct Text {
        std::wstring t;
        RectF r;
    };
    std::vector<Text> labels_, groups_;
    std::vector<RectF> dividers_;
};

// ---------------------------------------------------------------- 弹出层（下拉菜单、调色板、说明气泡）

void ShowMenu(Host *host, RectF anchor, const std::vector<Option> &options, const std::string &selected, float minWidth,
              std::function<void(const std::string *)> onDone);
void ShowPalette(Host *host, RectF anchor, const std::string &selected, std::function<void(int, bool)> onDone);
void ShowInfoBubble(Host *host, RectF anchor, const std::wstring &text);
void HideInfoBubble();
void CloseAllPopups();
// 系统取色框；返回 false 表示取消
bool PickColor(HWND owner, std::string &hex, bool alpha);
// 快捷键录入框正在录入
bool IsRecordingNow();

// 页面
struct PageInfo {
    const char *id;
    const char *icon;
    const char *titleKey;
};
const std::vector<PageInfo> &Pages();
FormPage *BuildPage(const std::string &id);
bool NeedsRebuild(const std::string &page, const std::string &key);

}  // namespace prefs
}  // namespace ls
