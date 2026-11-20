#include "Prefs/Widgets.h"

#include <commdlg.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cctype>
#include <cmath>
#include <cstring>

#include "App/App.h"
#include "App/Hotkeys.h"
#include "Capture/StylePopover.h"
#include "Capture/StyleSettings.h"
#include "Settings/I18n.h"
#include "Settings/Settings.h"
#include "UI/Controls.h"
#include "UI/Window.h"
#include "Util/Util.h"

namespace ls {
namespace prefs {

namespace {

const Tokens &K() { return theme::Tok(); }
const UiColors &U() { return theme::Ui(); }
float Body() { return K().fontSize.body; }
float ControlH() { return K().window.controlHeight; }
float ControlR() { return K().window.controlRadius; }

void InputBox(Canvas &c, RectF r, bool focused, bool danger = false) {
    float rad = ControlR();
    if (focused) c.FillRound(r.Inflate(3), rad + 3, U().focusRing);
    c.FillRound(r, rad, U().inputBg);
    Color border = danger ? U().danger : (focused ? U().accent : U().inputBorder);
    c.StrokeRound(r.Inflate(-0.5f), rad - 0.5f, border, 1);
}

TextStyle TS(float size, Color col, int weight = kRegular, HAlign h = HAlign::Left) {
    TextStyle t;
    t.size = size;
    t.color = col;
    t.weight = weight;
    t.h = h;
    t.v = VAlign::Center;
    return t;
}

float TextW(const std::wstring &s, float size, int weight = kRegular) { return gfx::Measure(s, size, weight).w; }

}  // namespace

// ---------------------------------------------------------------- Widget

Widget *Widget::HitTest(float x, float y) {
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if ((*it)->hidden) continue;
        if (Widget *w = (*it)->HitTest(x, y)) return w;
    }
    if (Interactive() && frame.Contains(x, y)) return this;
    return nullptr;
}

void Widget::SetHost(Host *h) {
    host = h;
    for (auto &c : children) c->SetHost(h);
}

// ---------------------------------------------------------------- Label

Label::Label(const std::wstring &t, float fontSize, const char *key, int w)
    : text(t), size(fontSize), weight(w), colorKey(key) {
    frame = RectF(0, 0, std::ceil(TextW(t, fontSize, w)) + 2, ControlH());
}

Color Label::KeyColor(const char *key) {
    const auto &u = U();
    if (!key) return u.text;
    if (!strcmp(key, "textSecondary")) return u.textSecondary;
    if (!strcmp(key, "textTertiary")) return u.textTertiary;
    if (!strcmp(key, "danger")) return u.danger;
    if (!strcmp(key, "success")) return u.success;
    if (!strcmp(key, "accent")) return u.accent;
    return u.text;
}

void Label::Draw(Canvas &c) { c.Text(text, frame, TS(size, KeyColor(colorKey), weight)); }

// ---------------------------------------------------------------- ButtonW

ButtonW::ButtonW(const std::wstring &t, Kind k, const char *ic, std::function<void()> a) : title(t), kind(k), icon(ic), action(a) {
    SizeF s = Preferred();
    frame = RectF(0, 0, s.w, s.h);
}

SizeF ButtonW::Preferred() {
    float w = TextW(title, Body(), kMedium) + 2 * K().window.buttonPaddingX;
    if (icon) w += 22;
    return SizeF{std::ceil(w), ControlH()};
}

void ButtonW::Draw(Canvas &c) {
    const auto &u = U();
    RectF r = frame;
    float rad = ControlR();
    Color tc;
    if (kind == Primary) {
        c.FillRound(r, rad, hovered || pressed ? u.accentHover : u.accent);
        tc = u.accentText;
    } else {
        c.FillRound(r, rad, pressed ? u.surfacePressed : (hovered ? u.surfaceHover : u.surface));
        c.StrokeRound(r.Inflate(-0.5f), rad - 0.5f, u.inputBorder, 1);
        tc = kind == Danger ? u.danger : u.text;
    }
    if (!enabled) tc = u.textTertiary;
    float tw = TextW(title, Body(), kMedium);
    float x = r.cx() - tw / 2;
    if (icon) {
        float total = tw + 22;
        x = r.cx() - total / 2;
        c.Icon(icon, x + 8, r.cy(), 16, tc, 1.6f);
        x += 22;
    }
    c.Text(title, RectF(x, r.y, tw + 2, r.h), TS(Body(), tc, kMedium));
}

void ButtonW::OnMouseUp(float, float, bool inside) {
    if (inside && enabled && action) action();
}

// ---------------------------------------------------------------- SwitchW

SwitchW::SwitchW(bool v, std::function<void(bool)> cb) : on(v), onChange(cb) {
    frame = RectF(0, 0, K().window.switchWidth, K().window.switchHeight);
}

SizeF SwitchW::Preferred() { return SizeF{K().window.switchWidth, K().window.switchHeight}; }

void SwitchW::Draw(Canvas &c) {
    RectF r(frame.x, frame.cy() - K().window.switchHeight / 2, K().window.switchWidth, K().window.switchHeight);
    ui::CtrlState st;
    ui::DrawSwitch(c, r, on, st);
}

void SwitchW::OnMouseUp(float, float, bool inside) {
    if (!inside) return;
    on = !on;
    Redraw();
    if (onChange) onChange(on);
}

// ---------------------------------------------------------------- SegmentedW

SegmentedW::SegmentedW(const std::vector<std::wstring> &it, int sel, std::function<void(int)> cb)
    : items(it), selected(sel), onChange(cb) {
    SizeF s = Preferred();
    frame = RectF(0, 0, s.w, s.h);
}

SizeF SegmentedW::Preferred() {
    float m = 40;
    bool first = true;
    for (auto &i : items) {
        float w = TextW(i, Body(), kMedium);
        if (first || w > m) m = w;
        first = false;
    }
    return SizeF{std::ceil((m + 24) * items.size() + 4), ControlH()};
}

RectF SegmentedW::Seg(int i) {
    RectF inner = frame.Inflate(-2);
    float w = inner.w / (items.empty() ? 1 : items.size());
    return RectF(inner.x + w * i, inner.y, w, inner.h);
}

void SegmentedW::Draw(Canvas &c) {
    const auto &u = U();
    float rad = ControlR();
    c.FillRound(frame, rad, u.controlTrack);
    for (int i = 0; i < (int)items.size(); ++i) {
        RectF r = Seg(i);
        if (i == selected) {
            c.FillRound(r.Offset(0, 1), rad - 1, Color{0, 0, 0, 0.10f});
            c.FillRound(r, rad - 1, theme::IsDark() ? u.surfacePressed : u.surface);
        } else if (i == hoverIndex && enabled) {
            c.FillRound(r, rad - 1, WithAlpha(u.surfaceHover, 0.6f));
        }
        Color col = i == selected ? u.text : u.textSecondary;
        c.Text(items[(size_t)i], r.Inflate(-4, 0), TS(Body(), col, i == selected ? kMedium : kRegular, HAlign::Center));
    }
}

void SegmentedW::OnMouseMove(float x, float y) {
    int idx = -1;
    for (int i = 0; i < (int)items.size(); ++i)
        if (Seg(i).Contains(x, y)) idx = i;
    if (idx != hoverIndex) {
        hoverIndex = idx;
        Redraw();
    }
}

void SegmentedW::OnMouseLeave() {
    hoverIndex = -1;
    Redraw();
}

void SegmentedW::OnMouseUp(float x, float y, bool inside) {
    if (!inside) return;
    for (int i = 0; i < (int)items.size(); ++i) {
        if (Seg(i).Contains(x, y) && i != selected) {
            selected = i;
            Redraw();
            if (onChange) onChange(i);
            return;
        }
    }
}

// ---------------------------------------------------------------- DropdownW

DropdownW::DropdownW(const std::vector<Option> &o, const std::string &v, float width, std::function<void(const std::string &)> cb)
    : options(o), value(v), onChange(cb) {
    frame = RectF(0, 0, width, ControlH());
}

std::wstring DropdownW::CurrentLabel() {
    for (auto &o : options)
        if (o.value == value) return o.label;
    return Widen(value);
}

void DropdownW::Draw(Canvas &c) {
    const auto &u = U();
    RectF r = frame;
    InputBox(c, r, open);
    if (hovered && !open) c.FillRound(r.Inflate(-1), ControlR() - 1, WithAlpha(u.surfaceHover, 0.5f));
    TextStyle ts = TS(Body(), u.text);
    ts.ellipsis = true;
    c.Text(CurrentLabel(), RectF(r.x + 10, r.y, r.w - 36, r.h), ts);
    c.Icon("chevron-down", r.r() - 17, r.cy(), 14, u.textSecondary, 1.6f);
}

void DropdownW::OnMouseUp(float, float, bool inside) {
    if (!inside || open || NowSeconds() - closedAt < 0.25 || !host || !host->Hwnd()) return;
    open = true;
    Redraw();
    DropdownW *self = this;
    ShowMenu(host, frame, options, value, frame.w, [self](const std::string *picked) {
        self->open = false;
        self->closedAt = NowSeconds();
        self->Redraw();
        if (picked && *picked != self->value) {
            self->value = *picked;
            if (self->onChange) self->onChange(*picked);
        }
    });
}

// ---------------------------------------------------------------- SliderW

SliderW::SliderW(double mn, double mx, double st, double v, float trackW, std::function<std::wstring(double)> fmt,
                 std::function<void(double)> cb)
    : minV(mn), maxV(mx), step(st), value(v), trackWidth(trackW), format(fmt), onChange(cb) {
    SizeF s = Preferred();
    frame = RectF(0, 0, s.w, s.h);
}

SizeF SliderW::Preferred() { return SizeF{trackWidth + 10 + valueWidth, ControlH()}; }

RectF SliderW::Track() { return RectF(frame.x + 8, frame.cy() - 2, trackWidth - 16, 4); }

void SliderW::Draw(Canvas &c) {
    const auto &u = U();
    RectF t = Track();
    c.FillRound(t, 2, u.controlTrack);
    double f = (Clamp(value, minV, maxV) - minV) / (maxV - minV > 1e-9 ? maxV - minV : 1);
    c.FillRound(RectF(t.x, t.y, (float)(t.w * f), t.h), 2, u.accent);
    float kx = t.x + (float)(t.w * f), ky = t.cy();
    c.FillEllipse(kx, ky + 1, 8.5f, 8.5f, Color{0, 0, 0, 0.12f});
    c.FillEllipse(kx, ky, 8, 8, u.controlKnob);
    c.StrokeEllipse(kx, ky, 7.75f, 7.75f, u.inputBorder, 0.5f);
    TextStyle ts = TS(Body() - 1, u.textSecondary);
    ts.tabular = true;
    c.Text(format ? format(value) : NumberText(value), RectF(frame.x + trackWidth + 10, frame.y, valueWidth, frame.h), ts);
}

void SliderW::SetFrom(float x) {
    RectF t = Track();
    double f = Clamp((double)(x - t.x) / (t.w > 1 ? t.w : 1), 0.0, 1.0);
    double v = minV + f * (maxV - minV);
    if (step > 0) v = std::floor(v / step + 0.5) * step;
    v = Clamp(v, minV, maxV);
    if (std::fabs(v - value) > 1e-9) {
        value = v;
        Redraw();
        if (onChange) onChange(v);
    }
}

bool SliderW::OnWheel(float dy) {
    double v = Clamp(value + (dy > 0 ? step : -step), minV, maxV);
    if (v != value) {
        value = v;
        Redraw();
        if (onChange) onChange(v);
    }
    return true;
}

// ---------------------------------------------------------------- StepperW

StepperW::StepperW(double mn, double mx, double st, double v, float width, std::function<std::wstring(double)> fmt,
                   std::function<void(double)> cb)
    : minV(mn), maxV(mx), step(st), value(v), format(fmt), onChange(cb) {
    frame = RectF(0, 0, width, ControlH());
}

void StepperW::Draw(Canvas &c) {
    const auto &u = U();
    InputBox(c, frame, editing);
    RectF parts[2] = {Minus(), Plus()};
    const char *icons[2] = {"minus", "plus"};
    for (int i = 0; i < 2; ++i) {
        int part = i == 0 ? -1 : 1;
        if (hotPart == part && hovered && enabled)
            c.FillRound(parts[i].Inflate(-2), ControlR() - 2, pressed ? u.surfacePressed : u.surfaceHover);
        bool disabled = (part < 0 && value <= minV) || (part > 0 && value >= maxV);
        c.Icon(icons[i], parts[i].cx(), parts[i].cy(), 14, disabled ? u.textTertiary : u.icon, 1.75f);
    }
    c.FillRect(RectF(Minus().r(), frame.y + 6, 1, frame.h - 12), u.divider);
    c.FillRect(RectF(Plus().x - 1, frame.y + 6, 1, frame.h - 12), u.divider);
    if (!editing) {
        TextStyle ts = TS(Body(), u.text, kRegular, HAlign::Center);
        ts.tabular = true;
        c.Text(format ? format(value) : NumberText(value), ValueRect(), ts);
    }
}

void StepperW::OnMouseMove(float x, float y) {
    int part = Minus().Contains(x, y) ? -1 : (Plus().Contains(x, y) ? 1 : 0);
    if (part != hotPart) {
        hotPart = part;
        Redraw();
    }
}

void StepperW::Bump(double d) {
    double v = Clamp(value + d, minV, maxV);
    if (v != value) {
        value = v;
        Redraw();
        if (onChange) onChange(v);
    }
}

bool StepperW::OnWheel(float dy) {
    Bump(dy > 0 ? step : -step);
    return true;
}

void StepperW::OnMouseUp(float x, float y, bool inside) {
    if (!inside || !enabled) return;
    if (Minus().Contains(x, y)) {
        Bump(-step);
    } else if (Plus().Contains(x, y)) {
        Bump(step);
    } else if (host && host->Hwnd()) {
        editing = true;
        Redraw();
        StepperW *self = this;
        host->BeginEdit(ValueRect().Inflate(-2, -4), FormatW(L"%d", (int)std::lround(value)), false, true,
                        [self](const std::wstring &s, bool done) {
                            if (!done) return;
                            self->editing = false;
                            std::wstring t = Trim(s);
                            if (!t.empty()) {
                                double v = Clamp(_wtof(t.c_str()), self->minV, self->maxV);
                                if (v != self->value) {
                                    self->value = v;
                                    if (self->onChange) self->onChange(v);
                                }
                            }
                            self->Redraw();
                        });
    }
}

// ---------------------------------------------------------------- TextFieldW

TextFieldW::TextFieldW(const std::wstring &t, bool sec, float width, std::function<void(const std::wstring &)> cb)
    : text(t), secure(sec), onChange(cb) {
    frame = RectF(0, 0, width, ControlH());
}

void TextFieldW::Draw(Canvas &c) {
    const auto &u = U();
    InputBox(c, frame, editing);
    if (editing) return;
    RectF tr = frame.Inflate(-8, 0);
    TextStyle ts = TS(Body(), u.text);
    ts.ellipsis = true;
    if (text.empty()) {
        ts.color = u.textTertiary;
        c.Text(placeholder, tr, ts);
    } else {
        std::wstring shown = text;
        if (secure) shown = std::wstring(text.size() < 24 ? text.size() : 24, L'•');
        c.Text(shown, tr, ts);
    }
}

void TextFieldW::OnMouseUp(float, float, bool inside) {
    if (!inside || editing || !host || !host->Hwnd()) return;
    editing = true;
    Redraw();
    TextFieldW *self = this;
    host->BeginEdit(frame.Inflate(-8, -4), text, secure, false, [self](const std::wstring &s, bool done) {
        if (s != self->text) {
            self->text = s;
            if (self->onChange) self->onChange(s);
        }
        if (done) {
            self->editing = false;
            self->Redraw();
        }
    });
}

// ---------------------------------------------------------------- InfoW

void InfoW::Draw(Canvas &c) {
    c.Icon("info", frame.cx(), frame.cy(), 16, hovered ? U().text : U().textSecondary, 1.6f);
}

void InfoW::OnMouseMove(float, float) {
    if (host && host->Hwnd()) ShowInfoBubble(host, frame, text);
}

void InfoW::OnMouseLeave() { HideInfoBubble(); }

// ---------------------------------------------------------------- ColorSwatchW

ColorSwatchW::ColorSwatchW(const std::string &h, bool allowAlpha, std::function<void(const std::string &)> cb)
    : hex(h), alpha(allowAlpha), onChange(cb) {
    frame = RectF(0, 0, 44, ControlH());
}

static void DrawChecker(Canvas &c, float cx, float cy, float d) {
    ComPtr<ID2D1EllipseGeometry> g;
    D2D1_ELLIPSE e = {{cx, cy}, d / 2, d / 2};
    gfx::D2D()->CreateEllipseGeometry(&e, g.Put());
    c.PushGeometryMask(g.Get());
    c.FillRect(RectF(cx - d / 2, cy - d / 2, d, d), Color{1, 1, 1, 1});
    float s = 4;
    int row = 0;
    for (float y = cy - d / 2; y < cy + d / 2; y += s, ++row)
        for (float x = cx - d / 2 + (row % 2 ? s : 0); x < cx + d / 2; x += 2 * s)
            c.FillRect(RectF(x, y, s, s), Color{0.8f, 0.8f, 0.8f, 1});
    c.PopLayer();
}

void ColorSwatchW::Draw(Canvas &c) {
    InputBox(c, frame, false);
    float d = 18, cx = frame.cx(), cy = frame.cy();
    DrawChecker(c, cx, cy, d);
    c.FillEllipse(cx, cy, d / 2, d / 2, ParseColor(hex, Color{0, 0, 0, 1}));
    c.StrokeEllipse(cx, cy, d / 2 - 0.5f, d / 2 - 0.5f, U().border, 1);
}

void ColorSwatchW::OnMouseUp(float, float, bool inside) {
    if (!inside || !host || !host->Hwnd()) return;
    ColorSwatchW *self = this;
    ShowPalette(host, frame, hex, [self](int index, bool ok) {
        if (!ok) return;
        std::string v;
        if (index >= 0) {
            std::vector<Color> pal = style::Palette();
            v = ColorToHex(pal[(size_t)index], false);
            if (self->alpha && self->hex.size() == 9) v += self->hex.substr(7);
        } else {
            v = self->hex;
            if (!PickColor(self->host->Hwnd(), v, self->alpha)) return;
        }
        self->hex = v;
        self->Redraw();
        if (self->onChange) self->onChange(v);
    });
}

// ---------------------------------------------------------------- PathW

PathW::PathW(const std::string &p, float width, std::function<void(const std::string &)> cb) : path(p), onChange(cb) {
    frame = RectF(0, 0, width, ControlH());
    PathW *self = this;
    button = (ButtonW *)Add(new ButtonW(T("common.choose"), ButtonW::Normal, nullptr, [self]() {
        if (!self->host || !self->host->Hwnd()) return;
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, dlg.PutVoid())))
            return;
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        std::wstring cur = settings::ExpandPath(self->path);
        ComPtr<IShellItem> folder;
        if (DirExists(cur) && SUCCEEDED(SHCreateItemFromParsingName(cur.c_str(), nullptr, IID_IShellItem, folder.PutVoid())))
            dlg->SetFolder(folder.Get());
        if (FAILED(dlg->Show(self->host->Hwnd()))) return;
        ComPtr<IShellItem> item;
        PWSTR p = nullptr;
        if (FAILED(dlg->GetResult(item.Put())) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return;
        std::wstring chosen = p;
        CoTaskMemFree(p);
        // 在图片/视频/音乐目录下时写成占位符，设置文件可在两端、不同用户间通用
        std::string v = Narrow(chosen);
        struct {
            const char *token;
            const GUID *id;
        } map[] = {{"{pictures}", &FOLDERID_Pictures}, {"{movies}", &FOLDERID_Videos}, {"{music}", &FOLDERID_Music}};
        for (auto &m : map) {
            std::wstring base = KnownFolder(*m.id);
            if (!base.empty() && chosen.size() >= base.size() && _wcsnicmp(chosen.c_str(), base.c_str(), base.size()) == 0 &&
                (chosen.size() == base.size() || chosen[base.size()] == L'\\')) {
                v = std::string(m.token) + ReplaceAll(Narrow(chosen.substr(base.size())), "\\", "/");
                break;
            }
        }
        self->path = v;
        self->Redraw();
        if (self->onChange) self->onChange(v);
    }));
}

