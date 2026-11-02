#include "../common/windows_compat.h"
#include "tag_manager_dialog.h"

#include "confirm_dialog_view.h"
#include "typography.h"
#include "window_helpers.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace luling::ui {
namespace {

constexpr wchar_t kTagManagerClass[] = L"LulingTagManagerWindow";
constexpr float kWidth = 520.0f;
constexpr float kPad = 24.0f;
constexpr float kTitleBar = 36.0f;
constexpr float kCloseW = 46.0f;
constexpr float kTabH = 32.0f;
constexpr float kRowH = 40.0f;
constexpr float kVisibleRows = 9.0f;
constexpr float kButtonMinW = 96.0f;
constexpr float kButtonGap = 8.0f;
constexpr float kFooterPadY = 18.0f;

enum : int { kNone = 0, kDone = 1, kClose = 2, kMerge = 3, kDelete = 4,
             kTabBase = 10, kRowBase = 40 };

float Dip(float scale, float value) { return value * scale; }

struct Layout {
    float width = 0.0f;
    float height = 0.0f;
    float title_bar = 0.0f;
    D2D1_RECT_F close{}, title{}, intro{}, list{};
    D2D1_RECT_F tabs[4]{};
    std::vector<D2D1_RECT_F> rows;
    D2D1_RECT_F footer{}, done{}, merge{}, remove{}, note{};
};

struct Visual {
    int hover = kNone;
    int pressed = kNone;
    bool show_focus = false;
    int focus = kDone;
};

Layout MakeLayout(const TagManagerSpec& spec, size_t visible, const fluent::Painter& painter,
                  IDWriteFactory2* factory, float scale) {
    (void)factory;
    Layout l;
    const float pad = Dip(scale, kPad);
    const float width = std::ceil(Dip(scale, kWidth));
    const float text_w = width - pad * 2.0f;
    l.title_bar = Dip(scale, kTitleBar);
    l.close = D2D1::RectF(width - Dip(scale, kCloseW), 0, width, l.title_bar);
    float y = Dip(scale, 28.0f);
    const float title_h = Dip(scale, 26.0f);
    l.title = D2D1::RectF(pad, y, width - pad, y + title_h);
    y = l.title.bottom + Dip(scale, 4.0f);
    const float intro_h = Dip(scale, 18.0f);
    l.intro = D2D1::RectF(pad, y, width - pad, y + intro_h);
    y = l.intro.bottom + Dip(scale, 14.0f);
    const float tab_w = text_w / 4.0f;
    for (int i = 0; i < 4; ++i)
        l.tabs[i] = D2D1::RectF(pad + i * tab_w, y, pad + (i + 1) * tab_w, y + Dip(scale, kTabH));
    y = l.tabs[0].bottom + Dip(scale, 10.0f);
    const float rows_h = Dip(scale, kVisibleRows * kRowH);
    l.list = D2D1::RectF(pad, y, width - pad, y + rows_h);
    for (size_t i = 0; i < visible && i < static_cast<size_t>(kVisibleRows); ++i)
        l.rows.push_back(D2D1::RectF(l.list.left, l.list.top + Dip(scale, static_cast<float>(i) * kRowH),
                                     l.list.right,
                                     l.list.top + Dip(scale, static_cast<float>(i + 1) * kRowH)));
    y = l.list.bottom + Dip(scale, 12.0f);
    l.note = D2D1::RectF(pad, y, width - pad, y + Dip(scale, 20.0f));
    y = l.note.bottom + Dip(scale, 12.0f);
    const float btn_h = painter.MeasureButtonHeight();
    l.footer = D2D1::RectF(0, y, width, y + btn_h + Dip(scale, kFooterPadY) * 2.0f);
    const float by = y + Dip(scale, kFooterPadY);
    const float done_w = std::max(painter.MeasureButtonWidth(spec.done_text), Dip(scale, kButtonMinW));
    l.done = D2D1::RectF(width - pad - done_w, by, width - pad, by + btn_h);
    const float del_w = std::max(painter.MeasureButtonWidth(spec.delete_text), Dip(scale, kButtonMinW));
    l.remove = D2D1::RectF(l.done.left - Dip(scale, kButtonGap) - del_w, by,
                           l.done.left - Dip(scale, kButtonGap), by + btn_h);
    const float merge_w = std::max(painter.MeasureButtonWidth(spec.merge_text), Dip(scale, kButtonMinW));
    l.merge = D2D1::RectF(l.remove.left - Dip(scale, kButtonGap) - merge_w, by,
                          l.remove.left - Dip(scale, kButtonGap), by + btn_h);
    l.width = width;
    l.height = std::ceil(l.footer.bottom);
    return l;
}

int HitTest(const Layout& l, float x, float y) {
    if (ContainsRect(l.close, x, y)) return kClose;
    if (ContainsRect(l.done, x, y)) return kDone;
    if (ContainsRect(l.merge, x, y)) return kMerge;
    if (ContainsRect(l.remove, x, y)) return kDelete;
    for (int i = 0; i < 4; ++i)
        if (ContainsRect(l.tabs[i], x, y)) return kTabBase + i;
    for (size_t i = 0; i < l.rows.size(); ++i)
        if (ContainsRect(l.rows[i], x, y)) return kRowBase + static_cast<int>(i);
    return kNone;
}

class TagManagerWindow {
public:
    bool Show(HWND owner, TagManagerSpec& spec, TagManagerResult& result, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        spec_ = spec;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(luling::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kTagManagerClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kTagManagerClass, &wc)) RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kTagManagerClass, spec_.title.c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
            static_cast<int>(kWidth * scale_), static_cast<int>(560 * scale_), owner, nullptr,
            wc.hInstance, this);
        if (!hwnd_) return false;
        RECT placed{};
        GetWindowRect(hwnd_, &placed);
        CenterOwnedWindow(hwnd_, owner_, placed.right - placed.left, placed.bottom - placed.top);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            RedirectStrayModalKey(message, hwnd_);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        if (accepted_) { spec = spec_; result = result_; }
        return accepted_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<TagManagerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<TagManagerWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    std::vector<size_t> VisibleRows() const {
        std::vector<size_t> out;
        for (size_t i = 0; i < spec_.rows.size(); ++i) {
            const TagManagerRow& row = spec_.rows[i];
            if (tab_ == 1 && row.count <= 0) continue;
            if (tab_ == 2 && row.count > 0) continue;
            if (tab_ == 3 && !row.invalid) continue;
            out.push_back(i);
        }
        if (tab_ == 1) {
            std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
                return spec_.rows[a].count > spec_.rows[b].count;
            });
        }
        return out;
    }

    void SizeToContent() {
        if (!hwnd_ || !compositor_.DwriteFactory()) return;
        layout_ = MakeLayout(spec_, 0, painter_, compositor_.DwriteFactory(), scale_);
        layout_ = MakeLayout(spec_, std::min(static_cast<size_t>(kVisibleRows), VisibleRows().size()),
                             painter_, compositor_.DwriteFactory(), scale_);
        const int width = static_cast<int>(std::ceil(layout_.width));
        const int height = static_cast<int>(std::ceil(layout_.height));
        sizing_ = true;
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        sizing_ = false;
        compositor_.Resize(width, height);
    }

    size_t SelectedCount() const {
        size_t n = 0;
        for (const auto& row : spec_.rows) n += row.checked ? 1u : 0u;
        return n;
    }

    void Complete(bool accepted, bool merge, bool remove) {
        result_ = {};
        if (accepted) {
            std::vector<std::wstring> chosen;
            for (auto& row : spec_.rows)
                if (row.checked) chosen.push_back(row.id);
            if (merge && chosen.size() >= 2) result_.merge_ids = chosen;
            else if (remove) result_.delete_ids = chosen;
        }
        accepted_ = accepted;
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    void Activate(int id) {
        if (id == kDone) { Complete(true, false, false); return; }
        if (id == kClose) { Complete(false, false, false); return; }
        if (id == kMerge) { if (SelectedCount() >= 2) Complete(true, true, false); else MessageBeep(MB_OK); return; }
        if (id == kDelete) { if (SelectedCount() >= 1) Complete(true, false, true); else MessageBeep(MB_OK); return; }
        if (id >= kTabBase && id < kTabBase + 4) {
            tab_ = id - kTabBase;
            scroll_ = 0;
            SizeToContent();
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (id >= kRowBase) {
            const size_t slot = static_cast<size_t>(id - kRowBase);
            const std::vector<size_t> visible = VisibleRows();
            const size_t index = slot + scroll_;
            if (index < visible.size()) {
                const size_t row = visible[index];
                spec_.rows[row].checked = !spec_.rows[row].checked;
            }
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_, scale_);
        Draw(theme);
        EndSurface(compositor_);
    }

    void Draw(const Theme& theme) {
        const float scale = painter_.Scale();
        fluent::ControlState close_state{};
        close_state.hovered = visual_.hover == kClose;
        painter_.DrawTitleBarButton(layout_.close, fluent::TitleBarButtonRole::Close, {}, close_state);
        painter_.DrawText(spec_.title, layout_.title, compositor_.TextFormat(), theme.text,
                          fluent::HorizontalAlignment::Left);
        if (!spec_.intro.empty() && compositor_.SmallFormat())
            painter_.DrawText(spec_.intro, layout_.intro, compositor_.SmallFormat(), theme.text_secondary,
                              fluent::HorizontalAlignment::Left);
        for (int i = 0; i < 4; ++i) {
            const auto& r = layout_.tabs[i];
            const bool active = i == tab_;
            if (!active && visual_.hover == kTabBase + i)
                painter_.FillRoundedRect(r, Dip(scale, 6.0f), theme.fill_hover);
            painter_.DrawText(spec_.tabs[i], r, compositor_.TextFormat(),
                              active ? theme.accent : theme.text_secondary,
                              fluent::HorizontalAlignment::Center);
            if (active) {
                const float w = compositor_.TextFormat()
                    ? std::ceil(typography::MeasureLine(&compositor_, compositor_.TextFormat(), spec_.tabs[i]))
                    : 0.0f;
                const float cx = (r.left + r.right) * 0.5f;
                painter_.FillRoundedRect(D2D1::RectF(cx - w * 0.5f, r.bottom - Dip(scale, 2.0f),
                                                     cx + w * 0.5f, r.bottom),
                                         Dip(scale, 1.0f), theme.accent);
            }
        }
        const std::vector<size_t> visible = VisibleRows();
        painter_.FillRoundedRect(layout_.list, Dip(scale, 8.0f), theme.surface_card);
        painter_.StrokeRoundedRect(layout_.list, Dip(scale, 8.0f), theme.stroke_card, Dip(scale, 1.0f));
        for (size_t slot = 0; slot < layout_.rows.size(); ++slot) {
            const size_t index = slot + scroll_;
            if (index >= visible.size()) break;
            const TagManagerRow& row = spec_.rows[visible[index]];
            const auto& r = layout_.rows[slot];
            const int id = kRowBase + static_cast<int>(slot);
            if (visual_.hover == id || visual_.pressed == id)
                painter_.FillRoundedRect(D2D1::RectF(r.left + Dip(scale, 4.0f), r.top,
                                                     r.right - Dip(scale, 4.0f), r.bottom),
                                         Dip(scale, 6.0f), theme.fill_hover);
            fluent::ControlState state{};
            state.enabled = true;
            state.checked = row.checked;
            state.hovered = visual_.hover == id;
            painter_.DrawCheckBox(D2D1::RectF(r.left + Dip(scale, 14.0f), r.top,
                                              r.left + Dip(scale, 34.0f), r.bottom), L"", state);
            D2D1_COLOR_F dot = HexColor(row.rgb);
            painter_.FillRoundedRect(D2D1::RectF(r.left + Dip(scale, 46.0f),
                                                 (r.top + r.bottom) * 0.5f - Dip(scale, 5.0f),
                                                 r.left + Dip(scale, 56.0f),
                                                 (r.top + r.bottom) * 0.5f + Dip(scale, 5.0f)),
                                     Dip(scale, 5.0f), dot);
            const float text_left = r.left + Dip(scale, 68.0f);
            const float count_w = Dip(scale, 60.0f);
            painter_.DrawText(row.name,
                              D2D1::RectF(text_left, r.top, r.right - Dip(scale, 80.0f), r.bottom),
                              compositor_.TextFormat(), theme.text, fluent::HorizontalAlignment::Left);
            if (compositor_.SmallFormat())
                painter_.DrawText(std::to_wstring(row.count),
                                  D2D1::RectF(r.right - count_w - Dip(scale, 12.0f), r.top,
                                              r.right - Dip(scale, 12.0f), r.bottom),
                                  compositor_.SmallFormat(),
                                  row.invalid ? HexColor(0xE06C6C) : theme.text_secondary,
                                  fluent::HorizontalAlignment::Right);
        }
        if (!spec_.invalid_note.empty() && compositor_.SmallFormat())
            painter_.DrawText(spec_.invalid_note, layout_.note, compositor_.SmallFormat(),
                              theme.text_secondary);
        painter_.FillRoundedRect(layout_.footer, 0, theme.surface_sheet);
        painter_.FillRoundedRect(D2D1::RectF(layout_.footer.left, layout_.footer.top,
                                             layout_.footer.right, layout_.footer.top + 1.0f),
                                 0, theme.stroke_divider);
        const auto state_for = [&](int id, bool enabled = true) {
            fluent::ControlState st{};
            st.enabled = enabled;
            st.hovered = visual_.hover == id;
            st.pressed = visual_.pressed == id;
            return st;
        };
        const bool two = SelectedCount() >= 2, one = SelectedCount() >= 1;
        painter_.DrawButton({layout_.merge, spec_.merge_text, {}, fluent::ButtonKind::Standard,
                             state_for(kMerge, two)});
        painter_.DrawButton({layout_.remove, spec_.delete_text, {}, fluent::ButtonKind::Standard,
                             state_for(kDelete, one)});
        painter_.DrawButton({layout_.done, spec_.done_text, {}, fluent::ButtonKind::Primary,
                             state_for(kDone)});
        const float inset = 0.5f;
        painter_.StrokeRoundedRect(D2D1::RectF(inset, inset, layout_.width - inset, layout_.height - inset),
                                  Dip(scale, 12.0f), theme.stroke_card);
    }

    int Hit(LPARAM lparam) const {
        return HitTest(layout_, static_cast<float>(GET_X_LPARAM(lparam)),
                       static_cast<float>(GET_Y_LPARAM(lparam)));
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(luling::compat::WindowDpi(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            SizeToContent();
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST: {
            const LRESULT hit = BorderlessHitTest(hwnd_, lparam, layout_.title_bar, layout_.close);
            return hit >= HTLEFT && hit <= HTBOTTOMRIGHT ? HTCLIENT : hit;
        }
        case WM_SIZE:
            if (!sizing_ && compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            if (const auto* suggested = reinterpret_cast<RECT*>(lparam))
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            SizeToContent();
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1;
            const int max_scroll = static_cast<int>(VisibleRows().size()) - static_cast<int>(kVisibleRows);
            const int next = std::clamp(static_cast<int>(scroll_) + delta, 0, (std::max)(0, max_scroll));
            if (next != static_cast<int>(scroll_)) {
                scroll_ = static_cast<size_t>(next);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int next = Hit(lparam);
            if (next != visual_.hover) {
                visual_.hover = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            visual_.hover = kNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            visual_.pressed = Hit(lparam);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(lparam);
            const int pressed = visual_.pressed;
            visual_.pressed = kNone;
            ReleaseCapture();
            if (hit == pressed && hit != kNone) Activate(hit);
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { Complete(false, false, false); return 0; }
            if (wparam == VK_RETURN) { Complete(true, false, false); return 0; }
            break;
        case WM_CLOSE:
            Complete(false, false, false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            compositor_.Shutdown();
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_{&compositor_};
    Layout layout_;
    Visual visual_;
    TagManagerSpec spec_;
    TagManagerResult result_;
    size_t scroll_ = 0;
    int tab_ = 0;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool accepted_ = false;
    bool done_ = false;
    bool sizing_ = false;
};

} // namespace

bool ShowTagManagerDialog(HWND owner, TagManagerSpec& spec, TagManagerResult& result, bool dark,
                          D2D1_COLOR_F accent) {
    TagManagerWindow window;
    return window.Show(owner, spec, result, dark, accent);
}

} // namespace luling::ui
