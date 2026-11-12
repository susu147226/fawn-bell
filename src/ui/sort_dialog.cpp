#include "../common/windows_compat.h"
#include "sort_dialog.h"

#include "window_helpers.h"
#include "fluent_components.h"
#include "../common/localization.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace luling::ui {
namespace {

constexpr wchar_t kSortClass[] = L"LulingSortWindow";
constexpr float kPanelW = 480.0f;
constexpr float kPanelH = 340.0f;
constexpr int kKeyCount = 6;
constexpr float kRowStep = 48.0f;
constexpr float kListViewLeft = 268.0f;
// The selected card never moves: the keys slide through it, so the list reads
// as a wheel picker (Pulse's panel behaves the same way).
constexpr int kFocusRow = 2;

struct SortKeyDef {
    int column;
    const wchar_t* title;
    const wchar_t* desc;
    const wchar_t* glyph;
};

const SortKeyDef& KeyAt(int index);
const SortKeyDef kKeys[kKeyCount] = {
    {0, L"名称", L"按文件名的字母顺序", L"\xE8CB"},
    {1, L"修改日期", L"按最后一次修改的时间", L"\xE823"},
    {5, L"创建日期", L"按文件创建的时间", L"\xE787"},
    {3, L"大小", L"按文件大小", L"\xE7C3"},
    {2, L"类型", L"按文件类型", L"\xE8A5"},
    {6, L"访问日期", L"按最后一次打开的时间", L"\xE823"},
};
const SortKeyDef& KeyAt(int index) {
    return kKeys[std::clamp(index, 0, kKeyCount - 1)];
}

D2D1_COLOR_F WithAlpha(D2D1_COLOR_F color, float alpha) {
    color.a = alpha;
    return color;
}

// The preview card's tint follows the sort key, so switching keys reads as one
// motion rather than a jump.
D2D1_COLOR_F TintFor(int column) {
    switch (column) {
    case 1: return D2D1::ColorF(0x10B981);
    case 5: return D2D1::ColorF(0x0EA5E9);
    case 6: return D2D1::ColorF(0x6366F1);
    case 3: return D2D1::ColorF(0xF59E0B);
    case 2: return D2D1::ColorF(0xEC4899);
    default: return D2D1::ColorF(0x8B5CF6);
    }
}

D2D1_COLOR_F MixColor(D2D1_COLOR_F a, const D2D1_COLOR_F& b, float t) {
    a.r += (b.r - a.r) * t;
    a.g += (b.g - a.g) * t;
    a.b += (b.b - a.b) * t;
    return a;
}

struct SortLayout {
    D2D1_RECT_F rows[kKeyCount]{};
    D2D1_RECT_F preview{};
    D2D1_RECT_F direction_label{};
    D2D1_RECT_F direction[2]{};
    D2D1_RECT_F folder_label{};
    D2D1_RECT_F folder[3]{};
    D2D1_RECT_F cancel{};
    D2D1_RECT_F apply{};
};

SortLayout MakeSortLayout() {
    SortLayout layout;
    const float pad = 12.0f;
    const float left = pad;
    const float left_w = 244.0f;
    for (int i = 0; i < kKeyCount; ++i) {
        const float top = pad + static_cast<float>(i) * kRowStep;
        layout.rows[i] = D2D1::RectF(left, top, left + left_w, top + 44.0f);
    }
    const float right = 268.0f;
    const float right_w = kPanelW - pad - right;
    layout.preview = D2D1::RectF(right, pad, right + right_w, pad + 120.0f);
    layout.direction_label = D2D1::RectF(right, 142.0f, right + right_w, 160.0f);
    const float seg_h = 30.0f;
    const float seg_gap = 6.0f;
    const float dir_w = (right_w - seg_gap) * 0.5f;
    layout.direction[0] = D2D1::RectF(right, 164.0f, right + dir_w, 164.0f + seg_h);
    layout.direction[1] = D2D1::RectF(right + dir_w + seg_gap, 164.0f, right + right_w,
                                      164.0f + seg_h);
    layout.folder_label = D2D1::RectF(right, 202.0f, right + right_w, 220.0f);
    const float fold_w = (right_w - seg_gap * 2.0f) / 3.0f;
    for (int i = 0; i < 3; ++i) {
        const float x = right + static_cast<float>(i) * (fold_w + seg_gap);
        layout.folder[i] = D2D1::RectF(x, 224.0f, x + fold_w, 224.0f + seg_h);
    }
    const float footer_y = kPanelH - 60.0f;
    layout.cancel = D2D1::RectF(kPanelW - pad - 190.0f, footer_y, kPanelW - pad - 100.0f,
                                footer_y + 34.0f);
    layout.apply = D2D1::RectF(kPanelW - pad - 92.0f, footer_y, kPanelW - pad,
                               footer_y + 34.0f);
    return layout;
}

class SortWindow {
public:
    bool Show(HWND owner, POINT anchor, SortDialogSpec& spec, bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        spec_ = spec;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(luling::compat::WindowDpi(owner ? owner : GetDesktopWindow())) /
                 96.0f;
        layout_ = MakeSortLayout();
        // The chosen key starts inside the fixed card.
        for (int i = 0; i < kKeyCount; ++i) {
            if (KeyAt(i).column != spec.column) continue;
            focus_index_ = i;
            break;
        }
        scroll_pos_ = scroll_target_ = static_cast<float>(focus_index_) * kRowStep;

        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kSortClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kSortClass, &wc)) RegisterClassExW(&wc);