void PathW::Layout() {
    SizeF bs = button->Preferred();
    button->frame = RectF(frame.r() - bs.w, frame.y, bs.w, bs.h);
}

RectF PathW::Box() { return RectF(frame.x, frame.y, frame.w - button->Preferred().w - 8, frame.h); }

std::wstring PathW::ShortName(const std::string &raw) {
    std::wstring p = settings::ExpandPath(raw);
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t a = p.find_last_of(L"\\/");
    if (a == std::wstring::npos) return p;
    size_t b = a > 0 ? p.find_last_of(L"\\/", a - 1) : std::wstring::npos;
    return b == std::wstring::npos ? p : p.substr(b + 1);
}

void PathW::Draw(Canvas &c) {
    RectF r = Box();
    InputBox(c, r, false);
    c.Icon("folder-open", r.x + 15, r.cy(), 14, U().textSecondary, 1.6f);
    TextStyle ts = TS(Body(), U().text);
    ts.ellipsis = true;
    c.Text(ShortName(path), RectF(r.x + 28, r.y, r.w - 36, r.h), ts);
    Widget::Draw(c);
}

// ---------------------------------------------------------------- HotkeyW

static bool g_recording = false;

HotkeyW::HotkeyW(const std::string &s, bool single, float width, std::function<void(const std::string &)> cb)
    : shortcut(s), singleKey(single), onChange(cb) {
    frame = RectF(0, 0, width, ControlH());
}

