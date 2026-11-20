// 第二期几个工具窗口（文字识别、翻译、长截图结果、带壳预览）共用的部分（对应 Mac 端 OCR/ToolUI.swift）：
// 窗口外壳（原生标题栏 + 客户区按 prefs::Widget 树自绘）、自绘部件、原生多行文本框、界面线程派发、后台串行队列。
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "Prefs/Widgets.h"
#include "UI/Gfx.h"
#include "UI/Window.h"
#include "lemonshot.h"

namespace ls {
namespace tool {

// ---------------------------------------------------------------- 线程

// 界面线程派发：必须先在界面线程调用一次 InitDispatcher（创建一个只收消息的窗口）。
// PostToUi 任何线程都可以调用，fn 稍后在界面线程执行。
void InitDispatcher();
void PostToUi(std::function<void()> fn);

// 后台串行队列：任务按提交顺序在同一条线程上执行。对象由线程自己释放（Shutdown 之后不能再访问）。
class SerialQueue {
public:
    static SerialQueue *Create();
    void Post(std::function<void()> fn);
    // 已提交的任务做完后线程退出并释放本对象
    void Shutdown();

private:
    SerialQueue() = default;
    static unsigned __stdcall ThreadMain(void *p);
    CRITICAL_SECTION lock_;
    HANDLE event_ = nullptr;
    std::vector<std::function<void()>> jobs_;
    bool quit_ = false;
};

// 新开一条线程执行一次（失败返回 false，fn 不会执行）
bool RunDetached(std::function<void()> fn);

// ---------------------------------------------------------------- 输出

// 复制图片到剪贴板 / 按保存设置存文件（save.resolution、save.format、askLocation），带 toast
void CopyImage(const ls_image &img, float scale);
void SaveImage(const ls_image &img, float scale);
void CopyText(const std::wstring &text);

// 预乘 BGRA → 缩到不超过 maxW×maxH（只缩小），返回新分配的图
ls_image ShrinkCopy(const ls_image &img, int maxW, int maxH);

// 渲染目标代次：任何工具窗口或离屏目标重建时加 1，缓存的 D2D 位图据此判断是否失效
unsigned TargetGeneration();
void BumpTargetGeneration();

// ---------------------------------------------------------------- 部件

// 工具行上的开关胶囊：开时 surfaceSelected 底 + 强调色字与边
class ToggleChip : public prefs::Widget {
public:
    std::wstring title;
    bool on;
    std::function<void(bool)> onChange;
    ToggleChip(const std::wstring &t, bool v, std::function<void(bool)> cb);
    SizeF Preferred() override;
    bool Interactive() override { return true; }
    void Draw(Canvas &c) override;
    void OnMouseUp(float, float, bool inside) override;
    static const float kHeight;
};

// 图标按钮（28 见方，图标 16），悬停显示提示气泡
class IconButton : public prefs::Widget {
public:
    const char *icon;
    std::wstring tooltip;
    bool selected = false;
    std::function<void()> action;
    float iconSize = 16;
    IconButton(const char *ic, const std::wstring &tip, std::function<void()> a);
    bool Interactive() override { return enabled; }
    void Draw(Canvas &c) override;
    void OnMouseMove(float, float) override;
    void OnMouseLeave() override;
    void OnMouseUp(float, float, bool inside) override;
};

// 多行文本区的外框。屏幕上由原生 EDIT 叠在框里负责输入；自测时 drawText 为真，直接画出文字。
class TextArea : public prefs::Widget {
public:
    std::wstring text;
    bool drawText = false;
    std::wstring overlay;  // 内容为空时框中央显示的状态（正在识别…）
    // 屏幕上显示覆盖文字时原生输入框是藏起来的：点框时回调（用来显示输入框）
    std::function<void()> onActivate;
    bool Interactive() override { return !overlay.empty() && (bool)onActivate; }
    LPCWSTR Cursor() override { return MAKEINTRESOURCEW(32513); }
    void OnMouseUp(float, float, bool inside) override {
        if (inside && onActivate) onActivate();
    }
    void Draw(Canvas &c) override;
    // 原生输入框相对外框的内缩（逻辑点）
    static RectF EditRect(RectF frame);
    static const float kTextInsetX, kTextInsetY;
};

// 把一张图等比缩放画进框内（可选棋盘格底、上对齐）。图在设置时复制一份（必要时缩小）。
class ImageBox : public prefs::Widget {
public:
    bool checker = false;
    bool topAligned = false;
    bool border = true;
    float radius = 6;
    ImageBox() = default;
    ~ImageBox() override;
    // maxW/maxH：保留的最大像素尺寸（超过就缩小，省内存）
    void SetImage(const ls_image *img, int maxW = 1600, int maxH = 1600);
    bool HasImage() const { return img_.data != nullptr; }
    int ImageWidth() const { return srcW_; }
    int ImageHeight() const { return srcH_; }
    RectF ImageRect() const;
    void Draw(Canvas &c) override;
    static void DrawChecker(Canvas &c, RectF r, float cell);

private:
    ls_image img_ = {};
    int srcW_ = 0, srcH_ = 0;
    ComPtr<ID2D1Bitmap> bmp_;
    ID2D1RenderTarget *bmpRt_ = nullptr;
    unsigned bmpGen_ = 0;
};

// 底部左侧的状态：可选图标 + 一行文字（危险色表示出错）
class Status : public prefs::Widget {
public:
    std::wstring text;
    const char *icon = nullptr;
    bool danger = false;
    SizeF Preferred() override;
    void Draw(Canvas &c) override;
};

// 从右往左排按钮（数组第一个在最右），垂直居中在 bar 里，返回最左按钮的左边界
float LayoutRight(RectF bar, const std::vector<prefs::Widget *> &buttons, float margin = 16, float gap = 8);

// 底部按钮栏高度、顶部工具行高度（与 Mac 端一致）
const float kFooterH = 56;
const float kToolRowH = 44;

// ---------------------------------------------------------------- 窗口

// 工具窗口：原生标题栏 + 客户区自绘（prefs::Widget 树）+ 原生多行文本框。
// 窗口关闭后对象在下一轮消息循环里释放（OnClosed 之后不能再访问）。
class ToolWindow : public Window, public prefs::Host {
public:
    ToolWindow();
    ~ToolWindow() override;

