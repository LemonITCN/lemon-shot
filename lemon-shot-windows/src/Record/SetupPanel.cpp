#include "Record/SetupPanel.h"

#include <cmath>
#include <memory>

#include "Capture/Toolbar.h"
#include "Prefs/Widgets.h"
#include "Record/Devices.h"
#include "Record/MF.h"
#include "Record/RecordUI.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "Util/Util.h"

namespace ls {
namespace rec {

namespace {

const Tokens &K() { return theme::Tok(); }

// 弹出菜单的宿主：只需要屏幕坐标与 DPI
class MenuHost : public prefs::Host {
public:
    HWND hwnd = nullptr;
    float scale = 1;
    UINT dpi = 96;
    void Invalidate() override {}
    HWND Hwnd() override { return hwnd; }
    RECT ScreenRect(RectF r) override {
        POINT o = {0, 0};
        if (hwnd) ClientToScreen(hwnd, &o);
        RECT out = {o.x + (LONG)std::floor(r.x * scale), o.y + (LONG)std::floor(r.y * scale), o.x + (LONG)std::ceil(r.r() * scale),
                    o.y + (LONG)std::ceil(r.b() * scale)};
        return out;
    }
    UINT Dpi() override { return dpi; }
    void BeginEdit(RectF, const std::wstring &, bool, bool, std::function<void(const std::wstring &, bool)>) override {}
    void EndEdit() override {}
    void SetFocusWidget(prefs::Widget *) override {}
    prefs::Widget *FocusWidget() override { return nullptr; }
    void RequestRebuild() override {}
};

MenuHost g_menuHost;

struct TimerEntry {
    UINT_PTR id;
    SetupPanel *panel;
    int which;
};
std::vector<TimerEntry> g_timers;

// 菜单回调可能晚于面板销毁：用共享标记判断面板还在不在
std::vector<std::pair<SetupPanel *, std::shared_ptr<bool>>> g_alive;

std::shared_ptr<bool> AliveFlag(SetupPanel *p) {
    for (auto &a : g_alive)
        if (a.first == p) return a.second;
    std::shared_ptr<bool> f = std::make_shared<bool>(true);
    g_alive.push_back(std::make_pair(p, f));
    return f;
}

void KillAlive(SetupPanel *p) {
    for (size_t i = 0; i < g_alive.size(); ++i) {
        if (g_alive[i].first == p) {
            *g_alive[i].second = false;
            g_alive.erase(g_alive.begin() + (long)i);
            return;
        }
    }
}

}  // namespace

SetupPanel::SetupPanel(HWND owner, float scale, UINT dpi, std::function<void()> redraw, std::function<void()> start)
    : owner_(owner), scale_(scale), dpi_(dpi), redraw_(redraw), start_(start) {
    mfAvailable_ = owner ? mf::Available() : true;  // 自测时按有 MF 的样子画
    chipW_[0] = ChipWidth(ChipLabels(kFormat));
    chipW_[1] = ChipWidth(ChipLabels(kQuality));
    chipW_[2] = ChipWidth(ChipLabels(kFps));
    startW_ = PrimaryButtonWidth(T("record.start"));
    Layout();
}

SetupPanel::~SetupPanel() {
    StopTimer(0);
    StopTimer(1);
    bool hadMenu = menuOpen_;
    KillAlive(this);
    if (hadMenu) prefs::CloseAllPopups();
}

SizeF SetupPanel::Size() const {
    const auto &t = K().toolbar;
    float sep = 2 * t.separatorMargin + t.separatorWidth;
    float w = 2 * t.paddingX + 6 * t.button + 5 * t.spacing;
    for (float cw : chipW_) w += sep + cw;
    w += sep + startW_ + 2;
    return SizeF{std::ceil(w), t.height};
}

void SetupPanel::SetRect(RectF r, bool below) {
    rect_ = r;
    below_ = below;
    Layout();
}

void SetupPanel::Layout() {
    const auto &t = K().toolbar;
    items_.clear();
    seps_.clear();
    float bs = t.button;
    float x = rect_.x + t.paddingX;
    float y = rect_.cy() - bs / 2;
    for (int i = 0; i < 6; ++i) {
        if (i > 0) x += t.spacing;
        items_.push_back({kMic + i, RectF(x, y, bs, bs)});
        x += bs;
    }
    for (int i = 0; i < 3; ++i) {
        x += t.separatorMargin;
        seps_.push_back(RectF(x, rect_.cy() - t.separatorHeight / 2, t.separatorWidth, t.separatorHeight));
        x += t.separatorWidth + t.separatorMargin;
        items_.push_back({kFormat + i, RectF(x, y, chipW_[i], bs)});
        x += chipW_[i];
    }
    x += t.separatorMargin;
    seps_.push_back(RectF(x, rect_.cy() - t.separatorHeight / 2, t.separatorWidth, t.separatorHeight));
    x += t.separatorWidth + t.separatorMargin;
    float bh = K().window.controlHeight;
    items_.push_back({kStart, RectF(x, rect_.cy() - bh / 2, startW_, bh)});
}

int SetupPanel::Hit(float x, float y) const {
    if (!rect_.Contains(x, y)) return -1;
    for (auto &it : items_)
        if (it.r.Inflate(K().toolbar.spacing / 2, 4).Contains(x, y)) return it.id;
    return -1;
}

const char *SetupPanel::ToggleKey(int id) const {
    switch (id) {
    case kMic: return "record.microphone";
    case kSystem: return "record.systemAudio";
    case kCamera: return "record.camera";
    case kCursor: return "record.showCursor";
    case kClicks: return "record.showClicks";
    case kKeys: return "record.showKeys";
    }
    return "";
}

bool SetupPanel::Enabled(int id) const {
    bool gif = settings::Str("record.format") == "gif";
    if (id == kMic || id == kSystem) return !gif;  // GIF 不录声音
    if (id == kCamera) return mfAvailable_;        // 没有 Media Foundation 时不能开摄像头
    if (id == kStart) return startEnabled_;
    return true;
}

bool SetupPanel::ToggleOn(int id) const {
    if (id == kCamera && !mfAvailable_) return false;
    return settings::Bool(ToggleKey(id));
}

std::vector<std::wstring> SetupPanel::ChipLabels(int id) const {
    if (id == kFormat) return {L"MP4", L"GIF"};
    if (id == kQuality) return {T("record.quality.ultra"), T("record.quality.high"), T("record.quality.standard")};
    std::vector<std::wstring> out;
    for (int f : {15, 24, 30, 60}) out.push_back(i18n::T("common.fps", {{"n", FormatW(L"%d", f)}}));
    return out;
}

std::vector<std::string> SetupPanel::ChipValues(int id) const {
    if (id == kFormat) return {"mp4", "gif"};
    if (id == kQuality) return {"ultra", "high", "standard"};
    return {"15", "24", "30", "60"};
}

int SetupPanel::ChipIndex(int id) const {
    if (id == kFormat) return settings::Str("record.format") == "gif" ? 1 : 0;
    if (id == kQuality) {
        std::string q = settings::Str("record.quality");
        return q == "ultra" ? 0 : (q == "standard" ? 2 : 1);
    }
    int fps = settings::Int("record.fps");
    const int list[4] = {15, 24, 30, 60};
    int best = 2;
    for (int i = 0; i < 4; ++i)
        if (std::abs(list[i] - fps) < std::abs(list[best] - fps)) best = i;
    return best;
}

std::wstring SetupPanel::Tooltip(int id) const {
    std::wstring dot = L" · ";
    switch (id) {
    case kMic: return T("record.microphone") + dot + T("record.holdToChooseDevice");
    case kSystem: return T("record.systemAudio");
    case kCamera: return mfAvailable_ ? T("record.camera") + dot + T("record.holdToChooseDevice") : T("record.camera");
    case kCursor: return T("record.showCursor");
    case kClicks: return T("record.showClicks");
    case kKeys: return T("record.showKeys");
    case kFormat: return T("record.format");
    case kQuality: return T("record.quality");
    case kFps: return T("record.fps");
    }
    return std::wstring();
}

void SetupPanel::Draw(Canvas &c, float screenW, float screenH) {
    const auto &u = theme::Ui();
    DrawBarPanel(c, rect_, K().toolbar.radius);
    for (auto &s : seps_) c.FillRect(s, u.divider);
    static const char *onIcons[6] = {"mic", "volume-2", "webcam", "mouse-pointer-2", "mouse-pointer-click", "keyboard"};
    static const char *offIcons[6] = {"mic-off", "volume-x", "webcam", "mouse-pointer-2", "mouse-pointer-click", "keyboard"};
    for (auto &it : items_) {
        bool en = Enabled(it.id);
        bool hot = hover_ == it.id, down = pressed_ == it.id && hot;
        if (it.id <= kKeys) {
            bool on = ToggleOn(it.id);
            DrawToggle(c, it.r, on ? onIcons[it.id] : offIcons[it.id], on, hot, down, en);
        } else if (it.id >= kFormat && it.id <= kFps) {
            std::vector<std::wstring> labels = ChipLabels(it.id);
            int idx = ChipIndex(it.id);
            DrawChip(c, it.r, labels[(size_t)idx], hot, down || (menuOpen_ && menuFor_ == it.id), en);
        } else if (it.id == kStart) {
            DrawPrimaryButton(c, it.r, T("record.start"), hot, down, en);
        }
    }
    if (tooltip_ >= 0 && !menuOpen_ && pressed_ < 0) {
        for (auto &it : items_) {
            if (it.id != tooltip_) continue;
            std::wstring tip = Tooltip(it.id);
            if (!tip.empty()) DrawToolbarTooltip(c, it.r, tip, below_, screenW, screenH);
        }
    }
}

bool SetupPanel::MouseMove(float x, float y) {
    int h = Hit(x, y);
    bool inside = rect_.Contains(x, y);
    if (pressed_ >= 0) {
        // 按住时只跟踪是否还在原按钮上
        int nh = h == pressed_ ? h : -1;
        if (nh != hover_) {
            hover_ = nh;
            Redraw();
        }
        return true;
    }
    if (h != hover_) {
        hover_ = h;
        tooltip_ = -1;
        StopTimer(0);
        if (h >= 0 && h != kStart) StartTimer(0, 500);
        Redraw();
    }
    return inside;
}

void SetupPanel::MouseLeave() {
    if (pressed_ >= 0) return;
    if (hover_ >= 0 || tooltip_ >= 0) {
        hover_ = -1;
        tooltip_ = -1;
        StopTimer(0);
        Redraw();
    }
}

bool SetupPanel::MouseDown(float x, float y) {
    if (!rect_.Contains(x, y)) return false;
    int h = Hit(x, y);
    tooltip_ = -1;
    StopTimer(0);
    if (h < 0 || !Enabled(h) || menuOpen_) {
        Redraw();
        return true;
    }
    pressed_ = h;
    hover_ = h;
    longFired_ = false;
    if ((h == kMic || h == kCamera) && owner_) StartTimer(1, 500);
    Redraw();
    return true;
}

void SetupPanel::MouseUp(float x, float y) {
    if (pressed_ < 0) return;
    int id = pressed_;
    pressed_ = -1;
    StopTimer(1);
    bool inside = Hit(x, y) == id;
    hover_ = inside ? id : Hit(x, y);
    bool fired = longFired_;
    longFired_ = false;
    Redraw();
    // 长按：松开后再弹设备菜单（弹出层在松开时接管鼠标，按住时弹出会被松开动作立即关掉）
    if (inside && fired) {
        ShowDeviceMenu(id);
        return;
    }
    if (inside) Click(id);  // 可能关闭截图层，之后不能再访问成员
}

void SetupPanel::Click(int id) {
    if (id <= kKeys) {
        const char *key = ToggleKey(id);
        settings::SetBool(key, !settings::Bool(key));
        Redraw();
        return;
    }
    if (id >= kFormat && id <= kFps) {
        ShowChipMenu(id);
        return;
    }
    if (id == kStart && start_) start_();
}

void SetupPanel::ShowChipMenu(int id) {
    if (!owner_) return;
    RectF anchor;
    for (auto &it : items_)
        if (it.id == id) anchor = it.r;
    std::vector<std::wstring> labels = ChipLabels(id);
    std::vector<std::string> values = ChipValues(id);
    std::vector<prefs::Option> opts;
    for (size_t i = 0; i < labels.size(); ++i) opts.push_back({values[i], labels[i], true});
    g_menuHost.hwnd = owner_;
    g_menuHost.scale = scale_;
    g_menuHost.dpi = dpi_;
    menuOpen_ = true;
    menuFor_ = id;
    std::shared_ptr<bool> alive = AliveFlag(this);
    Redraw();
    prefs::ShowMenu(&g_menuHost, anchor, opts, values[(size_t)ChipIndex(id)], 120, [this, alive, id](const std::string *v) {
        if (!*alive) return;
        menuOpen_ = false;
        menuFor_ = -1;
        if (v) {
            if (id == kFormat) settings::SetStr("record.format", *v);
            else if (id == kQuality) settings::SetStr("record.quality", *v);
            else settings::SetNum("record.fps", atof(v->c_str()));
        }
        Redraw();
    });
}

void SetupPanel::ShowDeviceMenu(int id) {
    if (!owner_) return;
    bool audio = id == kMic;
    const char *key = audio ? "record.microphoneDevice" : "record.cameraDevice";
    std::vector<Device> list = audio ? Microphones() : Cameras();
    std::vector<prefs::Option> opts;
    if (list.empty()) {
        opts.push_back({"", T(audio ? "record.noMicrophone" : "record.noCamera"), false});
    } else {
        opts.push_back({"", T("record.default"), true});
        for (auto &d : list) opts.push_back({d.id, d.name, true});
    }
    RectF anchor;
    for (auto &it : items_)
        if (it.id == id) anchor = it.r;
    g_menuHost.hwnd = owner_;
    g_menuHost.scale = scale_;
    g_menuHost.dpi = dpi_;
    menuOpen_ = true;
    menuFor_ = id;
    std::shared_ptr<bool> alive = AliveFlag(this);
    std::string k = key;
    prefs::ShowMenu(&g_menuHost, anchor, opts, settings::Str(key), 120, [this, alive, k](const std::string *v) {
        if (!*alive) return;
        menuOpen_ = false;
        menuFor_ = -1;
        if (v) settings::SetStr(k, *v);
        Redraw();
    });
}

// ---------------------------------------------------------------- 定时器（线程定时器，回调里按编号找面板）

void SetupPanel::StartTimer(int which, UINT ms) {
    StopTimer(which);
    if (!owner_) return;
    UINT_PTR id = SetTimer(nullptr, 0, ms, &SetupPanel::TimerProc);
    if (!id) return;
    timers_[which] = id;
    g_timers.push_back({id, this, which});
}

void SetupPanel::StopTimer(int which) {
    if (!timers_[which]) return;
    KillTimer(nullptr, timers_[which]);
    for (size_t i = 0; i < g_timers.size(); ++i) {
        if (g_timers[i].id == timers_[which]) {
            g_timers.erase(g_timers.begin() + (long)i);
            break;
        }
    }
    timers_[which] = 0;
}

void CALLBACK SetupPanel::TimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    for (auto &t : g_timers) {
        if (t.id == id) {
            SetupPanel *p = t.panel;
            int which = t.which;
            p->StopTimer(which);
            p->OnTimer(which);
            return;
        }
    }
    KillTimer(nullptr, id);
}

void SetupPanel::OnTimer(int which) {
    if (which == 0) {
        if (hover_ >= 0 && pressed_ < 0 && !menuOpen_) {
            tooltip_ = hover_;
            Redraw();
        }
    } else if (which == 1) {
        if ((pressed_ == kMic || pressed_ == kCamera) && hover_ == pressed_) {
            longFired_ = true;
            Redraw();
        }
    }
}

}  // namespace rec
}  // namespace ls