static std::wstring ModsText(UINT mods) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_WIN) s += L"Win+";
    return s;
}

static UINT CurrentHotMods() {
    UINT m = 0;
    if (GetKeyState(VK_CONTROL) < 0) m |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) m |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) m |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= MOD_WIN;
    return m;
}

void HotkeyW::Draw(Canvas &c) {
    const auto &u = U();
    InputBox(c, frame, recording, conflict && !recording);
    RectF tr(frame.x + 10, frame.y, frame.w - 40, frame.h);
    if (recording) {
        if (liveMods == 0)
            c.Text(invalid ? T("pref.shortcuts.invalid") : T("pref.shortcuts.record"), tr,
                   TS(Body() - 1, invalid ? u.danger : u.textSecondary));
        else
            c.Text(ModsText(liveMods), tr, TS(Body(), u.accent));
    } else if (shortcut.empty()) {
        c.Text(L"—", tr, TS(Body(), u.textTertiary));
    } else {
        std::wstring d = singleKey ? Widen(shortcut) : hotkeys::Display(shortcut);
        c.Text(d, tr, TS(Body(), conflict ? u.danger : u.text));
    }
    if (!shortcut.empty() && !recording && hovered) {
        RectF cr = ClearRect();
        c.Icon("x", cr.cx(), cr.cy(), 12, u.textSecondary, 1.75f);
    }
}