    // 客户区逻辑尺寸 w×h；resizable 时 minW×minH 为最小客户区。窗口放在 mon 的工作区中央（mon 为空时取光标所在屏）
    bool Open(const std::wstring &title, float w, float h, float minW, float minH, bool resizable, HMONITOR mon = nullptr);
    void Present(bool activate);
    void SetTopmost(bool on);
    void Close();
    bool IsOpen() const { return hwnd_ != nullptr; }
    float S() const { return dpi_ / 96.f; }
    float ClientW() const;
    float ClientH() const;
    prefs::Widget &Root() { return root_; }
    void Relayout();

    // 原生多行文本框（UTF-16，\n 与 \r\n 自动换算）
    HWND CreateTextEdit(bool readOnly);
    void PlaceEdit(HWND e, RectF frame, bool visible = true);
    static std::wstring EditText(HWND e);
    void SetEditText(HWND e, const std::wstring &t);
    void AppendEditText(HWND e, const std::wstring &t);

    // prefs::Host
    void Invalidate() override;
    HWND Hwnd() override { return hwnd_; }
    RECT ScreenRect(RectF r) override;
    UINT Dpi() override { return dpi_; }
    void BeginEdit(RectF, const std::wstring &, bool, bool, std::function<void(const std::wstring &, bool)>) override {}
    void EndEdit() override {}
    void SetFocusWidget(prefs::Widget *w) override { focus_ = w; }
    prefs::Widget *FocusWidget() override { return focus_; }
    void RequestRebuild() override { Relayout(); }

    // 主题变化后（设置或系统）刷新所有打开的工具窗口
    static void RefreshAllThemes();

protected:
    virtual void Layout(float w, float h) = 0;
    virtual void DrawContent(Canvas &c, float w, float h);  // 默认：windowBg 底 + 部件树
    virtual void OnEditChange(HWND) {}
    virtual bool OnEditKey(HWND, UINT) { return false; }   // 文本框里的按键（返回 true 表示已处理）
    virtual bool OnWheel(float, float, float) { return false; }
    virtual void OnClosed() {}
    virtual void OnTargetReset() {}  // 渲染目标重建：缓存的位图作废
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override;
    void OnDestroyed() override;

    prefs::Widget root_;
    UINT dpi_ = 96;
    float minW_ = 0, minH_ = 0;
    bool resizable_ = false;

private:
    void Paint();
    void SetHover(prefs::Widget *w);
    void ApplyEditTheme();
    void UpdateEditFont();
    static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref);

    ComPtr<ID2D1HwndRenderTarget> rt_;
    prefs::Widget *hover_ = nullptr, *pressed_ = nullptr, *focus_ = nullptr;
    bool tracking_ = false;
    std::vector<HWND> edits_;
    HWND lastEdit_ = nullptr;  // 上次有焦点的文本框（窗口激活时把焦点还给它）
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    COLORREF editBg_ = 0, editText_ = 0;
};

// 离屏渲染一棵部件树（自测）：w×h 逻辑点，scale 倍；draw 里先画底再画部件
ls_image RenderOffscreen(float w, float h, float scale, const std::function<void(Canvas &)> &draw);

}  // namespace tool
}  // namespace ls
