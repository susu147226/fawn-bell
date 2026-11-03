// ui_settings_enhance.cpp - Settings > 增强包 (page 14): only the packs that
// can be downloaded here; their usage switches stay on 快速预览.
#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace luling::ui {

void MainRenderer::DrawSettingsEnhance(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                       const Theme& theme) {
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
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8 * s, 8 * s), brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 8 * s, 8 * s), brStrokeCard_.get(), 1);
    };
    auto hovered = [&](PackAction action) {
        return IsHovered(vm, H::SettingsPackAction, static_cast<int>(action));
    };
    auto button = [&](D2D1_RECT_F r, std::wstring_view label, PackAction action, bool enabled,
                      fluent::ButtonKind kind) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.enabled = enabled;
        state.hovered = enabled && hovered(action);
        painter_.DrawButton({r, label, {}, kind, state});
    };
    auto divider = [&](float y) {
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), lay.content.left + 20 * s + 16 * s, y,
                 lay.content.right - lay.content.left - 40 * s - 32 * s, 1);
    };
    auto hover_row = [&](D2D1_RECT_F r, PackAction action) {
        if (r.bottom <= r.top || !hovered(action)) return;
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), r.left + 4 * s, r.top + 2 * s,
                        r.right - r.left - 8 * s, r.bottom - r.top - 4 * s, 4 * s);
    };
    auto toggle = [&](D2D1_RECT_F bounds, bool on, PackAction action) {
        fluent::ControlState state{};
        state.checked = on;
        state.hovered = hovered(action);
        painter_.DrawSwitch(bounds, L"", state);
    };
    auto row_switch = [&](D2D1_RECT_F row) {
        const float cy = (row.top + row.bottom) * 0.5f;
        return D2D1::RectF(row.right - 16 * s - 42 * s, cy - 16 * s,
                           row.right - 16 * s, cy + 16 * s);
    };
    auto setting = [&](D2D1_RECT_F r, const wchar_t* icon, std::wstring_view title,
                       std::wstring_view desc) {
        const float right = r.right - 72 * s;
        DrawIconText(r.left + 16 * s, r.top + 20 * s, 24 * s, 24 * s, icon, L"",
                     theme.text_secondary, 0.85f);
        text(title, D2D1::RectF(r.left + 54 * s, r.top + 10 * s, right, r.top + 34 * s));
        painter_.DrawWrappedCaption(desc, D2D1::Point2F(r.left + 54 * s, r.top + 35 * s),
                                    right - r.left - 54 * s, theme.text_secondary);
    };
    auto pack = [&](D2D1_RECT_F card_rc, D2D1_RECT_F badge_rc, D2D1_RECT_F primary_rc,
                    D2D1_RECT_F update_rc, const wchar_t* icon, std::wstring_view title,
                    std::wstring_view desc, std::wstring_view meta,
                    const pack_text::Badge& badge, std::wstring_view primary_label,
                    PackAction primary_action, bool primary_enabled, std::wstring_view update_label,
                    PackAction update_action) {
        card(card_rc);
        DrawIconText(card_rc.left + 16 * s, card_rc.top + 16 * s, 22 * s, 22 * s, icon, L"",
                     theme.text_secondary, 0.85f);
        const float text_left = card_rc.left + 50 * s;
        const float text_right = primary_rc.left - 16 * s;
        text(title, D2D1::RectF(text_left, card_rc.top + 12 * s, text_right, card_rc.top + 36 * s));
        painter_.DrawWrappedCaption(desc, D2D1::Point2F(text_left, card_rc.top + 36 * s),
                                    text_right - text_left, theme.text_secondary);
        text(meta, D2D1::RectF(text_left, card_rc.bottom - 28 * s, text_right, card_rc.bottom - 10 * s), true);
        painter_.DrawBadge({badge_rc, badge.text, badge.kind});
        if (primary_enabled)
            button(primary_rc, primary_label, primary_action, true, fluent::ButtonKind::Primary);
        if (update_rc.right > update_rc.left && !update_label.empty())
            button(update_rc, update_label, update_action, true, fluent::ButtonKind::Standard);
    };

    text(L"AI 增强包", lay.ai_section);
    {
        card(lay.ai_card);
        DrawIconText(lay.ai_card.left + 16 * s, lay.ai_card.top + 16 * s, 22 * s, 22 * s, L"é45",
                     L"", theme.text_secondary, 0.85f);
        const float text_left = lay.ai_card.left + 50 * s;
        const float text_right = lay.ai_badge.left - 16 * s;
        text(L"AI 增强包", D2D1::RectF(text_left, lay.ai_card.top + 12 * s, text_right,
                                       lay.ai_card.top + 36 * s));
        painter_.DrawWrappedCaption(
            L"Luling.AI.exe · MCP 宿主：让 Claude 等外部 AI 应用查找、整理你的文件；权限、确认和活动记录在「AI 与 MCP」里设置。",
            D2D1::Point2F(text_left, lay.ai_card.top + 36 * s), text_right - text_left,
            theme.text_secondary);
        text(L"Luling.AI.exe · MCP · 随鹿铃同步更新",
             D2D1::RectF(text_left, lay.ai_card.bottom - 28 * s, text_right, lay.ai_card.bottom - 10 * s), true);
        painter_.DrawBadge({lay.ai_badge, vm.settings_ai_host ? L"已安装" : L"未安装",
                            vm.settings_ai_host ? fluent::BadgeKind::Accent : fluent::BadgeKind::Neutral});
    }

    text(pack_text::Media(), lay.pack_media_section);
    pack(lay.pack_card, lay.pack_badge, lay.pack_primary, lay.pack_update, L"\xE714",
         pack_text::MediaTitle(), pack_text::MediaDesc(), pack_text::MediaMeta(),
         pack_text::MediaBadge(vm), pack_text::Primary(vm),
         vm.settings_pack_media_installed && !vm.settings_pack_installing ? PackAction::Remove
                                                                         : PackAction::Install,
         pack_text::PrimaryEnabled(vm.settings_pack_media_available, vm.settings_pack_media_installed,
                                   vm.settings_pack_installing),
         pack_text::MediaUpdate(vm), PackAction::Update);

    text(pack_text::Images(), lay.pack_images_section);
    pack(lay.pack_images_card, lay.pack_images_badge, lay.pack_images_primary, lay.pack_images_update,
         L"\xEB9F", pack_text::ImagesTitle(), pack_text::ImagesDesc(), pack_text::ImagesMeta(),
         pack_text::ImagesBadge(vm), pack_text::ImagesPrimary(vm),
         vm.settings_pack_images_installed && !vm.settings_pack_images_installing
             ? PackAction::ImagesRemove : PackAction::ImagesInstall,
         pack_text::PrimaryEnabled(vm.settings_pack_images_available, vm.settings_pack_images_installed,
                                   vm.settings_pack_images_installing),
         pack_text::ImagesUpdate(vm), PackAction::ImagesUpdate);

    pack(lay.pack_raw_card, lay.pack_raw_badge, lay.pack_raw_primary, lay.pack_raw_update, L"\xE91B",
         pack_text::RawTitle(), pack_text::RawDesc(), pack_text::RawMeta(), pack_text::RawBadge(vm),
         pack_text::RawPrimary(vm),
         vm.settings_pack_raw_installed && !vm.settings_pack_raw_installing ? PackAction::RawRemove
                                                                           : PackAction::RawInstall,
         pack_text::PrimaryEnabled(vm.settings_pack_raw_available, vm.settings_pack_raw_installed,
                                   vm.settings_pack_raw_installing),
         pack_text::RawUpdate(vm), PackAction::RawUpdate);

    text(pack_text::Archive(), lay.pack_archive_section);
    pack(lay.pack_archive_card, lay.pack_archive_badge, lay.pack_archive_primary,
         lay.pack_archive_update, L"\xE7B8", pack_text::ArchiveTitle(), pack_text::ArchiveDesc(),
         pack_text::ArchiveMeta(), pack_text::ArchiveBadge(vm), pack_text::ArchivePrimary(vm),
         vm.settings_pack_archive_installed && !vm.settings_pack_archive_installing
             ? PackAction::ArchiveRemove : PackAction::ArchiveInstall,
         pack_text::PrimaryEnabled(vm.settings_pack_archive_available, vm.settings_pack_archive_installed,
                                   vm.settings_pack_archive_installing),
         pack_text::ArchiveUpdate(vm), PackAction::ArchiveUpdate);

    text(pack_text::TranscodeSection(), lay.pack_transcode_section);
    pack(lay.pack_transcode_card, lay.pack_transcode_badge, lay.pack_transcode_primary,
         lay.pack_transcode_update, L"\xE8B2", pack_text::TranscodeTitle(), pack_text::TranscodeDesc(),
         pack_text::TranscodeMeta(), pack_text::TranscodeBadge(vm), pack_text::TranscodePrimary(vm),
         vm.settings_pack_transcode_installed && !vm.settings_pack_transcode_installing
             ? PackAction::TranscodeRemove : PackAction::TranscodeInstall,
         pack_text::PrimaryEnabled(vm.settings_pack_transcode_available,
                                   vm.settings_pack_transcode_installed,
                                   vm.settings_pack_transcode_installing),
         pack_text::TranscodeUpdate(vm), PackAction::TranscodeUpdate);

    const bool pack_busy = vm.settings_pack_installing || vm.settings_pack_images_installing ||
        vm.settings_pack_raw_installing || vm.settings_pack_archive_installing ||
        vm.settings_pack_transcode_installing;
    button(lay.pack_install_all, l10n::Get(l10n::StringId::PackInstallAll), PackAction::InstallAll,
           !pack_busy, fluent::ButtonKind::Standard);

    // 媒体来源: the FFmpeg pack, an existing ffmpeg, or neither.
    text(l10n::Pick(L"媒体来源", L"Media source"), lay.pack_section);
    card(lay.pack_source_group);
    {
        const auto& r = lay.pack_source_status;
        const bool source_switch = pack_text::ShowsEnable(vm);
        const float right = source_switch ? lay.pack_enable.left - 14 * s : r.right - 16 * s;
        text(pack_text::SourceTitle(),
             D2D1::RectF(r.left + 16 * s, r.top + 10 * s, right, r.top + 34 * s));
        painter_.DrawWrappedCaption(
            pack_text::SourceStatus(vm), D2D1::Point2F(r.left + 16 * s, r.top + 35 * s),
            right - r.left - 16 * s, theme.text_secondary);
        if (source_switch) toggle(lay.pack_enable, vm.settings_pack_ffmpeg_enabled, PackAction::Enable);
        divider(r.bottom);
    }
    hover_row(lay.pack_custom_row, PackAction::UseCustom);
    setting(lay.pack_custom_row, L"\xE943", pack_text::CustomTitle(), pack_text::CustomDesc());
    toggle(row_switch(lay.pack_custom_row), vm.settings_pack_use_custom, PackAction::UseCustom);
    divider(lay.pack_custom_row.bottom);
    if (lay.pack_path_row.bottom > lay.pack_path_row.top) {
        const auto& r = lay.pack_path_row;
        const float right = r.right - 16 * s;
        const std::wstring path = vm.settings_pack_custom_path.empty()
            ? std::wstring(pack_text::NoCustom()) : vm.settings_pack_custom_path;
        const bool active = vm.settings_pack_use_custom && vm.settings_pack_ffmpeg == 2;
        if (!pack_text::ShowsDetect(vm)) {
            painter_.DrawText(path, D2D1::RectF(r.left + 54 * s, r.top + 14 * s, right, r.top + 38 * s),
                              compositor_->SmallFormat(),
                              active ? theme.text : theme.text_secondary);
        } else {
            painter_.DrawText(path, D2D1::RectF(r.left + 54 * s, r.top + 12 * s, right, r.top + 34 * s),
                              compositor_->SmallFormat(),
                              active ? theme.text : theme.text_secondary);
            text(pack_text::Found(vm.settings_pack_detected_path),
                 D2D1::RectF(r.left + 54 * s, r.top + 34 * s, right, r.top + 54 * s), true);
        }
        button(lay.pack_detect, pack_text::UseDetected(), PackAction::UseDetected, true,
               fluent::ButtonKind::Standard);
        button(lay.pack_browse, pack_text::Browse(), PackAction::Browse, true,
               fluent::ButtonKind::Standard);
    }

    text(pack_text::Advanced(), lay.pack_advanced_section);
    card(lay.pack_group);
    hover_row(lay.pack_auto_update_row, PackAction::AutoUpdate);
    setting(lay.pack_auto_update_row, L"\xE895", pack_text::AutoUpdateTitle(), pack_text::AutoUpdateDesc());
    toggle(row_switch(lay.pack_auto_update_row), vm.settings_pack_auto_update, PackAction::AutoUpdate);
    divider(lay.pack_auto_update_row.bottom);
    hover_row(lay.pack_remove_row, PackAction::RemoveOnUninstall);
    setting(lay.pack_remove_row, L"\xE74D", pack_text::RemoveTitle(), pack_text::RemoveDesc());
    toggle(row_switch(lay.pack_remove_row), vm.settings_pack_remove_on_uninstall,
           PackAction::RemoveOnUninstall);

    painter_.DrawWrappedCaption(pack_text::Note(),
                                D2D1::Point2F(lay.pack_note.left + 4 * s, lay.pack_note.top),
                                lay.pack_note.right - lay.pack_note.left - 8 * s,
                                theme.text_secondary);

    if (lay.footer.bottom > lay.footer.top)
        text(l10n::Get(l10n::StringId::SettingsImmediate), lay.footer, true);
}

} // namespace luling::ui