void HotkeyW::Stop() {
    if (!recording) return;
    recording = false;
    liveMods = 0;
    g_recording = false;
    if (!singleKey) hotkeys::RegisterAll(app::MainHwnd());
    Redraw();
}

void HotkeyW::OnFocus(bool f) {
    if (!f) Stop();
    Redraw();
}

void HotkeyW::OnMouseUp(float x, float y, bool inside) {
    if (!inside) return;
    if (!shortcut.empty() && !recording && ClearRect().Contains(x, y)) {
        shortcut.clear();
        if (onChange) onChange("");
        if (host) host->SetFocusWidget(nullptr);
        Redraw();
        return;
    }
    if (recording) return;
    if (host) host->SetFocusWidget(this);
    recording = true;
    invalid = false;
    liveMods = 0;
    g_recording = true;
    if (!singleKey) hotkeys::UnregisterAll(app::MainHwnd());
    Redraw();
}

bool HotkeyW::OnKey(UINT vk, bool down) {
    if (!recording) {
        if (down && (vk == VK_RETURN || vk == VK_SPACE)) {
            OnMouseUp(frame.cx(), frame.cy(), true);
            return true;
        }
        return false;
    }
    UINT mods = CurrentHotMods();
    if (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN || vk == VK_RWIN || vk == VK_LCONTROL ||
        vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LSHIFT || vk == VK_RSHIFT) {
        liveMods = mods;
        invalid = false;
        Redraw();
        return true;
    }
    if (!down) return true;
    if (mods == 0 && vk == VK_ESCAPE) {
        Stop();
        if (host) host->SetFocusWidget(nullptr);
        return true;
    }
    if (mods == 0 && !singleKey && (vk == VK_BACK || vk == VK_DELETE)) {
        Stop();
        shortcut.clear();
        if (onChange) onChange("");
        if (host) host->SetFocusWidget(nullptr);
        return true;
    }
    if (singleKey) {
        std::string name = Narrow(hotkeys::KeyName(vk));
        if (name.empty()) return true;
        Stop();
        shortcut = name;
        if (onChange) onChange(name);
        if (host) host->SetFocusWidget(nullptr);
        return true;
    }
    bool fkey = vk >= VK_F1 && vk <= VK_F24;
    if ((mods & ~(UINT)MOD_SHIFT) == 0 && !fkey) {
        invalid = true;
        liveMods = 0;
        Redraw();
        return true;
    }
    std::string s = hotkeys::Compose(mods, vk);
    if (s.empty()) return true;
    shortcut = s;
    // 先恢复全局快捷键再写设置：设置变化会按新值重新注册
    Stop();
    if (onChange) onChange(s);
    if (host) host->SetFocusWidget(nullptr);
    return true;
}

