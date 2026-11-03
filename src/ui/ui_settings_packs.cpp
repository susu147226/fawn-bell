#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace luling::ui {

// Settings > 快速预览 (page 5). Geometry: LayoutSettingsPacks; texts:
// pack_text (settings_layout_sections.h); clicks: SettingsPackAction.
void MainRenderer::DrawSettingsPacks(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using H = HitTestResult;
    auto* dc = compositor_->Dc();
    const auto lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const float s = scale_;
    auto text = [&](std::wstring_view value, D2D1_RECT_F r, bool is_small = false) {
        painter_.DrawText(value, r, is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),
                          is_small ? theme.text_secondary : theme.text);
    };
    auto card = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, WithAlpha(theme.surface_card, card_alpha_), brFillInput_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8*s, 8*s), brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 8*s, 8*s), brStrokeCard_.get(), 1);
    };
    auto divider_below = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), r.left + 16*s, r.bottom, r.right - r.left - 32*s, 1);
    };
    auto hovered = [&](PackAction action) {
        return IsHovered(vm, H::SettingsPackAction, static_cast<int>(action));
    };

    // Format availability and pack controls share the same current settings model.
    const auto source_color = [&](PreviewFormatSource source) {
        if (source == PreviewFormatSource::Pack) {
            auto color = theme.accent;
            if (vm.dark) {
                color.r += (1.0f - color.r) * 0.5f;
                color.g += (1.0f - color.g) * 0.5f;
                color.b += (1.0f - color.b) * 0.5f;
            }
            return color;
        }
        if (source == PreviewFormatSource::PackOrSystem)
            return vm.dark ? HexColor(0xB5C6E5) : HexColor(0x4F5D77);
        return theme.text;
    };
    const auto chip = [&](D2D1_RECT_F bounds, std::wstring_view label, PreviewFormatState support) {
        const auto color = source_color(support.source);
        const auto rounded = D2D1::RoundedRect(bounds, (bounds.bottom - bounds.top)/2, (bounds.bottom - bounds.top)/2);
        MakeBrush(dc, WithAlpha(color, vm.dark ? 0.13f : 0.08f), brFillHover_);
        if (support.available) dc->FillRoundedRectangle(rounded, brFillHover_.get());
        else {
            if (!dashStroke_.get()) {
                ComPtr<ID2D1Factory> factory;
                dc->GetFactory(&factory);
                auto properties = D2D1::StrokeStyleProperties();
                properties.dashStyle = D2D1_DASH_STYLE_DASH;
                factory->CreateStrokeStyle(properties, nullptr, 0, &dashStroke_);
            }
            MakeBrush(dc, WithAlpha(color, 0.65f), brStrokeCard_);
            dc->DrawRoundedRectangle(rounded, brStrokeCard_.get(), s, dashStroke_.get());
        }
        painter_.DrawText(label, bounds, compositor_->SmallFormat(), color, fluent::HorizontalAlignment::Center);
    };
    // 预览方式: hover card or Space only, then the hover delay and card size.
    auto choice_row = [&](D2D1_RECT_F r, const D2D1_RECT_F* choices, int count, int checked,
                          PackAction first, const wchar_t* icon, std::wstring_view title,
                          std::wstring_view desc, const wchar_t* (*label)(int)) {
        if (r.bottom <= r.top) return;
        card(r);
        const bool stacked = choices[0].left < r.left + 100*s;
        const float right = stacked ? r.right - 16*s : choices[0].left - 12*s;
        DrawIconText(r.left + 16*s, r.top + 20*s, 24*s, 24*s, icon, L"", theme.text_secondary, 0.85f);
        text(title, D2D1::RectF(r.left + 54*s, r.top + 10*s, right, r.top + 34*s));
        painter_.DrawWrappedCaption(desc, D2D1::Point2F(r.left + 54*s, r.top + 35*s),
                                    right - r.left - 54*s, theme.text_secondary);
        painter_.DrawSegmentedTrack(D2D1::RectF(choices[0].left, choices[0].top,
                                                choices[count - 1].right, choices[count - 1].bottom));
        for (int i = 0; i < count; ++i) {
            fluent::SegmentedItemSpec item{};
            item.bounds = choices[i];
            item.text = label(i);
            item.state.checked = i == checked;
            item.state.hovered = hovered(static_cast<PackAction>(static_cast<int>(first) + i));
            item.shared_track = true;
            item.position = i == 0 ? fluent::SegmentPosition::First
                : i == count - 1 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            painter_.DrawSegmentedItem(item);
        }
    };
    text(pack_text::TriggerSection(), lay.trigger_section);
    {
        const int trigger = vm.settings_preview_trigger == 1 ? 1 : 0;
        choice_row(lay.trigger_row, lay.trigger_choice, 2, trigger, PackAction::TriggerHover, L"è90",
                   pack_text::TriggerTitle(), pack_text::TriggerDesc(trigger), &pack_text::TriggerChoice);
        const int delay = vm.settings_hover_delay_ms == 300 ? 0 : vm.settings_hover_delay_ms == 800 ? 2 : 1;
        choice_row(lay.hover_delay_row, lay.hover_delay_choice, 3, delay, PackAction::HoverDelayShort, L"é16",
                   pack_text::DelayTitle(), pack_text::DelayDesc(), &pack_text::DelayChoice);
        const int card_size = vm.settings_hover_card_size == 0 ? 0 : vm.settings_hover_card_size == 2 ? 2 : 1;
        choice_row(lay.hover_size_row, lay.hover_size_choice, 3, card_size, PackAction::HoverSizeSmall, L"ç40",
                   pack_text::SizeTitle(), pack_text::SizeDesc(), &pack_text::SizeChoice);
    }
    text(pack_text::FormatsSection(), lay.preview_section);
    card(lay.preview_group);
    const auto header = lay.disclosure[2];
    if (IsHovered(vm, H::SettingsDisclosure, 2)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), header.left + 2*s, header.top + 2*s,
                        header.right - header.left - 4*s, header.bottom - header.top - 4*s, 6*s);
    }
    DrawIconText(header.left + 16*s, header.top + 20*s, 24*s, 24*s, L"\xE8A5", L"", theme.text_secondary, 0.85f);
    text(pack_text::FormatsTitle(), lay.preview_header_title);
    painter_.DrawWrappedCaption(pack_text::FormatsSummary(vm),
        D2D1::Point2F(lay.preview_header_summary.left, lay.preview_header_summary.top),
        lay.preview_header_summary.right - lay.preview_header_summary.left, theme.text_secondary);
    const bool formats_open = (vm.settings_expanded & 4u) != 0;
    DrawIconText(header.right - 38*s, (header.top + header.bottom)/2 - 9*s, 18*s, 18*s,
                 formats_open ? L"\xE70D" : L"\xE76C", L"", theme.text_secondary, 0.75f);
    if (formats_open) {
        for (int i = 0; i < 3; ++i)
            chip(lay.preview_legend_chip[i], pack_text::Legend(i), {static_cast<PreviewFormatSource>(i), i == 0});
        painter_.DrawWrappedCaption(pack_text::LegendNote(),
            D2D1::Point2F(lay.preview_legend_note.left, lay.preview_legend_note.top),
            lay.preview_legend_note.right - lay.preview_legend_note.left, theme.text_secondary);
        const auto format_divider = [&](float y) {
            MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
            FillRect(dc, brStrokeDivider_.get(), lay.preview_group.left, y,
                     lay.preview_group.right - lay.preview_group.left, 1);
        };
        format_divider(lay.preview_legend.bottom);
        std::vector<PreviewFormatChip> chips;
        std::vector<PreviewFormatRow> rows;
        LayoutPreviewFormats(lay.preview_formats, s, &chips, &rows,
            [&](std::wstring_view value, float width) { return painter_.MeasureWrappedCaptionHeight(value, width); });
        const auto& groups = PreviewFormatGroups();
        for (size_t g = 0; g < rows.size() && g < groups.size(); ++g) {
            text(groups[g].name, rows[g].name);
            const auto& note = rows[g].note;
            if (note.bottom > note.top && !groups[g].note.empty()) {
                painter_.DrawWrappedCaption(groups[g].note, D2D1::Point2F(note.left, note.top),
                                            note.right - note.left, theme.text_secondary);
            }
            if (g + 1 < rows.size()) format_divider(rows[g].bounds.bottom);
        }
        for (const auto& item : chips)
            chip(item.rect, groups[item.group].extensions[item.index], PreviewFormatSupport(vm, item.group, item.index));
    }

    text(pack_text::SystemTitle(), lay.preview_codec_section);
    card(lay.preview_codec_group);
    const bool detected = (vm.settings_preview_codecs & kPreviewCodecsDetected) != 0;
    for (int i = 0; i < kPreviewCodecCount; ++i) {
        const auto info = PreviewCodec(i);
        const auto& r = lay.preview_codec_row[i];
        const auto& bounds = lay.preview_codec_text[i];
        const float title_h = 26*s*static_cast<float>(vm.settings_ui_font_scale)/100;
        DrawIconText(r.left + 16*s, r.top + 17*s, 24*s, 24*s,
            i == 1 || i == 2 ? L"\xE714" : L"\xE91B", L"", theme.text_secondary, 0.85f);
        text(info.name, D2D1::RectF(bounds.left, bounds.top, bounds.right, bounds.top + title_h));
        painter_.DrawWrappedCaption(info.description, D2D1::Point2F(bounds.left, bounds.top + title_h),
            bounds.right - bounds.left, theme.text_secondary);
        const bool has = (vm.settings_preview_codecs & (1u << i)) != 0;
        const auto status = !detected ? l10n::Pick(L"检测中", L"Checking") : has
            ? l10n::Pick(L"已安装", L"Installed") : l10n::Pick(L"未安装", L"Not installed");
        fluent::BadgeSpec badge;
        badge.bounds = lay.preview_codec_badge[i]; badge.text = status;
        badge.kind = !detected ? fluent::BadgeKind::Neutral : has ? fluent::BadgeKind::Success : fluent::BadgeKind::Warning;
        painter_.DrawBadge(badge);
        const auto& get = lay.preview_codec_button[i];
        if (get.right > get.left) {
            fluent::ControlState state{};
            state.hovered = IsHovered(vm, H::SettingsPreviewStore, i);
            painter_.DrawButton({get, l10n::Pick(L"获取", L"Get"), {}, fluent::ButtonKind::Standard, state});
        }
        divider_below(r);
    }
    painter_.DrawWrappedCaption(pack_text::SystemHint(vm),
        D2D1::Point2F(lay.preview_codec_hint.left, lay.preview_codec_hint.top),
        lay.preview_codec_hint.right - lay.preview_codec_hint.left, theme.text_secondary);
}

} // namespace luling::ui