        // A drop-down under the Sort button, not a centered dialog: it takes
        // the keyboard while open, but the window behind stays usable.
        const int width = static_cast<int>(kPanelW * scale_);
        const int height = static_cast<int>(kPanelH * scale_);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int x = anchor.x;
        int y = anchor.y + static_cast<int>(4 * scale_);
        const int work_left = static_cast<int>(work.left);
        const int work_top = static_cast<int>(work.top);
        const int work_right = static_cast<int>(work.right);
        const int work_bottom = static_cast<int>(work.bottom);
        x = std::clamp(x, work_left, std::max(work_left, work_right - width));
        y = std::clamp(y, work_top, std::max(work_top, work_bottom - height));
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, kSortClass, L"",
            WS_POPUP, x, y, width, height, owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return false;
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        UpdateWindow(hwnd_);
        // Expand out of the Sort button, the way Pulse's drop-down does.
        if (compositor_.Dc())
            compositor_.PlayZoom(static_cast<float>(width) * 0.06f, 0.0f, true, 0.16f);

        MSG message{};
        while (!done_) {
            const DWORD wait = MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT,
                                                           MWMO_INPUTAVAILABLE);
            if (wait == WAIT_TIMEOUT) continue;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    done_ = true;
                    break;
                }
                RedirectStrayModalKey(message, hwnd_);
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (IsWindow(hwnd_)) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
        if (accepted_) spec = spec_;
        return accepted_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<SortWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<SortWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void Finish(bool accepted) {
        if (finishing_) return;
        finishing_ = true;
        accepted_ = accepted;
        done_ = true;
        if (hwnd_) {
            KillTimer(hwnd_, 1);
            KillTimer(hwnd_, 2);
            HideComposedDialog(hwnd_, owner_);
            DestroyWindow(hwnd_);
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
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                     scale_);
        Draw(theme, high_contrast);
        EndSurface(compositor_);
    }

    void Draw(const Theme& theme, bool high_contrast) {
        const float s = scale_;
        auto* dc = compositor_.Dc();
        IDWriteTextFormat* body = compositor_.TextFormat();
        IDWriteTextFormat* small_fmt = compositor_.SmallFormat();
        painter_.FillRoundedRect(D2D1::RectF(0, 0, kPanelW * s, kPanelH * s), 0, theme.bg);

        // Sort keys: the accent card is pinned to the middle row and the keys
        // slide through it, so scrolling moves the list and not the frame.
        const D2D1_RECT_F list_area = D2D1::RectF(0, 12 * s, kListViewLeft * s,
                                                  (kPanelH - 60.0f) * s);
        const float focus_top = layout_.rows[kFocusRow].top;
        dc->PushAxisAlignedClip(list_area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (int i = 0; i < kKeyCount; ++i) {
            const float top_dip = focus_top + static_cast<float>(i) * kRowStep - scroll_pos_;
            const D2D1_RECT_F row{layout_.rows[i].left * s, top_dip * s,
                                  layout_.rows[i].right * s, (top_dip + 44.0f) * s};
            const bool selected = i == focus_index_;
            const bool hot = hovered_ == i;
            if (selected) {
                const D2D1_RECT_F card{D2D1::RectF(layout_.rows[i].left * s,
                                                   layout_.rows[kFocusRow].top * s,
                                                   layout_.rows[i].right * s,
                                                   (layout_.rows[kFocusRow].top + 44.0f) * s)};
                painter_.FillRoundedRect(card, 8 * s, theme.surface_card);
                painter_.StrokeRoundedRect(card, 8 * s, theme.accent, 1.0f * s);
                painter_.FillRoundedRect(D2D1::RectF(card.left + 1 * s, card.top + 10 * s,
                                                     card.left + 3 * s, card.bottom - 10 * s),
                                         1 * s, theme.accent);
            } else if (hot) {
                painter_.FillRoundedRect(row, 8 * s, theme.fill_hover);
            }
            const D2D1_RECT_F icon_rc = D2D1::RectF(row.left + 8 * s, row.top + 8 * s,
                                                    row.left + 36 * s, row.top + 36 * s);
            if (selected) painter_.FillRoundedRect(icon_rc, 14 * s, WithAlpha(theme.accent, 0.16f));
            painter_.DrawGlyph(KeyAt(i).glyph,
                D2D1::RectF(icon_rc.left + 6 * s, icon_rc.top + 6 * s, icon_rc.right - 6 * s,
                            icon_rc.bottom - 6 * s),
                selected ? theme.accent : theme.text_secondary);
            painter_.DrawText(KeyAt(i).title,
                D2D1::RectF(row.left + 46 * s, row.top + 4 * s, row.right - 12 * s,
                            row.top + 23 * s),
                body, selected ? theme.text : theme.text_secondary);
            const std::wstring desc = selected
                ? std::wstring(l10n::Pick(L"当前 · ", L"Current · ")) + KeyAt(i).desc
                : std::wstring(KeyAt(i).desc);
            painter_.DrawText(desc,
                D2D1::RectF(row.left + 46 * s, row.top + 21 * s, row.right - 12 * s,
                            row.bottom - 4 * s),
                small_fmt, theme.text_secondary);
            if (selected && hot) {
                // Hovering the active key offers the one-click apply, as Pulse does.
                const D2D1_RECT_F pill = D2D1::RectF(row.right - 84 * s, row.top + 9 * s,
                                                     row.right - 10 * s, row.bottom - 9 * s);
                painter_.FillRoundedRect(pill, (pill.bottom - pill.top) * 0.5f, theme.accent);
                painter_.DrawText(l10n::Pick(L"单击应用", L"Apply now"), pill, small_fmt,
                                  D2D1::ColorF(1.0f, 1.0f, 1.0f),
                                  fluent::HorizontalAlignment::Center);
            }
        }
        dc->PopAxisAlignedClip();

        // Thin scrollbar beside the list, sized to how much of the key list is
        // in view.
        {
            const float view = (kPanelH - 60.0f - 12.0f) * s;
            const float track_x = kListViewLeft * s - 6 * s;
            const float track_top = 12 * s;
            painter_.FillRoundedRect(
                D2D1::RectF(track_x, track_top, track_x + 2 * s, track_top + view), 1 * s,
                WithAlpha(theme.text_secondary, 0.10f));
            const float thumb_h = std::max(24 * s, view * 4.0f / kKeyCount);
            const float ratio = std::clamp(
                scroll_pos_ / (kRowStep * static_cast<float>(kKeyCount - 1)), 0.0f, 1.0f);
            const float thumb_y = track_top + (view - thumb_h) * ratio;
            painter_.FillRoundedRect(
                D2D1::RectF(track_x, thumb_y, track_x + 2 * s, thumb_y + thumb_h), 1 * s,
                WithAlpha(theme.text_secondary, 0.45f));
        }

        // Sample list: how the chosen order reads at a glance.
        const D2D1_RECT_F preview{layout_.preview.left * s, layout_.preview.top * s,
                                  layout_.preview.right * s, layout_.preview.bottom * s};
        const D2D1_COLOR_F tint = AnimatedTint();
        painter_.FillRoundedRect(preview, 10 * s, WithAlpha(tint, 0.42f));
        painter_.FillRoundedRect(D2D1::RectF(preview.left, preview.top + preview.bottom * 0.45f,
                                             preview.right, preview.bottom),
                                 10 * s, WithAlpha(tint, 0.75f));
        struct Sample {
            const wchar_t* name;
            const wchar_t* kind;
        };
        const Sample samples[5] = {
            {L"项目资料", L"文件夹"}, {L"照片", L"文件夹"}, {L"报告.pdf", L"PDF"},
            {L"会议记录.docx", L"DOCX"}, {L"旅行照片.jpg", L"JPG"},
        };
        const D2D1_COLOR_F dots[5] = {
            D2D1::ColorF(0xFBBF24), D2D1::ColorF(0xFBBF24), D2D1::ColorF(0xF472B6),
            D2D1::ColorF(0xFB7185), D2D1::ColorF(0x60A5FA),
        };
        const bool show_times = spec_.column == 1 || spec_.column == 5 || spec_.column == 6;
        const bool show_sizes = spec_.column == 3;
        const wchar_t* times[5] = {L"10:31", L"11:12", L"09-26", L"10-03", L"11-05"};
        const wchar_t* sizes[5] = {L"—", L"—", L"2.4 MB", L"88 KB", L"6.1 MB"};
        for (int i = 0; i < 5; ++i) {
            const float y = preview.top + 12 * s + static_cast<float>(i) * 21 * s;
            painter_.FillRoundedRect(D2D1::RectF(preview.left + 10 * s, y + 3 * s,
                                                preview.left + 17 * s, y + 10 * s),
                                     3 * s, dots[i]);
            painter_.DrawText(samples[i].name,
                D2D1::RectF(preview.left + 24 * s, y, preview.right - 56 * s, y + 18 * s),
                small_fmt, theme.text);
            const wchar_t* right = show_times ? times[i] : show_sizes ? sizes[i] : samples[i].kind;
            painter_.DrawText(right,
                D2D1::RectF(preview.right - 54 * s, y, preview.right - 10 * s, y + 18 * s),
                small_fmt, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f));
        }

        // 方向 and 文件夹 segments.
        auto label = [&](const D2D1_RECT_F& rc, const wchar_t* text) {
            painter_.DrawText(text,
                D2D1::RectF(rc.left * s, rc.top * s, rc.right * s, rc.bottom * s), small_fmt,
                theme.text_secondary);
        };
        label(layout_.direction_label, l10n::Pick(L"方向", L"Direction"));
        label(layout_.folder_label, l10n::Pick(L"文件夹", L"Folders"));
        auto segments = [&](const D2D1_RECT_F* rects, int count, const wchar_t* const* texts,
                            int selected, int hover_base) {
            const D2D1_RECT_F track = D2D1::RectF(rects[0].left * s, rects[0].top * s,
                                                  rects[count - 1].right * s,
                                                  rects[0].bottom * s);
            painter_.DrawSegmentedTrack(track);
            for (int i = 0; i < count; ++i) {
                fluent::SegmentedItemSpec item;
                item.bounds = D2D1::RectF(rects[i].left * s, rects[i].top * s,
                                          rects[i].right * s, rects[i].bottom * s);
                item.text = texts[i];
                item.position = i == 0 ? fluent::SegmentPosition::First
                                : i == count - 1 ? fluent::SegmentPosition::Last
                                                 : fluent::SegmentPosition::Middle;
                item.shared_track = true;
                item.state.selected = selected == i;
                item.state.hovered = hovered_ == hover_base + i;
                painter_.DrawSegmentedItem(item);
            }
        };
        const wchar_t* direction_texts[2] = {
            l10n::Pick(L"↑ A→Z", L"↑ A→Z"),
            l10n::Pick(L"↓ Z→A", L"↓ Z→A"),
        };
        segments(layout_.direction, 2, direction_texts, spec_.direction, 10);
        const wchar_t* folder_texts[3] = {
            l10n::Pick(L"置顶", L"Top"),
            l10n::Pick(L"跟随方向", L"Follow"),
            l10n::Pick(L"混排", L"Mixed"),
        };
        segments(layout_.folder, 3, folder_texts, spec_.folder_mode, 20);

        // 取消 / 应用.
        {
            fluent::ControlState cancel_state{};
            cancel_state.enabled = true;
            cancel_state.hovered = hovered_ == 30;
            painter_.DrawButton({D2D1::RectF(layout_.cancel.left * s, layout_.cancel.top * s,
                                             layout_.cancel.right * s,
                                             layout_.cancel.bottom * s),
                                 l10n::Pick(L"取消", L"Cancel"), {}, fluent::ButtonKind::Standard,
                                 cancel_state});
            fluent::ControlState apply_state{};
            apply_state.enabled = true;
            apply_state.hovered = hovered_ == 31;
            painter_.DrawButton({D2D1::RectF(layout_.apply.left * s, layout_.apply.top * s,
                                             layout_.apply.right * s, layout_.apply.bottom * s),
                                 l10n::Pick(L"应用", L"Apply"), {},
                                 fluent::ButtonKind::Primary, apply_state});
        }
        (void)high_contrast;
    }

    int Hit(float x, float y) const {
        const float focus_top = layout_.rows[kFocusRow].top;
        for (int i = 0; i < kKeyCount; ++i) {
            const float top = focus_top + static_cast<float>(i) * kRowStep - scroll_pos_;
            if (Contains(D2D1::RectF(layout_.rows[i].left, top, layout_.rows[i].right,
                                     top + 44.0f),
                         x, y))
                return i;
        }
        for (int i = 0; i < 2; ++i)
            if (Contains(layout_.direction[i], x, y)) return 10 + i;
        for (int i = 0; i < 3; ++i)
            if (Contains(layout_.folder[i], x, y)) return 20 + i;
        if (Contains(layout_.cancel, x, y)) return 30;
        if (Contains(layout_.apply, x, y)) return 31;
        return -1;
    }

    static bool Contains(const D2D1_RECT_F& rc, float x, float y) {
        return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
    }

    D2D1_COLOR_F AnimatedTint() const {
        if (tint_start_ == 0) return TintFor(spec_.column);
        const float t = std::clamp((GetTickCount64() - tint_start_) / 160.0f, 0.0f, 1.0f);
        if (t >= 1.0f) return tint_to_;
        const float eased = 1.0f - (1.0f - t) * (1.0f - t);
        return MixColor(tint_from_, tint_to_, eased);
    }

    void StartTintAnimation(int column) {
        tint_from_ = AnimatedTint();
        tint_to_ = TintFor(column);
        tint_start_ = GetTickCount64();
        SetTimer(hwnd_, 1, 16, nullptr);
    }

    // How far the keys are still sliding towards the fixed card. The position
    // chases the target continuously, so fast wheel input never restarts the
    // motion and the list never stalls between steps.
    void StepScroll() {
        const ULONGLONG now = GetTickCount64();
        const float dt = std::clamp(static_cast<float>(now - last_scroll_tick_) / 1000.0f,
                                    0.0f, 0.05f);
        last_scroll_tick_ = now;
        const float k = 1.0f - std::exp(-dt / 0.055f);
        scroll_pos_ += (scroll_target_ - scroll_pos_) * k;
        if (std::fabs(scroll_target_ - scroll_pos_) < 0.4f) {
            scroll_pos_ = scroll_target_;
            last_scroll_tick_ = 0;
            KillTimer(hwnd_, 2);
        }
    }

    void MoveFocusTo(int index) {
        index = std::clamp(index, 0, kKeyCount - 1);
        if (index == focus_index_) return;
        focus_index_ = index;
        scroll_target_ = static_cast<float>(index) * kRowStep;
        spec_.column = KeyAt(index).column;
        StartTintAnimation(spec_.column);
        if (last_scroll_tick_ == 0) last_scroll_tick_ = GetTickCount64();
        SetTimer(hwnd_, 2, 16, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void Activate(int id) {
        if (id >= 0 && id < kKeyCount) {
            // Clicking the key already inside the card applies it (单击应用).
            if (id == focus_index_) {
                Finish(true);
                return;
            }
            MoveFocusTo(id);
        } else if (id == 10 || id == 11) spec_.direction = id - 10;
        else if (id >= 20 && id < 23) spec_.folder_mode = id - 20;
        else if (id == 30) Finish(false);
        else if (id == 31) Finish(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
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
            compositor_.Resize(static_cast<int>(kPanelW * scale_),
                               static_cast<int>(kPanelH * scale_));
            return 0;
        case WM_ACTIVATE:
            // A drop-down closes as soon as the click lands outside it.
            if (LOWORD(wparam) == WA_INACTIVE && !done_ && !finishing_) Finish(false);
            return 0;
        case WM_TIMER:
            if (wparam == 1) {
                if (tint_start_ != 0 && GetTickCount64() - tint_start_ >= 160) {
                    KillTimer(hwnd_, 1);
                    tint_start_ = 0;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else if (wparam == 2) {
                StepScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &pt);
            if (pt.x < static_cast<int>(kListViewLeft * scale_)) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
                const int mag = delta < 0 ? -delta : delta;
                const int steps = std::max(1, mag / 120);
                MoveFocusTo(focus_index_ + (delta > 0 ? -steps : steps));
            }
            return 0;
        }
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, 0.0f, D2D1::RectF());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            return 0;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wparam) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            if (const auto* suggested = reinterpret_cast<RECT*>(lparam))
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
            compositor_.Resize(static_cast<int>(kPanelW * scale_),
                               static_cast<int>(kPanelH * scale_));
            return 0;
        }
        case WM_MOUSEMOVE: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam)) / scale_;
            const float y = static_cast<float>(GET_Y_LPARAM(lparam)) / scale_;
            const int next = Hit(x, y);
            if (next != hovered_) {
                hovered_ = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hovered_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)) / scale_,
                           static_cast<float>(GET_Y_LPARAM(lparam)) / scale_);
            SetCapture(hwnd_);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)) / scale_,
                                static_cast<float>(GET_Y_LPARAM(lparam)) / scale_);
            const int pressed = pressed_;
            pressed_ = -1;
            ReleaseCapture();
            if (hit == pressed) Activate(hit);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) Finish(false);
            else if (wparam == VK_RETURN) Finish(true);
            return 0;
        case WM_CLOSE:
            Finish(false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd_, message, wparam, lparam);
        }
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    SortDialogSpec spec_{};
    bool accepted_ = false;
    bool done_ = false;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    D2D1_COLOR_F accent_{};
    float scale_ = 1.0f;
    SortLayout layout_{};
    int hovered_ = -1;
    int pressed_ = -1;
    int focus_index_ = 0;
    float scroll_pos_ = 0.0f;     // current key-list offset, dip
    float scroll_target_ = 0.0f;  // offset the list is heading to, dip
    ULONGLONG last_scroll_tick_ = 0;
    bool finishing_ = false;
    D2D1_COLOR_F tint_from_{};
    D2D1_COLOR_F tint_to_{};
    ULONGLONG tint_start_ = 0;
    Compositor compositor_;
    fluent::Painter painter_;
};

} // namespace

bool ShowSortDialog(HWND owner, POINT anchor, SortDialogSpec& spec, bool dark,
                    D2D1_COLOR_F accent) {
    SortWindow window;
    return window.Show(owner, anchor, spec, dark, accent);
}

} // namespace luling::ui