bool IsRecordingNow() { return g_recording; }

// ---------------------------------------------------------------- PaletteEditW

PaletteEditW::PaletteEditW() { frame = RectF(0, 0, 10 * 24 + 9 * 8, 28); }

void PaletteEditW::Draw(Canvas &c) {
    std::vector<Color> pal = style::Palette();
    for (int i = 0; i < 10; ++i) {
        RectF r = Swatch(i);
        if (i == hover) c.StrokeEllipse(r.cx(), r.cy(), 14, 14, U().accent, 2);
        c.FillEllipse(r.cx(), r.cy(), 12, 12, pal[(size_t)i]);
        c.StrokeEllipse(r.cx(), r.cy(), 11.5f, 11.5f, U().border, 1);
    }
}

void PaletteEditW::OnMouseMove(float x, float y) {
    int h = -1;
    for (int i = 0; i < 10; ++i)
        if (Swatch(i).Contains(x, y)) h = i;
    if (h != hover) {
        hover = h;
        Redraw();
    }
}

void PaletteEditW::OnMouseUp(float x, float y, bool inside) {
    if (!inside || !host || !host->Hwnd()) return;
    for (int i = 0; i < 10; ++i) {
        if (!Swatch(i).Contains(x, y)) continue;
        std::vector<Color> pal = style::Palette();
        std::string hex = ColorToHex(pal[(size_t)i], false);
        if (!PickColor(host->Hwnd(), hex, false)) return;
        Json arr = Json::MakeArray();
        for (int k = 0; k < 10; ++k) arr.Push(Json::MakeString(k == i ? hex : ColorToHex(pal[(size_t)k], false)));
        settings::Set("annotate.palette", arr);
        Redraw();
        return;
    }
}

// ---------------------------------------------------------------- StyleRowW（标注页：与样式面板同样的控件）

static PopLayout RowLayout(int tool, int part) {
    ls_style st = style::FromSettings(tool);
    if (tool == LS_TOOL_MOSAIC && part > 0) st.variant = 1;  // 让两个滑块都排出来
    PopLayout lay = LayoutPopover(tool, st, false);
    float rh = theme::Tok().popover.rowHeight;
    PopLayout out;
    out.tool = tool;
    if (part == 0) {
        for (auto &it : lay.items)
            if (it.r.y < rh / 2) out.items.push_back(it);
    } else {
        // 马赛克第 2 行：[块大小标签][滑块][笔刷大小标签][滑块]，part 1 取前两个，part 2 取后两个
        int idx = 0;
        for (auto &it : lay.items) {
            if (it.r.y < rh / 2 || it.kind == PK_SEP) continue;
            if (idx / 2 + 1 == part) out.items.push_back(it);
            ++idx;
        }
    }
    float minX = 1e9f, maxX = 0, minY = 1e9f;
    for (auto &it : out.items) {
        minX = std::fmin(minX, it.r.x);
        maxX = std::fmax(maxX, it.r.r());
        minY = std::fmin(minY, it.r.y);
    }
    if (out.items.empty()) minX = minY = 0;
    for (auto &it : out.items) {
        it.r = it.r.Offset(-minX, -minY);
        it.track = it.track.Offset(-minX, -minY);
    }
    out.w = maxX - minX;
    out.h = rh;
    return out;
}

StyleRowW::StyleRowW(int t, bool second) : tool(t), secondRow(second) {
    SizeF s = Preferred();
    frame = RectF(0, 0, s.w, s.h);
}

SizeF StyleRowW::Preferred() {
    PopLayout lay = RowLayout(tool, secondRow ? 1 : 0);
    return SizeF{lay.w, lay.h};
}

void StyleRowW::Draw(Canvas &c) {
    if (tool == LS_TOOL_MOSAIC && secondRow) {
        // 马赛克两行滑块合在一个控件里：块大小、笔刷大小上下排
        PopLayout a = RowLayout(tool, 1), b = RowLayout(tool, 2);
        ls_style st = style::FromSettings(tool);
        float off = (K().window.rowHeight - K().popover.rowHeight) / 2;
        DrawPopover(c, a, frame.x, frame.y + off, st, hover < 100 ? hover : -1, false);
        DrawPopover(c, b, frame.x, frame.y + off + K().window.rowHeight + K().window.rowGap, st,
                    hover >= 100 ? hover - 100 : -1, false);
        return;
    }
    PopLayout lay = RowLayout(tool, 0);
    DrawPopover(c, lay, frame.x, frame.y, style::FromSettings(tool), hover, false);
}

static bool StyleHit(StyleRowW *w, float x, float y, PopLayout &lay, int &item, float &lx, float &ly) {
    if (w->tool == LS_TOOL_MOSAIC && w->secondRow) {
        float off = (K().window.rowHeight - K().popover.rowHeight) / 2;
        float y2 = w->frame.y + K().window.rowHeight + K().window.rowGap;
        bool second = y >= y2;
        lay = RowLayout(w->tool, second ? 2 : 1);
        lx = x - w->frame.x;
        ly = y - (second ? y2 : w->frame.y) - off;
        item = HitPopover(lay, lx, ly);
        if (item >= 0 && second) item += 100;
        return item >= 0;
    }
    lay = RowLayout(w->tool, 0);
    lx = x - w->frame.x;
    ly = y - w->frame.y;
    item = HitPopover(lay, lx, ly);
    return item >= 0;
}

void StyleRowW::OnMouseMove(float x, float y) {
    PopLayout lay;
    int item;
    float lx, ly;
    StyleHit(this, x, y, lay, item, lx, ly);
    if (item != hover) {
        hover = item;
        Redraw();
    }
}

void StyleRowW::OnMouseDown(float x, float y) {
    PopLayout lay;
    int item;
    float lx, ly;
    if (!StyleHit(this, x, y, lay, item, lx, ly)) return;
    pressedItem = item;
    ls_style st = style::FromSettings(tool);
    if (PopoverApply(lay, item % 100, lx, ly, st, false)) {
        style::ToSettings(tool, st);
        Redraw();
    }
}

void StyleRowW::OnMouseDrag(float x, float y) {
    if (pressedItem < 0) return;
    PopLayout lay = RowLayout(tool, (tool == LS_TOOL_MOSAIC && secondRow) ? (pressedItem >= 100 ? 2 : 1) : 0);
    float ly = 0;
    ls_style st = style::FromSettings(tool);
    if (PopoverApply(lay, pressedItem % 100, x - frame.x, ly, st, true)) {
        style::ToSettings(tool, st);
        Redraw();
    }
}

