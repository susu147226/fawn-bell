// ui_settings_pages.cpp - Settings pages split like Pulse 1.0.58: 外观,
// 文件列表, 标签页与窗口, 启动与集成, 文件操作 and the shortcut list. The rows
// themselves reuse ui_settings_core.cpp's controls, laid out by
// settings_pages_layout.h.
#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "bloom_accent_picker.h"
#include "../common/localization.h"

namespace luling::ui {

void MainRenderer::DrawSettingsPages(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                     const Theme& theme) {
    using I = l10n::StringId;
    using H = HitTestResult;
    auto* dc = compositor_->Dc();
    const auto lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const float s = scale_;
    auto text = [&](const std::wstring& value, D2D1_RECT_F r, bool is_small = false) {
        painter_.DrawText(value, r, is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),
                          is_small ? theme.text_secondary : theme.text);
    };
    auto draw_card = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top || r.right <= r.left) return;
        MakeBrush(dc, WithAlpha(theme.surface_card, card_alpha_), brFillInput_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8 * scale_, 8 * scale_), brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 8 * scale_, 8 * scale_), brStrokeCard_.get(), 1);
    };
    auto span = [](const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
        return D2D1::RectF(a.left, a.top, a.right, b.bottom);
    };
    auto divider = [&](D2D1_RECT_F r) {
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), r.left + 16 * scale_, r.bottom, r.right - r.left - 32 * scale_, 1);
    };
    auto label = [&](D2D1_RECT_F r, const std::wstring& title, const std::wstring& desc,
                     const wchar_t* icon, float text_right = 0.0f, float description_height = 21.0f) {
        const float right = text_right > 0 ? text_right : r.right - 72 * scale_;
        DrawIconText(r.left + 16 * scale_, r.top + 17 * scale_, 24 * scale_, 24 * scale_, icon, L"",
                     theme.text_secondary, 0.85f);
        text(title, D2D1::RectF(r.left + 54 * scale_, r.top + 10 * scale_, right, r.top + 34 * scale_));
        if (!desc.empty()) {
            const auto bounds = D2D1::RectF(r.left + 54 * scale_, r.top + 35 * scale_, right,
                                            r.top + (35 + description_height) * scale_);
            if (description_height <= 21.0f) {
                text(desc, bounds, true);
            } else {
                ComPtr<IDWriteTextLayout> layout;
                if (SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(
                        desc.c_str(), static_cast<UINT32>(desc.size()), compositor_->SmallFormat(),
                        bounds.right - bounds.left, bounds.bottom - bounds.top, &layout))) {
                    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
                    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                    dc->DrawTextLayout(D2D1::Point2F(bounds.left, bounds.top), layout.get(),
                                       brTextSecondary_.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    };
    auto toggle = [&](D2D1_RECT_F r, I title, I desc, const wchar_t* icon, bool on, int hit) {
        if (IsHovered(vm, H::SettingsToggle, hit)) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), r.left + 2 * scale_, r.top + 2 * scale_,
                            r.right - r.left - 4 * scale_, r.bottom - r.top - 4 * scale_, 6 * scale_);
        }
        label(r, l10n::Get(title), l10n::Get(desc), icon, 0.0f, hit == 15 ? 43.0f : 21.0f);
        fluent::ControlState state{};
        state.checked = on;
        state.hovered = IsHovered(vm, H::SettingsToggle, hit);
        painter_.DrawSwitch(D2D1::RectF(r.right - 60 * scale_, r.top + 16 * scale_,
                                        r.right - 16 * scale_, r.top + 48 * scale_), L"", state);
    };
    auto button = [&](D2D1_RECT_F r, const std::wstring& value, H::Region region, int index,
                      bool primary = false, bool enabled = true) {
        fluent::ControlState st{};
        st.enabled = enabled;
        st.hovered = enabled && IsHovered(vm, region, index);
        painter_.DrawButton({r, value, {}, primary ? fluent::ButtonKind::Primary : fluent::ButtonKind::Standard, st});
    };
    auto segmented = [&](D2D1_RECT_F card, D2D1_RECT_F const* choices, const I* labels,
                         const int* values, int current, H::Region hit, I title, I desc,
                         int count = 3, const wchar_t* icon = L"\xE8A4") {
        const bool stacked = choices[0].left < card.left + 100 * scale_;
        label(card, l10n::Get(title), l10n::Get(desc), icon,
              stacked ? card.right - 16 * scale_ : choices[0].left - 12 * scale_);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left, choices[0].top,
                                                choices[count - 1].right, choices[count - 1].bottom));
        for (int i = 0; i < count; ++i) {
            fluent::SegmentedItemSpec item{};
            item.bounds = choices[i];
            item.text = l10n::Get(labels[i]);
            item.state.checked = current == values[i];
            item.state.hovered = IsHovered(vm, hit, i);
            item.shared_track = true;
            item.position = i == 0 ? fluent::SegmentPosition::First
                           : i == count - 1 ? fluent::SegmentPosition::Last
                                            : fluent::SegmentPosition::Middle;
            painter_.DrawSegmentedItem(item);
        }
    };
    auto slider = [&](D2D1_RECT_F card, D2D1_RECT_F const* cells, int value, int max_value,
                      const int* ticks, const I* tick_labels, H::Region hit, I title, I desc,
                      const wchar_t* unit) {
        const bool stacked = cells[0].left < card.left + 100 * scale_;
        label(card, l10n::Get(title), l10n::Get(desc), L"\xE8A4",
              stacked ? card.right - 16 * scale_ : cells[0].left - 12 * scale_);
        const auto g = SettingsSlider(cells, scale_);
        const bool hot = vm.hover_region == static_cast<int>(hit);
        const float span = (std::max)(1.0f, g.right - g.left);
        const auto x_of = [&](int v) {
            return g.left + span * std::clamp(static_cast<float>(v) / static_cast<float>(max_value), 0.0f, 1.0f);
        };
        const float x = x_of(value);
        const float th = 4 * scale_;
        MakeBrush(dc, WithAlpha(theme.text, 0.16f), brFillHover_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g.left, g.cy - th / 2, g.right, g.cy + th / 2),
                                                   th / 2, th / 2), brFillHover_.get());
        MakeBrush(dc, theme.accent, brAccent_);
        if (x > g.left)
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g.left, g.cy - th / 2, x, g.cy + th / 2),
                                                       th / 2, th / 2), brAccent_.get());
        for (int i = 0; i < 3; ++i) {
            if (ticks[i] < 0) continue;
            const float tx = x_of(ticks[i]);
            MakeBrush(dc, WithAlpha(theme.text_secondary, value == ticks[i] ? 0.9f : 0.55f), brTextSecondary_);
            if (ticks[i] > 0)
                dc->FillRectangle(D2D1::RectF(tx - 0.5f * scale_, g.cy + 5 * scale_, tx + 0.5f * scale_,
                                              g.cy + 8 * scale_), brTextSecondary_.get());
            const auto lr = ticks[i] == 0
                ? D2D1::RectF(g.left - 9 * scale_, g.cy + 8 * scale_, g.left + 60 * scale_, g.cy + 22 * scale_)
                : D2D1::RectF(tx - 34 * scale_, g.cy + 8 * scale_, tx + 34 * scale_, g.cy + 22 * scale_);
            painter_.DrawText(l10n::Get(tick_labels[i]), lr, compositor_->SmallFormat(),
                              value == ticks[i] ? theme.text : theme.text_secondary,
                              ticks[i] == 0 ? fluent::HorizontalAlignment::Left
                                            : fluent::HorizontalAlignment::Center);
        }
        MakeBrush(dc, WithAlpha(theme.surface_card, 1.0f), brFillInput_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        const float outer = 9 * scale_;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, g.cy), outer, outer), brFillInput_.get());
        dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, g.cy), outer, outer), brStrokeCard_.get(), 1);
        const float inner = (hot ? 6.0f : 5.0f) * scale_;
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, g.cy), inner, inner), brAccent_.get());
        painter_.DrawText(std::to_wstring(value) + unit,
                          D2D1::RectF(g.right + 10 * scale_, g.cy - 10 * scale_, cells[2].right, g.cy + 10 * scale_),
                          compositor_->TextFormat(), theme.text, fluent::HorizontalAlignment::Right);
    };
    auto dropdown = [&](D2D1_RECT_F r, D2D1_RECT_F c, I title, I desc, const std::wstring& value, int id) {
        label(r, l10n::Get(title), l10n::Get(desc), id == 0 ? L"\xE790" : L"\xE8C1",
              c.left < r.left + 100 * scale_ ? r.right - 16 * scale_ : c.left - 12 * scale_);
        fluent::ButtonSpec control{};
        control.bounds = c;
        control.text = value;
        control.drop_down = true;
        control.state.hovered = IsHovered(vm, H::SettingsDropdown, id);
        painter_.DrawButton(control);
    };
    auto section_title = [&](int i, const wchar_t* title) { text(title, lay.section[i]); };

    const int page = vm.settings_page;
    if (page == 13) {
        if (!vm.settings_shortcuts) return;
        size_t row_index = 0, title_index = 0;
        for (const auto& group : *vm.settings_shortcuts) {
            if (title_index >= lay.shortcut_titles.size()) break;
            const auto& card_rc = lay.shortcut_cards[title_index];
            const auto& title_rc = lay.shortcut_titles[title_index++];
            const size_t first_row = row_index;
            row_index += group.rows.size();
            if (row_index > lay.shortcut_rows.size()) continue;
            draw_card(card_rc);
            text(group.title, title_rc);
            // The category badge counts its shortcuts, like Pulse's cards.
            {
                const std::wstring count = std::to_wstring(group.rows.size());
                const float w = painter_.MeasureBadgeWidth(count);
                const D2D1_RECT_F badge = D2D1::RectF(title_rc.right - w, title_rc.top + 2 * s,
                                                       title_rc.right, title_rc.top + 24 * s);
                painter_.DrawBadge({badge, count, fluent::BadgeKind::Neutral});
            }
            for (size_t r = 0; r < group.rows.size(); ++r) {
                const auto& row_rc = lay.shortcut_rows[first_row + r];
                if (r > 0) divider(lay.shortcut_rows[first_row + r - 1]);
                const auto& row = group.rows[r];
                const bool row_hot = row.command >= 0 &&
                    vm.hover_region == static_cast<int>(H::SettingsShortcutRow) &&
                    vm.hover_control_index == row.command;
                if (row_hot) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), row_rc.left + 6 * scale_, row_rc.top + 3 * scale_,
                        std::max(0.0f, row_rc.right - row_rc.left - 12 * scale_),
                        std::max(0.0f, row_rc.bottom - row_rc.top - 6 * scale_), 5 * scale_);
                }
                if (row.command >= 0 && vm.settings_shortcut_capture == row.command) {
                    text(row.action, D2D1::RectF(row_rc.left + 16 * scale_, row_rc.top,
                                                 row_rc.right - 16 * scale_, row_rc.bottom));
                    painter_.DrawText(l10n::Pick(L"按下新的快捷键…", L"Press the new shortcut…"),
                                      D2D1::RectF(row_rc.left + 16 * scale_, row_rc.top,
                                                  row_rc.right - 16 * scale_, row_rc.bottom),
                                      compositor_->SmallFormat(), theme.accent,
                                      fluent::HorizontalAlignment::Right);
                    continue;
                }
                // Keys as chips, right aligned; " / " separates alternatives.
                std::vector<std::wstring> aliases;
                size_t at = 0;
                while (at <= row.keys.size()) {
                    const size_t sep = row.keys.find(L" / ", at);
                    aliases.push_back(row.keys.substr(at, sep == std::wstring::npos ? std::wstring::npos : sep - at));
                    if (sep == std::wstring::npos) break;
                    at = sep + 3;
                }
                const float chip_h = 22 * scale_;
                const float gap = 6 * scale_;
                float total = 0.0f;
                std::vector<float> widths;
                for (const auto& alias : aliases) {
                    const float w = MeasureTextWidth(compositor_->DwriteFactory(),
                                                     compositor_->SmallFormat(), alias) + 16 * scale_;
                    widths.push_back(w);
                    total += w;
                }
                total += gap * static_cast<float>(aliases.size() > 0 ? aliases.size() - 1 : 0);
                float x = row_rc.right - 16 * scale_ - total;
                const float chip_top = row_rc.top + (row_rc.bottom - row_rc.top - chip_h) * 0.5f;
                text(row.action, D2D1::RectF(row_rc.left + 16 * scale_, row_rc.top,
                                             (std::max)(row_rc.left + 60 * scale_, x - 12 * scale_),
                                             row_rc.bottom));
                for (size_t a = 0; a < aliases.size(); ++a) {
                    const D2D1_RECT_F chip = D2D1::RectF(x, chip_top, x + widths[a], chip_top + chip_h);
                    MakeBrush(dc, theme.fill_input, brFillInput_);
                    FillRoundedRect(dc, brFillInput_.get(), chip.left, chip.top, chip.right - chip.left,
                                    chip.bottom - chip.top, 5 * scale_);
                    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(chip, 5 * scale_, 5 * scale_),
                                             brStrokeCard_.get(), 1);
                    painter_.DrawText(aliases[a], chip, compositor_->SmallFormat(), theme.text,
                                      fluent::HorizontalAlignment::Center);
                    x += widths[a] + gap;
                }
            }
        }
        {
            fluent::ButtonSpec reset;
            reset.bounds = lay.shortcut_reset;
            reset.text = l10n::Pick(L"恢复默认快捷键", L"Restore default shortcuts");
            reset.kind = fluent::ButtonKind::Standard;
            reset.state.hovered = vm.hover_region == static_cast<int>(H::SettingsShortcutReset);
            reset.state.enabled = true;
            painter_.DrawButton(reset);
        }
        if (!vm.settings_shortcut_hint.empty()) {
            painter_.DrawWrappedCaption(vm.settings_shortcut_hint,
                                        D2D1::Point2F(lay.footer.left, lay.footer.top),
                                        lay.footer.right - lay.footer.left, theme.accent);
        } else {
            painter_.DrawWrappedCaption(pages_text::ShortcutNote(),
                                        D2D1::Point2F(lay.footer.left, lay.footer.top),
                                        lay.footer.right - lay.footer.left, theme.text_secondary);
        }
        return;
    }

    if (page == 0) {
        section_title(0, pages_text::AppearanceTheme());
        draw_card(span(lay.theme_row, lay.accent_card));
        {
            const bool small_theme = lay.theme_tile[0].left < lay.theme_row.left + 100 * scale_;
            label(lay.theme_row, l10n::Get(I::SettingsTheme),
                  small_theme ? L"" : l10n::Get(I::SettingsThemeDesc), L"\xE790",
                  small_theme ? lay.theme_row.right - 16 * scale_ : lay.theme_tile[0].left - 12 * scale_);
            const I theme_names[] = {I::SettingsThemeLight, I::SettingsThemeDark, I::SettingsThemeSystem};
            const int theme_values[] = {1, 2, 0};
            for (int i = 0; i < 3; ++i) {
                const auto r = lay.theme_tile[i];
                auto preview = r;
                preview.bottom -= 22 * scale_;
                const bool selected = vm.settings_theme == theme_values[i];
                MakeBrush(dc, i == 0 ? HexColor(0xF1F3EE) : HexColor(0x1A1A1A), brFillHover_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(preview, 5 * scale_, 5 * scale_), brFillHover_.get());
                MakeBrush(dc, i == 0 ? HexColor(0xFFFFFF) : HexColor(0x333333), brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), preview.left + 7 * scale_, preview.top + 8 * scale_,
                                preview.right - preview.left - 14 * scale_, 8 * scale_, 2 * scale_);
                FillRoundedRect(dc, brFillHover_.get(), preview.left + 7 * scale_, preview.top + 21 * scale_,
                                14 * scale_, preview.bottom - preview.top - 28 * scale_, 2 * scale_);
                if (i == 2) {
                    MakeBrush(dc, HexColor(0xF1F3EE), brFillHover_);
                    FillRect(dc, brFillHover_.get(), preview.left + 7 * scale_, preview.top + 8 * scale_,
                             (preview.right - preview.left) / 2 - 7 * scale_,
                             preview.bottom - preview.top - 15 * scale_);
                }
                MakeBrush(dc, selected ? theme.accent
                                       : IsHovered(vm, H::SettingsTheme, theme_values[i]) ? theme.text_secondary
                                                                                          : theme.stroke_card,
                          brStrokeCard_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(preview, 5 * scale_, 5 * scale_),
                                         brStrokeCard_.get(), selected ? 2 * scale_ : 1);
                if (selected)
                    DrawIconText(preview.right - 23 * scale_, preview.top + 3 * scale_, 20 * scale_,
                                 20 * scale_, L"\xE73E", L"", theme.accent, 0.7f);
                painter_.DrawText(l10n::Get(theme_names[i]),
                                  D2D1::RectF(r.left, preview.bottom + 2 * scale_, r.right, r.bottom),
                                  compositor_->SmallFormat(), theme.text,
                                  fluent::HorizontalAlignment::Center);
            }
            divider(lay.theme_row);
            label(lay.accent_card, l10n::Get(I::SettingsThemeColor), l10n::Get(I::SettingsThemeColorDesc),
                  L"\xE790", lay.accent_toggle_button.left - 12 * scale_);
            {
                fluent::ControlState st;
                st.enabled = true;
                st.hovered = IsHovered(vm, H::SettingsAccentToggle, 0);
                painter_.DrawButton({lay.accent_toggle_button,
                                     l10n::Get(vm.settings_accent_editor_open ? I::SettingsAccentCollapse
                                                                              : I::SettingsAccentCustom),
                                     L"", fluent::ButtonKind::Standard, st});
            }
            if (vm.settings_bloom) {
                vm.settings_bloom->SetDisk(lay.accent_picker);
                vm.settings_bloom->Draw(dc, theme);
            }
        }
        if (vm.settings_accent_editor_open) {
            draw_card(lay.accent_custom_card);
            {
                const auto& card = lay.accent_custom_card;
                painter_.DrawText(l10n::Get(I::SettingsAccentCustomTitle),
                                  D2D1::RectF(card.left + 20 * scale_, card.top + 12 * scale_,
                                              card.left + 260 * scale_, card.top + 32 * scale_),
                                  compositor_->TextFormat(), theme.text,
                                  fluent::HorizontalAlignment::Left);
                painter_.DrawWrappedCaption(
                    l10n::Get(I::SettingsAccentCustomDesc),
                    D2D1::Point2F(card.left + 20 * scale_, card.top + 32 * scale_),
                    std::max(120.0f * scale_, lay.accent_hex_rect.left - card.left - 40 * scale_),
                    theme.text_secondary);
                // Swatch and its hex code.
                D2D1_COLOR_F swatch_ink = vm.settings_accent_valid
                    ? HexColor(vm.settings_accent_rgb) : theme.accent;
                MakeBrush(dc, swatch_ink, brAccent_);
                dc->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(lay.accent_swatch.left, lay.accent_swatch.top,
                                                  lay.accent_swatch.right, lay.accent_swatch.bottom),
                                      5 * scale_, 5 * scale_), brAccent_.get());
                painter_.DrawText(vm.settings_accent_hex.empty() ? std::wstring(L"#------")
                                                                 : (L"#" + vm.settings_accent_hex),
                                  lay.accent_hex_rect, compositor_->TextFormat(), theme.text,
                                  fluent::HorizontalAlignment::Right);
            }
            auto accent_slider = [&](const D2D1_RECT_F& row, const D2D1_RECT_F* cells, int value,
                                     int max_value, H::Region region, I title, const wchar_t* unit) {
                label(row, l10n::Get(title), L"", L"\xE8A4", cells[0].left - 12 * scale_);
                const auto g = SettingsSlider(cells, scale_);
                const bool hot = vm.hover_region == static_cast<int>(region);
                const float span = (std::max)(1.0f, g.right - g.left);
                const float x = g.left + span * std::clamp(
                    static_cast<float>(value) / static_cast<float>(max_value), 0.0f, 1.0f);
                const float th = 4 * scale_;
                MakeBrush(dc, WithAlpha(theme.text, 0.16f), brFillHover_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(
                    D2D1::RectF(g.left, g.cy - th / 2, g.right, g.cy + th / 2), th / 2, th / 2),
                    brFillHover_.get());
                MakeBrush(dc, theme.accent, brAccent_);
                if (x > g.left)
                    dc->FillRoundedRectangle(D2D1::RoundedRect(
                        D2D1::RectF(g.left, g.cy - th / 2, x, g.cy + th / 2), th / 2, th / 2),
                        brAccent_.get());
                MakeBrush(dc, WithAlpha(theme.surface_card, 1.0f), brFillInput_);
                MakeBrush(dc, theme.stroke_card, brStrokeCard_);
                const float outer = 9 * scale_;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, g.cy), outer, outer), brFillInput_.get());
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, g.cy), outer, outer), brStrokeCard_.get(), 1);
                const float inner = (hot ? 6.0f : 5.0f) * scale_;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, g.cy), inner, inner), brAccent_.get());
                painter_.DrawText(std::to_wstring(value) + unit,
                                  D2D1::RectF(g.right + 10 * scale_, g.cy - 10 * scale_,
                                              cells[2].right, g.cy + 10 * scale_),
                                  compositor_->TextFormat(), theme.text,
                                  fluent::HorizontalAlignment::Right);
            };
            accent_slider(lay.accent_custom_row[0], lay.accent_custom_cells[0],
                          vm.settings_accent_h, 359, H::SettingsAccentHue, I::SettingsAccentHue, L"\x00B0");
            accent_slider(lay.accent_custom_row[1], lay.accent_custom_cells[1],
                          vm.settings_accent_s, 100, H::SettingsAccentSaturation,
                          I::SettingsAccentSaturation, L"%");
            accent_slider(lay.accent_custom_row[2], lay.accent_custom_cells[2],
                          vm.settings_accent_l, 100, H::SettingsAccentLightness,
                          I::SettingsAccentLightness, L"%");
        }

        section_title(1, pages_text::AppearanceBackdrop());
        draw_card(span(lay.effect_card, lay.wallpaper_blur_card));
        {
            const I effects[] = {I::EffectNone, I::EffectAcrylic, I::EffectMica, I::EffectMicaAlt};
            label(lay.effect_card, l10n::Get(I::SettingsWindowEffect),
                  l10n::Get(compat::ModernWindows() ? I::SettingsWindowEffectDesc : I::EffectUnavailable),
                  L"\xE790", lay.effect_card.right - 16 * scale_);
            for (int i = 0; i < kWindowEffectCount; ++i) {
                const auto r = lay.effect_row[i];
                auto preview = r;
                preview.bottom -= 22 * scale_;
                const bool enabled = i == 0 || compat::ModernWindows();
                const bool selected = i == static_cast<int>(compat::ModernWindows() ? vm.window_effect
                                                                                   : WindowEffect::None);
                const bool hovered = enabled && IsHovered(vm, H::SettingsEffect, i);
                const float tint[] = {0.0f, 0.22f, 0.065f, 0.16f};
                auto background = BlendOver(WithAlpha(theme.accent, tint[i]), theme.surface_title);
                if (!enabled) background = BlendOver(WithAlpha(theme.surface_sheet, 0.6f), background);
                MakeBrush(dc, background, brFillHover_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(preview, 6 * scale_, 6 * scale_), brFillHover_.get());
                dc->PushAxisAlignedClip(preview, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                if (i == 1 || i == 3) {
                    MakeBrush(dc, WithAlpha(theme.accent, vm.dark ? 0.18f : 0.13f), brFillHover_);
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(preview.right - 18 * scale_, preview.top + 11 * scale_),
                                                  36 * scale_, 26 * scale_), brFillHover_.get());
                }
                const float inset = 10 * scale_;
                auto window = D2D1::RectF(preview.left + inset, preview.top + 9 * scale_, preview.right - inset,
                                          preview.bottom - 9 * scale_);
                const float plate_alpha = i == 1 ? 0.48f : i == 3 ? 0.72f : 0.96f;
                MakeBrush(dc, WithAlpha(theme.surface_card, plate_alpha), brFillInput_);
                dc->FillRoundedRectangle(D2D1::RoundedRect(window, 3 * scale_, 3 * scale_), brFillInput_.get());
                MakeBrush(dc, WithAlpha(theme.text_secondary, 0.30f), brTextSecondary_);
                FillRoundedRect(dc, brTextSecondary_.get(), window.left + 6 * scale_, window.top + 6 * scale_,
                                (std::min)(18 * scale_, (window.right - window.left) * 0.18f), 2 * scale_, scale_);
                MakeBrush(dc, WithAlpha(theme.accent, 0.10f), brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), window.right - 22 * scale_, window.top + 5 * scale_,
                                16 * scale_, window.bottom - window.top - 10 * scale_, 2 * scale_);
                dc->PopAxisAlignedClip();
                MakeBrush(dc, selected ? theme.accent : hovered ? theme.text_secondary : theme.stroke_card,
                          brStrokeCard_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(preview, 6 * scale_, 6 * scale_), brStrokeCard_.get(),
                                         selected ? 1.5f * scale_ : scale_);
                if (selected) {
                    const auto center = D2D1::Point2F(preview.right - 9 * scale_, preview.bottom - 9 * scale_);
                    MakeBrush(dc, theme.accent, brAccent_);
                    dc->FillEllipse(D2D1::Ellipse(center, 7 * scale_, 7 * scale_), brAccent_.get());
                    DrawIconText(center.x - 6 * scale_, center.y - 6 * scale_, 12 * scale_, 12 * scale_,
                                 L"\xE73E", L"", theme.accent_text, 0.6f);
                }
                painter_.DrawText(l10n::Get(effects[i]),
                                  D2D1::RectF(r.left, preview.bottom + 3 * scale_, r.right, r.bottom),
                                  compositor_->SmallFormat(),
                                  !enabled ? theme.text_disabled : selected ? theme.accent : theme.text_secondary,
                                  fluent::HorizontalAlignment::Center);
            }
            divider(lay.effect_card);
            // The wallpaper and how strongly the panes let it through.
            const auto& preview = lay.wallpaper_preview;
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), preview.left, preview.top, preview.right - preview.left,
                            preview.bottom - preview.top, 6 * scale_);
            if (!vm.background_image.empty()) material_.DrawSourceCover(dc, preview, vm.background_image);
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(preview, 6 * scale_, 6 * scale_), brStrokeCard_.get(), 1);
            {
                const float text_left = preview.right + 12 * scale_;
                const bool compact_wallpaper = lay.wallpaper_choose.top > lay.wallpaper_card.top + 64 * scale_;
                const float text_right = compact_wallpaper ? lay.wallpaper_card.right - 16 * scale_
                                                           : lay.wallpaper_choose.left - 12 * scale_;
                MakeBrush(dc, theme.text, brText_);
                DrawTextRect(dc, compositor_->TextFormat(), brText_.get(), l10n::Get(I::SettingsWallpaper),
                             text_left, lay.wallpaper_card.top + 18 * scale_,
                             (std::max)(40 * scale_, text_right - text_left), 22 * scale_);
                const std::wstring wallpaper_desc = vm.background_image.empty()
                    ? l10n::Get(I::SettingsWallpaperDesc) : FileNameOf(vm.background_image);
                MakeBrush(dc, theme.text_secondary, brTextSecondary_);
                DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), wallpaper_desc, text_left,
                             lay.wallpaper_card.top + 42 * scale_,
                             (std::max)(40 * scale_, text_right - text_left), 18 * scale_);
            }
            {
                fluent::ControlState choose{};
                choose.hovered = IsHovered(vm, H::SettingsWallpaper, 0);
                painter_.DrawButton({lay.wallpaper_choose, l10n::Get(I::ChooseImage), {},
                                     fluent::ButtonKind::Standard, choose});
                fluent::ControlState clear{};
                clear.enabled = !vm.background_image.empty();
                clear.hovered = clear.enabled && IsHovered(vm, H::SettingsWallpaper, 1);
                painter_.DrawButton({lay.wallpaper_clear, l10n::Get(I::Clear), {},
                                     fluent::ButtonKind::Standard, clear});
            }
            divider(lay.wallpaper_card);
            const I looks[] = {I::WallpaperLookSubtle, I::WallpaperLookBalanced, I::WallpaperLookVivid};
            const int look_ticks[] = {25, 50, 75};
            slider(lay.wallpaper_look_card, lay.wallpaper_look_row, vm.wallpaper_look, kPanelTransparencyMax,
                   look_ticks, looks, H::SettingsWallpaperLook, I::SettingsWallpaperLook,
                   I::SettingsWallpaperLookDesc, L"%");
            divider(lay.wallpaper_look_card);
            const I blurs[] = {I::WallpaperBlurOff, I::WallpaperBlurLight, I::WallpaperBlurStrong};
            const int blur_ticks[] = {0, 14, 28};
            slider(lay.wallpaper_blur_card, lay.wallpaper_blur_row, vm.wallpaper_blur, kWallpaperBlurMax,
                   blur_ticks, blurs, H::SettingsWallpaperBlur, I::SettingsWallpaperBlur,
                   I::SettingsWallpaperBlurDesc, L" px");
        }

        section_title(2, pages_text::AppearanceText());
        draw_card(span(lay.language_card, lay.title_clock_row));
        const I languages[] = {I::LanguageSystem, I::LanguageZhCN, I::LanguageZhTW, I::LanguageEnUS};
        dropdown(lay.language_card, lay.language_choice, I::SettingsLanguage, I::SettingsLanguageDesc,
                 l10n::Get(languages[vm.settings_language]), 1);
        divider(lay.language_card);
        const I text_render[] = {I::TextRenderAuto, I::TextRenderSharp, I::TextRenderSmooth};
        const int text_render_values[] = {0, 1, 2};
        segmented(lay.text_render_card, lay.text_render_row, text_render, text_render_values,
                  vm.settings_text_render, H::SettingsTextRender, I::SettingsTextRender,
                  I::SettingsTextRenderDesc);
        divider(lay.text_render_card);
        const I ui_font_size[] = {I::UiFontSmall, I::UiFontDefault, I::UiFontLarge, I::UiFontLarger};
        const int ui_font_size_values[] = {90, 100, 112, 125};
        segmented(lay.ui_font_size_card, lay.ui_font_size_row, ui_font_size, ui_font_size_values,
                  vm.settings_ui_font_scale, H::SettingsUiFontSize, I::SettingsUiFontSize,
                  I::SettingsUiFontSizeDesc, 4, L"\xE8D2");
        divider(lay.ui_font_size_card);
        toggle(lay.title_clock_row, I::SettingsTitleClock, I::SettingsTitleClockDesc,
               L"\xE823", vm.settings_show_title_clock, 60);
        text(l10n::Get(I::SettingsImmediate), lay.footer, true);
        return;
    }

    if (page == 7) {
        section_title(0, pages_text::FileListOpen());
        draw_card(lay.blank_click_row);
        const I blank_click[] = {I::SettingsBlankClickOff, I::Back, I::Up};
        const int blank_click_values[] = {0, 1, 2};
        segmented(lay.blank_click_row, lay.blank_click_choice, blank_click, blank_click_values,
                  vm.settings_blank_click_action, H::SettingsBlankClick, I::SettingsBlankClickBack,
                  I::SettingsBlankClickBackDesc, 3, L"\xE72B");
        draw_card(lay.single_click_row);
        toggle(lay.single_click_row, I::SettingsSingleClick, I::SettingsSingleClickDesc, L"\xE8A7",
               vm.settings_single_click_open, 37);
        draw_card(lay.checkbox_row);
        toggle(lay.checkbox_row, I::SettingsCheckboxes, I::SettingsCheckboxesDesc, L"\xE739",
               vm.settings_list_checkboxes, 39);

        section_title(1, pages_text::FileListShow());
        draw_card(span(lay.hidden_files_row, lay.hidden_shares_row));
        toggle(lay.hidden_files_row, I::SettingsShowHidden, I::SettingsShowHiddenDesc, L"\xE890",
               vm.settings_show_hidden_files, 5);
        divider(lay.hidden_files_row);
        toggle(lay.protected_files_row, I::SettingsShowProtected, I::SettingsShowProtectedDesc, L"\xE72E",
               vm.settings_show_protected_os_files, 16);
        divider(lay.protected_files_row);
        toggle(lay.hidden_shares_row, I::SettingsHiddenShares, I::SettingsHiddenSharesDesc, L"\xE968",
               vm.settings_show_hidden_shares, 38);

        section_title(2, pages_text::FileListLook());
        draw_card(span(lay.density_card, lay.folder_sort_card));
        const I density[] = {I::SettingsDensityCompact, I::SettingsDensityStandard, I::SettingsDensityRoomy};
        const int heights[] = {28, 34, 40};
        segmented(lay.density_card, lay.density_row, density, heights, vm.settings_row_height,
                  H::SettingsDensity, I::SettingsRowHeight, I::SettingsRowHeightDesc);
        divider(lay.density_card);
        toggle(lay.performance_row, I::SettingsShowPerformance, I::SettingsShowPerformanceDesc, L"\xE946",
               vm.settings_show_performance, 4);
        divider(lay.performance_row);
        toggle(lay.list_style_row[0], I::ListSmartDate, I::ListSmartDateDesc, L"\xE787",
               vm.settings_list_smart_date, 17);
        divider(lay.list_style_row[0]);
        toggle(lay.list_style_row[1], I::ListZebraRows, I::ListZebraRowsDesc, L"\xE8FD",
               vm.settings_list_zebra_rows, 18);
        divider(lay.list_style_row[1]);
        toggle(lay.list_style_row[2], I::ListSizeBar, I::ListSizeBarDesc, L"\xE9D2",
               vm.settings_list_size_bar, 19);
        divider(lay.list_style_row[2]);
        toggle(lay.list_style_row[3], I::ListTagNameColor, I::ListTagNameColorDesc, L"\xE8EC",
               vm.settings_list_tag_names, 22);
        divider(lay.list_style_row[3]);
        toggle(lay.list_style_row[4], I::ListSelectionOutline, I::ListSelectionOutlineDesc, L"\xE73E",
               vm.settings_list_selection_outline, 33);
        divider(lay.list_style_row[4]);
        toggle(lay.list_style_row[5], I::ListThumbnailBadges, I::ListThumbnailBadgesDesc, L"\xE8B9",
               vm.settings_list_thumbnail_badges, 34);
        divider(lay.list_style_row[5]);
        {
            const auto& r = lay.thumb_cache_row;
            DrawIconText(r.left + 16 * scale_, r.top + 22 * scale_, 22 * scale_, 22 * scale_,
                         L"\xE8B9", L"", theme.text_secondary, 0.85f);
            text(pages_text::ThumbCacheTitle(),
                 D2D1::RectF(r.left + 54 * scale_, r.top + 12 * scale_,
                             lay.thumb_cache_button.left - 12 * scale_, r.top + 34 * scale_));
            painter_.DrawWrappedCaption(
                vm.settings_thumb_cache_text.empty() ? pages_text::ThumbCacheDesc()
                                                     : vm.settings_thumb_cache_text,
                D2D1::Point2F(r.left + 54 * scale_, r.top + 34 * scale_),
                std::max(40.0f * scale_, lay.thumb_cache_button.left - r.left - 70 * scale_),
                theme.text_secondary);
            fluent::ControlState st;
            st.enabled = true;
            st.hovered = vm.hover_region == static_cast<int>(H::SettingsThumbCacheClear);
            painter_.DrawButton({lay.thumb_cache_button, pages_text::ThumbCacheClear(), L"",
                                 fluent::ButtonKind::Standard, st});
        }
        const I folder_sort[] = {I::FolderSortTop, I::FolderSortFollow, I::FolderSortMixed};
        const int folder_sort_values[] = {0, 1, 2};
        segmented(lay.folder_sort_card, lay.folder_sort_row, folder_sort, folder_sort_values,
                  vm.settings_folder_sort, H::SettingsFolderSort, I::SettingsFolderSort,
                  I::SettingsFolderSortDesc);
        draw_card(span(lay.sticky_parent_row, lay.svn_status_row));
        toggle(lay.sticky_parent_row, I::SettingsStickyParent, I::SettingsStickyParentDesc,
               L"\xE74A", vm.settings_sticky_parent, 47);
        divider(lay.sticky_parent_row);
        toggle(lay.tree_view_row, I::SettingsTreeView, I::SettingsTreeViewDesc,
               L"\xE8FD", vm.settings_tree_view, 49);
        divider(lay.tree_view_row);
        toggle(lay.svn_status_row, I::SettingsSvnStatus,
               vm.settings_svn_available ? I::SettingsSvnStatusDesc : I::SettingsSvnUnavailable,
               L"\xE896", vm.settings_svn_status, 48);

        section_title(3, l10n::Get(I::SettingsMtimeSection).c_str());
        draw_card(lay.mtime_style_card);
        const I mtime_style[] = {I::SettingsMtimeOff, I::SettingsMtimeDot, I::SettingsMtimeBar,
                                 I::SettingsMtimeText};
        const int mtime_style_values[] = {0, 1, 2, 3};
        segmented(lay.mtime_style_card, lay.mtime_style_row, mtime_style, mtime_style_values,
                  vm.settings_mtime_style, H::SettingsMtimeStyle, I::SettingsMtimeStyle,
                  I::SettingsMtimeStyleDesc, 4, L"\xE823");
        draw_card(lay.mtime_days_card);
        const I mtime_days[] = {I::SettingsMtimeToday, I::SettingsMtimeD3, I::SettingsMtimeD7,
                                I::SettingsMtimeD30};
        const int mtime_days_values[] = {1, 3, 7, 30};
        segmented(lay.mtime_days_card, lay.mtime_days_row, mtime_days, mtime_days_values,
                  vm.settings_mtime_days, H::SettingsMtimeDays, I::SettingsMtimeRange,
                  I::SettingsMtimeStyleDesc, 4, L"\xE787");
        draw_card(lay.mtime_palette_card);
        const I mtime_palette[] = {I::SettingsMtimeAccent, I::SettingsMtimeCoolwarm,
                                   I::SettingsMtimeTeal, I::SettingsMtimeAmber,
                                   I::SettingsMtimeAurora, I::SettingsMtimeGraphite};
        const int mtime_palette_values[] = {0, 1, 2, 3, 4, 5};
        segmented(lay.mtime_palette_card, lay.mtime_palette_row, mtime_palette, mtime_palette_values,
                  vm.settings_mtime_palette, H::SettingsMtimePalette, I::SettingsMtimePalette,
                  I::SettingsMtimePaletteDesc, 6, L"\xE790");
        draw_card(span(lay.mtime_opt_row[0], lay.mtime_opt_row[4]));
        toggle(lay.mtime_opt_row[0], I::SettingsMtimeDimOld, I::SettingsMtimeDimOldDesc,
               L"\xE70F", vm.settings_mtime_dim_old, 41);
        divider(lay.mtime_opt_row[0]);
        toggle(lay.mtime_opt_row[1], I::SettingsMtimeBoldToday, I::SettingsMtimeBoldTodayDesc,
               L"\xE8D3", vm.settings_mtime_bold_today, 42);
        divider(lay.mtime_opt_row[1]);
        toggle(lay.mtime_opt_row[2], I::SettingsMtimeIcons, I::SettingsMtimeIconsDesc,
               L"\xE71D", vm.settings_mtime_icons, 43);
        divider(lay.mtime_opt_row[2]);
        toggle(lay.mtime_opt_row[3], I::SettingsMtimeFolders, I::SettingsMtimeFoldersDesc,
               L"\xE8B7", vm.settings_mtime_folders, 44);
        divider(lay.mtime_opt_row[3]);
        toggle(lay.mtime_opt_row[4], I::SettingsMtimeFlash, I::SettingsMtimeFlashDesc,
               L"\xE945", vm.settings_mtime_flash, 45);

        section_title(4, l10n::Get(I::SettingsSyncSection).c_str());
        draw_card(lay.cloud_status_row);
        toggle(lay.cloud_status_row, I::SettingsCloudStatus, I::SettingsCloudStatusDesc,
               L"\xE753", vm.settings_cloud_status, 46);
        text(l10n::Get(I::SettingsImmediate), lay.footer, true);
        return;
    }

    if (page == 8) {
        section_title(0, pages_text::TabsOpen());
        draw_card(span(lay.home_folder_card, lay.new_tab_open_card));
        {
            const bool below = lay.home_folder_choose.top > lay.home_folder_card.top + 40 * scale_;
            label(lay.home_folder_card, l10n::Get(I::SettingsHomeFolder),
                  vm.settings_home_folder.empty() ? l10n::Get(I::ThisPc) : vm.settings_home_folder, L"\xE80F",
                  below ? lay.home_folder_card.right - 16 * scale_ : lay.home_folder_choose.left - 12 * scale_);
            button(lay.home_folder_choose, l10n::Get(I::SettingsHomeFolderPick), H::SettingsHomeFolder, 0);
            button(lay.home_folder_reset, l10n::Get(I::ThisPc), H::SettingsHomeFolder, 1, false,
                   !vm.settings_home_folder.empty());
            divider(lay.home_folder_card);
        }
        const I startup_open[] = {I::StartupOpenLastTabs, I::OpenDefaultLocation};
        const int two_values[] = {0, 1};
        segmented(lay.startup_open_card, lay.startup_open_row, startup_open, two_values,
                  vm.settings_startup_open, H::SettingsStartupOpen, I::SettingsStartupOpen,
                  I::SettingsStartupOpenDesc, 2, L"\xE81C");
        divider(lay.startup_open_card);
        const I new_tab_open[] = {I::NewTabOpenCurrent, I::OpenDefaultLocation};
        segmented(lay.new_tab_open_card, lay.new_tab_open_row, new_tab_open, two_values,
                  vm.settings_new_tab_open, H::SettingsNewTabOpen, I::SettingsNewTabOpen,
                  I::SettingsNewTabOpenDesc, 2, L"\xE710");

        section_title(1, pages_text::TabsTabs());
        draw_card(span(lay.close_last_tab_row, lay.pinned_names_row));
        toggle(lay.close_last_tab_row, I::SettingsCloseLastTab, I::SettingsCloseLastTabDesc, L"\xE711",
               vm.settings_close_last_tab, 26);
        divider(lay.close_last_tab_row);
        toggle(lay.single_window_row, I::SettingsSingleWindow, I::SettingsSingleWindowDesc, L"\xE737",
               vm.settings_single_window, 36);
        divider(lay.single_window_row);
        toggle(lay.vertical_tabs_row, I::SettingsVerticalTabs, I::SettingsVerticalTabsDesc, L"\xE7C4",
               vm.settings_vertical_tabs, 23);
        divider(lay.vertical_tabs_row);
        toggle(lay.pinned_names_row, I::PinnedNames, I::PinnedNamesDesc, L"\xE718",
               vm.show_pinned_tab_names, 6);

        section_title(2, pages_text::TabsManage());
        draw_card(lay.tab_manage_card);
        if (lay.tab_manage_rows.empty()) {
            painter_.DrawWrappedCaption(pages_text::TabsManageDesc(),
                D2D1::Point2F(lay.tab_manage_card.left + 16 * scale_,
                              lay.tab_manage_card.top + 14 * scale_),
                lay.tab_manage_card.right - lay.tab_manage_card.left - 32 * scale_,
                theme.text_secondary);
        } else {
            for (size_t i = 0; i < lay.tab_manage_rows.size(); ++i) {
                if (i >= vm.settings_tab_manage.size()) break;
                const auto& row = lay.tab_manage_rows[i];
                const auto& item = vm.settings_tab_manage[i];
                if (i > 0) divider(lay.tab_manage_rows[i - 1]);
                const bool hot = vm.hover_region == static_cast<int>(H::SettingsTabRow) &&
                    vm.hover_control_index == static_cast<int>(i);
                if (hot) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), row.left + 2 * scale_, row.top + 2 * scale_,
                        std::max(0.0f, row.right - row.left - 4 * scale_),
                        std::max(0.0f, row.bottom - row.top - 4 * scale_), 6 * scale_);
                }
                const auto dot = TabManageColorRect(row, scale_);
                const D2D1_COLOR_F dot_color = item.color
                    ? ui::HexColor(item.color)
                    : WithAlpha(theme.text_secondary, 0.45f);
                MakeBrush(dc, dot_color, brFillInput_);
                dc->FillEllipse(D2D1::Ellipse(
                    D2D1::Point2F((dot.left + dot.right) * 0.5f, (dot.top + dot.bottom) * 0.5f),
                    8 * scale_, 8 * scale_), brFillInput_.get());
                const auto group_rc = TabManageGroupRect(row, scale_);
                const auto rename_rc = TabManageRenameRect(row, scale_);
                const auto close_rc = TabManageCloseRect(row, scale_);
                painter_.DrawText(item.title,
                    D2D1::RectF(row.left + 44 * scale_, row.top + 8 * scale_,
                                group_rc.left - 12 * scale_, row.top + 30 * scale_),
                    compositor_->TextFormat(),
                    item.active ? theme.accent : theme.text);
                painter_.DrawText(item.path,
                    D2D1::RectF(row.left + 44 * scale_, row.top + 30 * scale_,
                                group_rc.left - 12 * scale_, row.top + 50 * scale_),
                    compositor_->SmallFormat(), theme.text_secondary);
                const bool group_hot =
                    vm.hover_region == static_cast<int>(H::SettingsTabGroup) &&
                    vm.hover_control_index == static_cast<int>(i);
                painter_.DrawText(item.group_name.empty() ? pages_text::TabGroupHint()
                                                         : item.group_name,
                                  group_rc, compositor_->SmallFormat(),
                                  item.group_name.empty() ? WithAlpha(theme.text_secondary, 0.6f)
                                  : group_hot ? theme.accent
                                              : theme.text_secondary,
                                  fluent::HorizontalAlignment::Right);
                {
                    fluent::ControlState rename_state;
                    rename_state.hovered = vm.hover_region == static_cast<int>(H::SettingsTabRename) &&
                        vm.hover_control_index == static_cast<int>(i);
                    painter_.DrawButton({rename_rc, pages_text::TabRenameButton(), L"",
                                         fluent::ButtonKind::Transparent, rename_state});
                    const bool close_hot = vm.hover_region == static_cast<int>(H::SettingsTabClose) &&
                        vm.hover_control_index == static_cast<int>(i);
                    if (close_hot) {
                        MakeBrush(dc, theme.fill_pressed, brFillHover_);
                        FillRoundedRect(dc, brFillHover_.get(), close_rc.left, close_rc.top,
                                        close_rc.right - close_rc.left, close_rc.bottom - close_rc.top,
                                        6 * scale_);
                    }
                    DrawIconText(close_rc.left, close_rc.top, close_rc.right - close_rc.left,
                                 close_rc.bottom - close_rc.top, L"\xE711", L"x",
                                 close_hot ? theme.text : theme.text_secondary, 0.68f);
                }
            }
        }
        text(l10n::Get(I::SettingsImmediate), lay.footer, true);
        return;
    }

    if (page == 9) {
        section_title(0, pages_text::StartupTray());
        draw_card(span(lay.startup_row[0], lay.tray_icon_card));
        toggle(lay.startup_row[0], I::SettingsLaunch, I::SettingsLaunchDesc, L"\xE7E8",
               vm.settings_launch_on_startup, 1);
        divider(lay.startup_row[0]);
        toggle(lay.start_in_tray_row, I::SettingsStartInTray, I::SettingsStartInTrayDesc, L"\xE921",
               vm.settings_start_in_tray, 27);
        divider(lay.start_in_tray_row);
        toggle(lay.startup_row[1], I::SettingsKeepRunning, I::SettingsKeepRunningDesc, L"\xE737",
               vm.settings_keep_running, 2);
        divider(lay.startup_row[1]);
        toggle(lay.explorer_fallback_row, I::SettingsExplorerFallback,
               I::SettingsExplorerFallbackDesc, L"\xE8B7", vm.settings_explorer_fallback, 40);
        divider(lay.explorer_fallback_row);
        const I notify_icon[] = {I::NotifyIconAlways, I::NotifyIconBackground, I::NotifyIconNever};
        const int notify_values[] = {0, 1, 2};
        segmented(lay.notify_icon_card, lay.notify_icon_row, notify_icon, notify_values,
                  vm.settings_notify_icon, H::SettingsNotifyIcon, I::SettingsNotifyIcon,
                  I::SettingsNotifyIconDesc, 3, L"\xE8A1");
        divider(lay.notify_icon_card);
        const I tray_sizes[] = {I::SettingsTraySmall, I::SettingsTrayStandard, I::SettingsTrayLarge};
        const int tray_values[] = {40, 48, 56};
        segmented(lay.tray_icon_card, lay.tray_icon_row, tray_sizes, tray_values, vm.settings_tray_icon,
                  H::SettingsTrayIcon, I::SettingsTrayIcon, I::SettingsTrayIconDesc);

        section_title(1, pages_text::StartupShell());
        draw_card(lay.integration_card);
        const bool master_on = vm.settings_integration_enabled;
        auto hover_fill = [&](D2D1_RECT_F r) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), r.left + 2 * scale_, r.top + 2 * scale_,
                            r.right - r.left - 4 * scale_, r.bottom - r.top - 4 * scale_, 6 * scale_);
        };
        {
            const auto r = lay.default_manager_row;
            fluent::ControlState state{};
            state.checked = master_on;
            state.hovered = IsHovered(vm, H::SettingsIntegration, 0);
            if (state.hovered) hover_fill(r);
            label(r, l10n::Get(I::IntegrationMaster), L"", L"\xEC50", lay.integration_text_right);
            painter_.DrawWrappedCaption(l10n::Get(I::IntegrationMasterDesc),
                                        D2D1::Point2F(r.left + 54 * scale_, r.top + 35 * scale_),
                                        (std::max)(40 * scale_, lay.integration_text_right - r.left - 54 * scale_),
                                        theme.text_secondary);
            const auto badge = MakeIntegrationBadge(vm);
            fluent::BadgeSpec pill{};
            pill.bounds = lay.integration_badge;
            pill.text = badge.text;
            pill.kind = badge.kind;
            painter_.DrawBadge(pill);
            painter_.DrawSwitch(D2D1::RectF(r.right - 60 * scale_, r.top + 16 * scale_,
                                            r.right - 16 * scale_, r.top + 48 * scale_), L"", state);
            if (lay.integration_bar.bottom <= lay.integration_bar.top) divider(r);
        }
        if (lay.integration_bar.bottom > lay.integration_bar.top) {
            const bool failed = vm.settings_integration_state == 3;
            fluent::InfoBarSpec bar{};
            bar.bounds = lay.integration_bar;
            bar.title = l10n::Get(failed ? I::IntegrationFailTitle : I::IntegrationDriftTitle);
            bar.message = IntegrationProblemMessage(vm);
            bar.kind = failed ? fluent::InfoBarKind::Error : fluent::InfoBarKind::Warning;
            bar.show_close = false;
            painter_.DrawInfoBar(bar);
            if (lay.integration_retry.bottom > lay.integration_retry.top)
                button(lay.integration_retry,
                       l10n::Get(failed ? I::IntegrationRetry : I::IntegrationReapply), H::SettingsIntegration, 5,
                       true);
            if (lay.integration_restore.bottom > lay.integration_restore.top)
                button(lay.integration_restore, l10n::Get(I::IntegrationRestore), H::SettingsIntegration, 6);
        }
        {
            const auto r = lay.integration_list_head;
            painter_.DrawText(l10n::Get(I::IntegrationScope),
                              D2D1::RectF(r.left + 54 * scale_, r.top + 8 * scale_, r.right - 16 * scale_, r.bottom),
                              compositor_->SmallFormat(), theme.text_secondary);
        }
        auto item = [&](D2D1_RECT_F r, I title, I desc, const wchar_t* icon, bool on, int action) {
            fluent::ControlState state{};
            state.checked = on;
            state.hovered = IsHovered(vm, H::SettingsIntegration, action);
            if (state.hovered) hover_fill(D2D1::RectF(r.left + 40 * scale_, r.top, r.right - 6 * scale_, r.bottom));
            const float x = r.left + 54 * scale_, text_left = r.left + 112 * scale_;
            painter_.DrawCheckBox(D2D1::RectF(x, r.top + 11 * scale_, x + 20 * scale_, r.top + 31 * scale_),
                                  L"", state);
            DrawIconText(x + 30 * scale_, r.top + 11 * scale_, 20 * scale_, 20 * scale_, icon, L"",
                         theme.text_secondary, master_on ? 0.85f : 0.6f);
            painter_.DrawText(l10n::Get(title), D2D1::RectF(text_left, r.top + 9 * scale_,
                                                            r.right - 16 * scale_, r.top + 31 * scale_),
                              compositor_->TextFormat(), master_on ? theme.text : theme.text_secondary);
            painter_.DrawWrappedCaption(l10n::Get(desc), D2D1::Point2F(text_left, r.top + 31 * scale_),
                                        (std::max)(40 * scale_, r.right - 16 * scale_ - text_left),
                                        theme.text_secondary);
        };
        item(lay.startup_row[2], I::IntegrationFolders, I::IntegrationFoldersDesc, L"\xE8B7",
             vm.settings_integration_folders, 1);
        item(lay.this_pc_row, I::SettingsThisPc, I::SettingsThisPcDesc, L"\xE7F4",
             vm.settings_integration_this_pc, 3);
        item(lay.win_e_row, I::SettingsWinE, I::SettingsWinEDesc, L"\xE765", vm.settings_integration_win_e, 2);
        item(lay.explorer_windows_row, I::IntegrationExperimental, I::IntegrationExperimentalDesc, L"\xE8A7",
             vm.settings_integration_experimental, 4);
        for (int i = 0; i < 4; ++i) {
            fluent::BadgeSpec chip{};
            chip.bounds = lay.integration_chip[i];
            chip.text = l10n::Get(kIntegrationChips[i]);
            chip.kind = i == 0 ? fluent::BadgeKind::Warning : fluent::BadgeKind::Neutral;
            painter_.DrawBadge(chip);
        }
        if (lay.integration_hint.bottom > lay.integration_hint.top) {
            const auto r = lay.integration_hint;
            divider(D2D1::RectF(r.left, r.top - 1, r.right, r.top - 1));
            DrawIconText(r.left + 54 * scale_, r.top + 5 * scale_, 16 * scale_, 16 * scale_, L"\xE946", L"",
                         theme.text_secondary, 0.8f);
            painter_.DrawWrappedCaption(IntegrationHint(vm), D2D1::Point2F(r.left + 78 * scale_, r.top + 4 * scale_),
                                        (std::max)(40 * scale_, r.right - 16 * scale_ - r.left - 78 * scale_),
                                        theme.text_secondary);
        }
        divider(lay.shell_tags_row);
        toggle(lay.shell_tags_row, I::SettingsShellTags, I::SettingsShellTagsDesc, L"\xE8EC",
               vm.settings_shell_tags, 21);
        text(l10n::Get(I::SettingsImmediate), lay.footer, true);
        return;
    }

    if (page == 10) {
        section_title(0, pages_text::OpsDelete());
        draw_card(lay.confirm_delete_row);
        toggle(lay.confirm_delete_row, I::SettingsConfirmDelete, I::SettingsConfirmDeleteDesc, L"\xE74D",
               vm.settings_confirm_delete, 32);

        section_title(1, pages_text::OpsStaging());
        draw_card(lay.staging_tray_row);
        toggle(lay.staging_tray_row, I::SettingsStagingTray, I::SettingsStagingTrayDesc, L"\xE8EC",
               vm.settings_staging_tray, 35);

        section_title(2, pages_text::OpsChanges());
        draw_card(span(lay.change_tracking_row, lay.change_days_row));
        toggle(lay.change_tracking_row, I::SettingsChangeTracking, I::SettingsChangeTrackingDesc, L"\xE823",
               vm.settings_change_tracking, 8);
        divider(lay.change_tracking_row);
        const I days[] = {I::ChangeToday, I::ChangeLast3Days, I::ChangeLast7Days};
        const int day_values[] = {1, 3, 7};
        {
            const bool stacked = lay.change_days[0].left < lay.change_days_row.left + 100 * s;
            label(lay.change_days_row, l10n::Get(I::SettingsChangeDays),
                  pages_text::OpsChangeDaysDesc(), L"\xE8A4",
                  stacked ? lay.change_days_row.right - 16 * s : lay.change_days[0].left - 12 * s);
            painter_.DrawSegmentedTrack(D2D1::RectF(lay.change_days[0].left, lay.change_days[0].top,
                                                    lay.change_days[2].right, lay.change_days[2].bottom));
            for (int i = 0; i < 3; ++i) {
                fluent::SegmentedItemSpec item{};
                item.bounds = lay.change_days[i];
                item.text = l10n::Get(days[i]);
                item.state.checked = vm.settings_change_days == day_values[i];
                item.state.hovered = IsHovered(vm, H::SettingsChangeDays, i);
                item.shared_track = true;
                item.position = i == 0 ? fluent::SegmentPosition::First
                               : i == 2 ? fluent::SegmentPosition::Last
                                        : fluent::SegmentPosition::Middle;
                painter_.DrawSegmentedItem(item);
            }
        }

        section_title(3, pages_text::OpsHints());
        draw_card(span(lay.hints_row, lay.hints_reset_row));
        toggle(lay.hints_row, I::SettingsHints, I::SettingsHintsDesc, L"\xE82F", vm.settings_show_hints, 24);
        divider(lay.hints_row);
        label(lay.hints_reset_row, l10n::Get(I::SettingsHintsReset),
              l10n::Get(vm.settings_tips_seen ? I::SettingsHintsResetDesc : I::HintsResetDone), L"\xE72C",
              lay.hints_reset_button.left - 8 * scale_);
        button(lay.hints_reset_button, l10n::Get(I::HintsResetButton), H::SettingsToggle, 25, false,
               vm.settings_tips_seen);

        const std::wstring collision_title = l10n::Get(I::SettingsCollision);
        section_title(4, collision_title.c_str());
        draw_card(lay.collision_row);
        const I collision[] = {I::SettingsCollisionAsk, I::OpKeepBoth, I::Replace};
        const int collision_values[] = {0, 1, 2};
        segmented(lay.collision_row, lay.collision_choice, collision, collision_values,
                  vm.settings_collision_policy, H::SettingsCollision, I::SettingsCollisionRow,
                  I::SettingsCollisionDesc, 3, L"\xE8C8");
        text(l10n::Get(I::SettingsImmediate), lay.footer, true);
        return;
    }
}

} // namespace luling::ui
