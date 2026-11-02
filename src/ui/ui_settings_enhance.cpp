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

    if (lay.footer.bottom > lay.footer.top)
        text(l10n::Get(l10n::StringId::SettingsImmediate), lay.footer, true);
}

} // namespace luling::ui