void StyleRowW::OnMouseUp(float, float, bool) { pressedItem = -1; }

// ---------------------------------------------------------------- 关于

void DrawAppIcon(Canvas &c, RectF r) {
    c.Shadow(r, r.w * 0.225f, 10, 3, Color{0, 0, 0, 0.25f});
    c.FillRound(r, r.w * 0.225f, K().light.accent);
    RectF a = r.Inflate(-r.w * 0.2f);
    c.Icon("scan", a.cx(), a.cy(), a.w, Color{1, 1, 1, 1}, r.w / 24.f * 1.9f);
    RectF b = r.Inflate(-r.w * 0.36f);
    c.Icon("circle-dot", b.cx(), b.cy(), b.w, Color{1, 1, 1, 1}, r.w / 24.f * 1.5f);
}

void AboutW::Draw(Canvas &c) {
    const auto &u = U();
    float size = 96;
    RectF icon(frame.cx() - size / 2, frame.y + 8, size, size);
    DrawAppIcon(c, icon);
    float y = icon.b() + 16;
    c.Text(T("app.name"), RectF(frame.x, y, frame.w, 30), TS(K().fontSize.header + 2, u.text, kSemibold, HAlign::Center));
    y += 32;
    c.Text(i18n::T("about.version", {{"version", L"0.1.0"}}), RectF(frame.x, y, frame.w, 18),
           TS(K().fontSize.caption + 1, u.textSecondary, kRegular, HAlign::Center));
    y += 26;
    c.Text(T("about.tagline"), RectF(frame.x, y, frame.w, 20), TS(Body(), u.text, kRegular, HAlign::Center));
}

void CenteredRowW::Layout() {
    float total = 0;
    for (auto &ch : children) total += ch->Preferred().w;
    if (children.size() > 1) total += (children.size() - 1) * 10.f;
    float x = frame.cx() - total / 2;
    for (auto &ch : children) {
        SizeF s = ch->Preferred();
        ch->frame = RectF(x, frame.y, s.w, s.h);
        x += s.w + 10;
    }
}

// ---------------------------------------------------------------- FormPage

void FormPage::AddGroup(const std::wstring &t, const std::wstring &info) {
    Item it;
    it.kind = Group;
    it.text = t;
    if (!info.empty()) it.info = (InfoW *)Add(new InfoW(info));
    items.push_back(it);
}

void FormPage::AddRow(const std::wstring &label, std::vector<Widget *> controls, const std::wstring &info) {
    Item it;
    it.kind = Row;
    it.text = label;
    it.controls = controls;
    for (auto *c : controls) Add(c);
    if (!info.empty()) it.info = (InfoW *)Add(new InfoW(info));
    items.push_back(it);
}

void FormPage::AddValue(Label *l) {
    Item it;
    it.kind = Value;
    it.controls.push_back(l);
    Add(l);
    items.push_back(it);
}

void FormPage::AddButtons(std::vector<Widget *> bs) {
    Item it;
    it.kind = Buttons;
    it.controls = bs;
    for (auto *b : bs) Add(b);
    items.push_back(it);
}

void FormPage::AddCustom(Widget *w, float h) {
    Item it;
    it.kind = Custom;
    it.controls.push_back(w);
    it.height = h;
    Add(w);
    items.push_back(it);
}

float FormPage::LayoutPage(float width) {
    const auto &win = K().window;
    labels_.clear();
    groups_.clear();
    dividers_.clear();
    float pad = win.contentPadding, labelW = win.labelWidth, rowH = win.rowHeight, rowGap = win.rowGap,
          sectionGap = win.sectionGap;
    float x0 = frame.x + pad, cx = x0 + labelW;
    float y = frame.y + pad + 28 + 12;
    bool first = true;
    for (auto &it : items) {
        switch (it.kind) {
        case Group: {
            if (!first) {
                y += sectionGap - rowGap;
                dividers_.push_back(RectF(x0, y - sectionGap / 2, width - 2 * pad, 1));
            }
            first = false;
            if (it.text.empty()) break;
            float tw = TextW(it.text, K().fontSize.caption, kSemibold);
            groups_.push_back({it.text, RectF(x0, y, tw + 2, 20)});
            if (it.info) it.info->frame = RectF(x0 + tw + 6, y + 1, 18, 18);
            y += 24;
            break;
        }
        case Row: {
            first = false;
            // 标签放不下时折成两行，行高随之增加（不缩小字号）
            float h = rowH;
            float lw = TextW(it.text, Body());
            if (!it.text.empty() && lw > labelW - 12) {
                gfx::TextSize ts = gfx::Measure(it.text, Body(), kRegular, false, labelW - 12);
                h = std::fmax(rowH, std::ceil(ts.h) + 8);
            }
            for (auto *c : it.controls)
                if (!c->hidden && c->frame.h > h) h = c->frame.h;
            labels_.push_back({it.text, RectF(x0, y, labelW - 12, h)});
            float x = cx;
            for (auto *c : it.controls) {
                if (c->hidden) continue;
                SizeF s = (c->frame.w > 0 && c->frame.h > 0) ? SizeF{c->frame.w, c->frame.h} : c->Preferred();
                c->frame = RectF(x, y + (h - s.h) / 2, s.w, s.h);
                c->Layout();
                x += s.w + 8;
            }
            if (it.info) {
                float iw = std::fmin(lw, labelW - 36);
                it.info->frame = RectF(x0 + iw + 4, y + (h - 18) / 2, 18, 18);
            }
            y += h + rowGap;
            break;
        }
        case Value: {
            Widget *l = it.controls[0];
            l->frame = RectF(cx, y - rowGap - 4, frame.x + width - pad - cx, 18);
            y += 16;
            break;
        }
        case Buttons: {
            y += sectionGap - rowGap;
            float x = x0;
            for (auto *b : it.controls) {
                SizeF s = b->Preferred();
                b->frame = RectF(x, y, s.w, s.h);
                b->Layout();
                x += s.w + 10;
            }
            y += ControlH() + rowGap;
            break;
        }
        case Custom: {
            Widget *w = it.controls[0];
            w->frame = RectF(x0, y, width - 2 * pad, it.height);
            w->Layout();
            y += it.height + rowGap;
            break;
        }
        }
    }
    return y - frame.y + pad - rowGap;
}

void FormPage::Draw(Canvas &c) {
    const auto &u = U();
    float pad = K().window.contentPadding;
    c.Text(title, RectF(frame.x + pad, frame.y + pad, frame.w - 2 * pad, 28), TS(K().fontSize.header, u.text, kSemibold));
    for (auto &g : groups_) c.Text(g.t, g.r, TS(K().fontSize.caption, u.textSecondary, kSemibold));
    for (auto &d : dividers_) c.FillRect(d, u.divider);
    for (auto &l : labels_) {
        if (l.t.empty()) continue;
        if (TextW(l.t, Body()) <= l.r.w) {
            c.Text(l.t, l.r, TS(Body(), u.text));
        } else {
            // 放不下（英文较长）时折成两行，垂直居中，字号不变
            TextStyle ts = TS(Body(), u.text);
            ts.wrap = true;
            c.Text(l.t, l.r, ts);
        }
    }
    Widget::Draw(c);
}

// ---------------------------------------------------------------- 弹出层

namespace {

class PopupContent {
public:
    virtual ~PopupContent() = default;
    virtual SizeF Size() = 0;
    virtual void Draw(Canvas &c, RectF r) = 0;
    virtual bool Move(float, float) { return false; }
    virtual bool Click(float, float) { return true; }  // 返回 true 关闭
    virtual void Cancel() {}
    virtual bool Interactive() { return true; }
};

void DestroyPopup(void *p);

class PopupWnd : public Window {
public:
    std::unique_ptr<PopupContent> content;
    LayeredSurface surface;
    UINT dpi = 96;
    POINT pos = {};
    SizeF size;
    bool closing = false;
    float margin = 22;

    void Show(RECT anchor, UINT d) {
        dpi = d;
        size = content->Size();
        float s = dpi / 96.f;
        int pw = (int)std::ceil((size.w + 2 * margin) * s), ph = (int)std::ceil((size.h + 2 * margin) * s);
        int m = (int)std::lround(margin * s), gap = (int)std::lround(4 * s);
        HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {};
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(mon, &mi);
        int x = anchor.left - m, y = anchor.bottom + gap - m;
        if (y + ph - m > mi.rcWork.bottom) y = anchor.top - gap - ph + m;
        if (x + pw - m > mi.rcWork.right) x = mi.rcWork.right - pw + m;
        if (x + m < mi.rcWork.left) x = mi.rcWork.left - m;
        pos = POINT{x, y};
        DWORD ex = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        if (!content->Interactive()) ex |= WS_EX_TRANSPARENT;
        if (!CreateWin(ex, WS_POPUP, x, y, pw, ph, nullptr, L"")) return;
        Render();
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        if (content->Interactive()) SetCapture(hwnd_);
    }
    void Render() {
        float s = dpi / 96.f;
        ID2D1RenderTarget *rt = surface.Begin((int)std::ceil((size.w + 2 * margin) * s),
                                              (int)std::ceil((size.h + 2 * margin) * s), dpi);
        if (!rt) return;
        {
            Canvas c(rt);
            content->Draw(c, RectF(margin, margin, size.w, size.h));
        }
        surface.End(hwnd_, pos.x, pos.y, 255);
    }
    void Close(bool cancelled) {
        if (closing) return;
        closing = true;
        if (cancelled) content->Cancel();
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
        app::Defer(&DestroyPopup, this);
    }
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) override {
        float s = dpi / 96.f;
        switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!closing && content->Move(MouseX(lp) / s - margin, MouseY(lp) / s - margin)) Render();
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            if (closing) return 0;
            float x = MouseX(lp) / s - margin, y = MouseY(lp) / s - margin;
            if (x >= 0 && y >= 0 && x < size.w && y < size.h) {
                if (content->Click(x, y)) Close(false);
            } else {
                Close(true);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (!closing && content->Interactive()) Close(true);
            return 0;
        }
        return Window::Handle(msg, wp, lp);
    }
};

std::vector<PopupWnd *> g_popups;
PopupWnd *g_info = nullptr;

void DestroyPopup(void *p) {
    PopupWnd *w = (PopupWnd *)p;
    for (size_t i = 0; i < g_popups.size(); ++i)
        if (g_popups[i] == w) g_popups.erase(g_popups.begin() + (long)i);
    if (g_info == w) g_info = nullptr;
    delete w;
}

PopupWnd *Open(Host *host, RectF anchor, PopupContent *content) {
    PopupWnd *w = new PopupWnd();
    w->content.reset(content);
    g_popups.push_back(w);
    w->Show(host->ScreenRect(anchor), host->Dpi());
    return w;
}

void Panel(Canvas &c, RectF r, float radius) {
    const auto &u = U();
    c.Shadow(r, radius, K().toolbar.shadowBlur, K().toolbar.shadowOffsetY, u.shadow);
    c.FillRound(r, radius, u.popoverBg);
    c.StrokeRound(r.Inflate(-0.5f), radius - 0.5f, u.border, 1);
}

class MenuList : public PopupContent {
public:
    std::vector<Option> options;
    std::string selected;
    float minWidth;
    int hover = -1;
    std::function<void(const std::string *)> done;
    bool fired = false;
    static constexpr float kRowH = 28, kPad = 5;

    SizeF Size() override {
        float tw = 80;
        for (auto &o : options) tw = std::fmax(tw, TextW(o.label, Body()));
        return SizeF{std::ceil(std::fmax(minWidth, tw + 52)), options.size() * kRowH + 2 * kPad};
    }
    RectF Row(RectF r, int i) { return RectF(r.x + kPad, r.y + kPad + i * kRowH, r.w - 2 * kPad, kRowH); }
    void Draw(Canvas &c, RectF r) override {
        const auto &u = U();
        Panel(c, r, 8);
        for (int i = 0; i < (int)options.size(); ++i) {
            RectF rr = Row(r, i);
            bool hot = i == hover && options[(size_t)i].enabled;
            if (hot) c.FillRound(rr, 5, u.accent);
            Color tc = !options[(size_t)i].enabled ? u.textTertiary : (hot ? u.accentText : u.text);
            if (options[(size_t)i].value == selected)
                c.Icon("check", rr.x + 15, rr.cy(), 14, hot ? u.accentText : u.accent, 2);
            c.Text(options[(size_t)i].label, RectF(rr.x + 30, rr.y, rr.w - 38, rr.h), TS(Body(), tc));
        }
    }
    bool Move(float x, float y) override {
        RectF r(0, 0, Size().w, Size().h);
        int h = -1;
        for (int i = 0; i < (int)options.size(); ++i)
            if (Row(r, i).Contains(x, y)) h = i;
        if (h == hover) return false;
        hover = h;
        return true;
    }
    bool Click(float x, float y) override {
        RectF r(0, 0, Size().w, Size().h);
        for (int i = 0; i < (int)options.size(); ++i) {
            if (Row(r, i).Contains(x, y)) {
                if (!options[(size_t)i].enabled) return false;
                fired = true;
                std::string v = options[(size_t)i].value;
                if (done) done(&v);
                return true;
            }
        }
        return false;
    }
    void Cancel() override {
        if (!fired && done) done(nullptr);
        fired = true;
    }
};

class PaletteGrid : public PopupContent {
public:
    std::string selected;
    int hover = -1;
    std::function<void(int, bool)> done;
    bool fired = false;
    float Sw() { return K().popover.swatch; }
    float Gap() { return K().popover.swatchGap; }
    float Pad() { return K().popover.padding; }
    SizeF Size() override { return SizeF{10 * Sw() + 9 * Gap() + 2 * Pad(), 2 * Pad() + Sw() + 8 + 24}; }
    RectF SwRect(int i) { return RectF(Pad() + i * (Sw() + Gap()), Pad(), Sw(), Sw()); }
    RectF Custom() { return RectF(Pad(), Pad() + Sw() + 8, Size().w - 2 * Pad(), 24); }
    void Draw(Canvas &c, RectF r) override {
        const auto &u = U();
        Panel(c, r, K().popover.radius);
        std::vector<Color> pal = style::Palette();
        std::string cur = selected.substr(0, 7);
        for (auto &ch : cur) ch = (char)toupper(ch);
        for (int i = 0; i < 10; ++i) {
            RectF s = SwRect(i).Offset(r.x, r.y);
            bool sel = ColorToHex(pal[(size_t)i], false) == cur;
            if (sel) c.StrokeEllipse(s.cx(), s.cy(), s.w / 2 + 3, s.w / 2 + 3, u.accent, 2);
            else if (hover == i) c.StrokeEllipse(s.cx(), s.cy(), s.w / 2 + 3, s.w / 2 + 3, u.border, 2);
            c.FillEllipse(s.cx(), s.cy(), s.w / 2, s.w / 2, pal[(size_t)i]);
            c.StrokeEllipse(s.cx(), s.cy(), s.w / 2 - 0.5f, s.w / 2 - 0.5f, Color{0, 0, 0, 0.12f}, 1);
        }
        RectF cr = Custom().Offset(r.x, r.y);
        if (hover == 100) c.FillRound(cr, 5, u.surfaceHover);
        c.Icon("palette", cr.x + 11, cr.cy(), 14, u.icon, 1.6f);
        c.Text(T("common.custom"), RectF(cr.x + 24, cr.y, cr.w - 28, cr.h), TS(Body() - 1, u.text));
    }
    bool Move(float x, float y) override {
        int h = -1;
        for (int i = 0; i < 10; ++i)
            if (SwRect(i).Inflate(3).Contains(x, y)) h = i;
        if (Custom().Contains(x, y)) h = 100;
        if (h == hover) return false;
        hover = h;
        return true;
    }
    bool Click(float x, float y) override {
        for (int i = 0; i < 10; ++i) {
            if (SwRect(i).Inflate(3).Contains(x, y)) {
                fired = true;
                if (done) done(i, true);
                return true;
            }
        }
        if (Custom().Contains(x, y)) {
            fired = true;
            // 系统取色框是模态的：先关掉气泡再弹
            std::function<void(int, bool)> d = done;
            app::Defer([](void *p) {
                auto *fn = (std::function<void(int, bool)> *)p;
                (*fn)(-1, true);
                delete fn;
            }, new std::function<void(int, bool)>(d));
            return true;
        }
        return false;
    }
    void Cancel() override { fired = true; }
};

class InfoBubble : public PopupContent {
public:
    std::wstring text;
    static constexpr float kMaxW = 280, kPadX = 12, kPadY = 10;
    SizeF Size() override {
        gfx::TextSize ts = gfx::Measure(text, Body() - 1, kRegular, false, kMaxW - 2 * kPadX);
        return SizeF{std::ceil(ts.w + 2 * kPadX), std::ceil(ts.h + 2 * kPadY)};
    }
    void Draw(Canvas &c, RectF r) override {
        Panel(c, r, K().popover.radius);
        TextStyle ts = TS(Body() - 1, U().text);
        ts.wrap = true;
        ts.v = VAlign::Top;
        c.Text(text, RectF(r.x + kPadX, r.y + kPadY, r.w - 2 * kPadX + 1, r.h - 2 * kPadY), ts);
    }
    bool Interactive() override { return false; }
};

}  // namespace

void ShowMenu(Host *host, RectF anchor, const std::vector<Option> &options, const std::string &selected, float minWidth,
              std::function<void(const std::string *)> onDone) {
    CloseAllPopups();
    MenuList *m = new MenuList();
    m->options = options;
    m->selected = selected;
    m->minWidth = minWidth;
    m->done = onDone;
    Open(host, anchor, m);
}

void ShowPalette(Host *host, RectF anchor, const std::string &selected, std::function<void(int, bool)> onDone) {
    CloseAllPopups();
    PaletteGrid *p = new PaletteGrid();
    p->selected = selected;
    p->done = onDone;
    Open(host, anchor, p);
}

void ShowInfoBubble(Host *host, RectF anchor, const std::wstring &text) {
    if (g_info && !g_info->closing) return;
    InfoBubble *b = new InfoBubble();
    b->text = text;
    g_info = Open(host, anchor, b);
}

void HideInfoBubble() {
    if (g_info) {
        g_info->Close(false);
        g_info = nullptr;
    }
}

void CloseAllPopups() {
    std::vector<PopupWnd *> copy = g_popups;
    for (auto *p : copy) p->Close(true);
    g_info = nullptr;
}

bool PickColor(HWND owner, std::string &hex, bool alpha) {
    static COLORREF custom[16] = {};
    Color c = ParseColor(hex, Color{0, 0, 0, 1});
    CHOOSECOLORW cc;
    std::memset(&cc, 0, sizeof cc);
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = owner;
    cc.rgbResult = RGB((int)(c.r * 255 + 0.5f), (int)(c.g * 255 + 0.5f), (int)(c.b * 255 + 0.5f));
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return false;
    Color out{GetRValue(cc.rgbResult) / 255.f, GetGValue(cc.rgbResult) / 255.f, GetBValue(cc.rgbResult) / 255.f, c.a};
    // 系统取色框没有透明度：保留原来的透明度
    hex = ColorToHex(out, alpha && hex.size() == 9);
    return true;
}

}  // namespace prefs
}  // namespace ls
