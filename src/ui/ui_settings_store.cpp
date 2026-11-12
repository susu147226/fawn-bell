// ui_settings_store.cpp — 扩展商店 (settings page 15).
//
// 浏览商店 mirrors Pulse's page: refresh and catalog state, the trust notice,
// a link to the pack settings, the official packs as cards, then the community
// list fetched from the GitHub catalog. 已安装扩展 and 管理增强包 summarize the
// packs that ship with Luling.
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace luling::ui {

void MainRenderer::DrawSettingsStore(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                     const Theme& theme) {
    using H = HitTestResult;
    auto* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc) return;
    const auto lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_,
                                        &painter_);
    const float s = scale_;
    const int tab = std::clamp(vm.settings_store_tab, 0, 2);
    const l10n::StringId labels[3] = {l10n::StringId::StoreBrowse, l10n::StringId::StoreInstalled,
                                      l10n::StringId::StoreManage};
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F tab_rc = SettingsStoreTabRect(lay, s, i);
        const bool selected = tab == i;
        const bool hot = IsHovered(vm, H::SettingsStoreTab, i);
        MakeBrush(dc, selected ? theme.fill_selected
                     : hot ? theme.fill_hover : theme.surface_card, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), tab_rc.left, tab_rc.top,
                        tab_rc.right - tab_rc.left, tab_rc.bottom - tab_rc.top,
                        theme.radius_control * s);
        if (selected) {
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(
                D2D1::RoundedRect(tab_rc, theme.radius_control * s, theme.radius_control * s),
                brStrokeCard_.get(), 1.0f);
        }
        painter_.DrawText(l10n::Get(labels[i]), tab_rc, compositor_->SmallFormat(),
                          selected ? theme.accent : theme.text_secondary,
                          fluent::HorizontalAlignment::Center);
    }
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);

    const float left = lay.content.left + 20.0f * s;
    const float right = lay.content.right - 20.0f * s;

    auto card = [&](D2D1_RECT_F r) {
        MakeBrush(dc, theme.fill_input, brFillInput_);
        FillRoundedRect(dc, brFillInput_.get(), r.left, r.top, r.right - r.left,
                        r.bottom - r.top, 8 * s);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 8 * s, 8 * s), brStrokeCard_.get(), 1.0f);
    };
    auto section = [&](std::wstring_view text, float y) {
        painter_.DrawText(text, D2D1::RectF(left, y, right, y + 22 * s),
                          compositor_->SmallFormat(), theme.text_secondary);
    };
    auto store_card = [&](D2D1_RECT_F r, const wchar_t* glyph, std::wstring_view title,
                          std::wstring_view desc, std::wstring_view meta,
                          std::wstring_view button, bool enabled, bool installed, int index) {
        card(r);
        DrawIconText(r.left + 16 * s, r.top + 16 * s, 22 * s, 22 * s, glyph, L"",
                     theme.text_secondary, 0.85f);
        painter_.DrawText(title, D2D1::RectF(r.left + 50 * s, r.top + 12 * s,
                                             r.right - 140 * s, r.top + 34 * s),
                          compositor_->TextFormat(), theme.text);
        DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->SmallFormat(),
                            brTextSecondary_.get(), std::wstring(desc), r.left + 50 * s,
                            r.top + 36 * s,
                            (std::max)(1.0f, r.right - 140 * s - r.left - 50 * s), 20 * s);
        painter_.DrawText(meta, D2D1::RectF(r.left + 16 * s, r.bottom - 28 * s,
                                            r.right - 140 * s, r.bottom - 10 * s),
                          compositor_->SmallFormat(), theme.text_secondary);
        if (installed) {
            painter_.DrawBadge({D2D1::RectF(r.right - 120 * s, r.bottom - 44 * s,
                                            r.right - 16 * s, r.bottom - 14 * s),
                                l10n::Get(l10n::StringId::StoreInstalledBadge),
                                fluent::BadgeKind::Accent});
        } else {
            const D2D1_RECT_F button_rc = SettingsStoreCardButtonRect(r, s);
            fluent::ControlState state{};
            state.enabled = enabled;
            state.hovered = enabled && IsHovered(vm, H::SettingsStoreOfficial, index);
            painter_.DrawButton({button_rc, button, {}, fluent::ButtonKind::Primary, state});
        }
    };

    if (tab == 0) {
        // Refresh and the catalog state.
        {
            const D2D1_RECT_F refresh = SettingsStoreRefreshRect(lay, s);
            fluent::ControlState state{};
            state.enabled = vm.settings_store_catalog_state != 1;
            state.hovered = IsHovered(vm, H::SettingsStoreRefresh);
            painter_.DrawButton({refresh, l10n::Pick(L"刷新", L"Refresh"), {},
                                 fluent::ButtonKind::Standard, state});
            const int catalog = vm.settings_store_catalog_state;
            const std::wstring badge = catalog == 1
                ? l10n::Pick(L"正在加载目录…", L"Loading catalog…")
                : catalog == 2
                    ? (vm.settings_store_from_cache
                           ? l10n::Pick(L"缓存目录", L"Cached catalog")
                           : l10n::Pick(L"目录已更新", L"Catalog updated"))
                    : l10n::Pick(L"离线目录", L"Offline catalog");
            painter_.DrawBadge({SettingsStoreStatusRect(lay, s), badge,
                                catalog == 2 ? fluent::BadgeKind::Accent
                                             : fluent::BadgeKind::Warning});
        }
        // Trust notice.
        {
            const D2D1_RECT_F notice = SettingsStoreNoticeRect(lay, s);
            auto fill = theme.accent;
            fill.a = 0.08f;
            MakeBrush(dc, fill, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), notice.left, notice.top,
                            notice.right - notice.left, notice.bottom - notice.top, 6 * s);
            painter_.DrawWrappedCaption(
                l10n::Pick(L"社区增强包由各自发布者提供。原生扩展不在沙箱中运行；安装前请核对来源并审阅权限。",
                           L"Community packs come from their own publishers. Native extensions do not run in a sandbox; check the source and review permissions before installing."),
                D2D1::Point2F(notice.left + 12 * s, notice.top + 8 * s),
                (std::max)(1.0f, notice.right - notice.left - 24 * s), theme.text_secondary);
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        }
        // Link to the pack options.
        {
            const D2D1_RECT_F link = SettingsStoreLinkRect(lay, s);
            const bool hot = IsHovered(vm, H::SettingsStoreLinkPacks);
            painter_.DrawText(
                l10n::Pick(L"官方预览、搜索和 AI 增强包与下载源设置 →",
                           L"Official preview, search and AI packs and download sources →"),
                link, compositor_->SmallFormat(), hot ? theme.accent_hover : theme.accent);
            if (hot) {
                MakeBrush(dc, theme.accent_hover, brAccentText_);
                dc->DrawLine(D2D1::Point2F(link.left, link.bottom - 2 * s),
                             D2D1::Point2F(link.left + 260 * s, link.bottom - 2 * s),
                             brAccentText_.get(), 1.0f);
            }
        }
        // 官方 cards.
        section(l10n::Pick(L"官方", L"Official"), SettingsStoreOfficialTop(lay, s) - 24 * s);
        struct Official {
            const wchar_t* glyph;
            l10n::StringId title;
            std::wstring desc;
            std::wstring meta;
            bool installed;
        };
        const Official official[3] = {
            {L"\xE714", l10n::StringId::StorePackFfmpeg,
             l10n::Pick(L"为更多视频格式生成缩略图、时长与媒体信息", L"Thumbnails, durations and media info for more video formats"),
             vm.settings_pack_media_installed ? l10n::Get(l10n::StringId::StoreInstalledBadge)
                                              : l10n::Pick(L"可下载", L"Available"),
             vm.settings_pack_media_installed},
            {L"\xEB9F", l10n::StringId::StorePackImages,
             l10n::Pick(L"直接预览 HEIC、AVIF、JPEG XL 与 HDR 图像", L"Preview HEIC, AVIF, JPEG XL and HDR images"),
             vm.settings_pack_images_installed ? l10n::Get(l10n::StringId::StoreInstalledBadge)
                                               : l10n::Pick(L"可下载", L"Available"),
             vm.settings_pack_images_installed},
            {L"\xE99A", l10n::StringId::StorePackAi,
             l10n::Pick(L"Luling.AI.exe · MCP 宿主，让外部 AI 应用整理文件", L"Luling.AI.exe · MCP host for external AI apps"),
             vm.settings_ai_host ? l10n::Get(l10n::StringId::StoreInstalledBadge)
                                 : l10n::Pick(L"可下载", L"Available"),
             vm.settings_ai_host},
        };
        for (int i = 0; i < 3; ++i) {
            const D2D1_RECT_F r = SettingsStoreOfficialCardRect(lay, s, i);
            store_card(r, official[i].glyph, l10n::Get(official[i].title), official[i].desc,
                       official[i].meta, l10n::Pick(L"下载并安装", L"Download and install"),
                       true, official[i].installed, i);
        }
        // 社区增强包.
        section(l10n::Pick(L"社区增强包", L"Community extensions"),
                SettingsStoreCommunityTop(lay, s) - 24 * s);
        if (vm.settings_store_community.empty()) {
            const std::wstring empty = vm.settings_store_catalog_state == 1
                ? l10n::Pick(L"正在加载社区扩展…", L"Loading community extensions…")
                : vm.settings_store_catalog_state == 2
                    ? l10n::Pick(L"还没有社区扩展", L"No community extensions yet")
                    : l10n::Pick(L"离线目录：暂时无法获取社区扩展", L"Offline catalog: community extensions are unavailable");
            painter_.DrawText(empty,
                D2D1::RectF(left + 4 * s, SettingsStoreCommunityTop(lay, s),
                            right, SettingsStoreCommunityTop(lay, s) + 24 * s),
                compositor_->SmallFormat(), theme.text_secondary);
        } else {
            for (size_t i = 0; i < vm.settings_store_community.size(); ++i) {
                const auto& item = vm.settings_store_community[i];
                const D2D1_RECT_F r = SettingsStoreCommunityCardRect(lay, s, static_cast<int>(i));
                card(r);
                DrawIconText(r.left + 16 * s, r.top + 16 * s, 22 * s, 22 * s, L"\xE7B8", L"",
                             theme.text_secondary, 0.85f);
                painter_.DrawText(item.name, D2D1::RectF(r.left + 50 * s, r.top + 12 * s,
                                                         r.right - 140 * s, r.top + 34 * s),
                                  compositor_->TextFormat(), theme.text);
                DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->SmallFormat(),
                                    brTextSecondary_.get(), item.desc, r.left + 50 * s,
                                    r.top + 36 * s,
                                    (std::max)(1.0f, r.right - 140 * s - r.left - 50 * s), 20 * s);
                std::wstring meta = item.version;
                if (!item.size_text.empty()) {
                    if (!meta.empty()) meta += L" · ";
                    meta += item.size_text;
                }
                painter_.DrawText(meta, D2D1::RectF(r.left + 16 * s, r.bottom - 28 * s,
                                                    r.right - 140 * s, r.bottom - 10 * s),
                                  compositor_->SmallFormat(), theme.text_secondary);
                if (item.installed) {
                    painter_.DrawBadge({D2D1::RectF(r.right - 120 * s, r.bottom - 44 * s,
                                                    r.right - 16 * s, r.bottom - 14 * s),
                                        l10n::Get(l10n::StringId::StoreInstalledBadge),
                                        fluent::BadgeKind::Accent});
                } else {
                    const D2D1_RECT_F button_rc = SettingsStoreCardButtonRect(r, s);
                    fluent::ControlState state{};
                    state.enabled = true;
                    state.hovered = IsHovered(vm, H::SettingsStoreInstall, static_cast<int>(i));
                    painter_.DrawButton({button_rc, l10n::Pick(L"下载并安装", L"Download and install"),
                                         {}, fluent::ButtonKind::Primary, state});
                }
            }
        }
        if (!vm.settings_store_install_error.empty()) {
            const float y = SettingsStoreCommunityTop(lay, s) +
                static_cast<float>(vm.settings_store_community.size()) * 100 * s + 8 * s;
            painter_.DrawText(vm.settings_store_install_error, D2D1::RectF(left, y, right, y + 20 * s),
                              compositor_->SmallFormat(), theme.text_secondary);
        }
        return;
    }

    // 已安装扩展 / 管理增强包: the packs Luling ships with.
    auto note = [&](l10n::StringId id, float height) {
        const float top = SettingsStoreBodyTop(lay, s);
        const D2D1_RECT_F card_rc = D2D1::RectF(left, top, right, top + height * s);
        card(card_rc);
        painter_.DrawWrappedCaption(l10n::Get(id),
            D2D1::Point2F(card_rc.left + 16 * s, card_rc.top + 14 * s),
            (std::max)(1.0f, card_rc.right - card_rc.left - 32 * s), theme.text_secondary);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    };

    struct PackLine {
        l10n::StringId name;
        bool installed;
    };
    const PackLine packs[3] = {
        {l10n::StringId::StorePackFfmpeg, vm.settings_pack_media_installed},
        {l10n::StringId::StorePackImages, vm.settings_pack_images_installed},
        {l10n::StringId::StorePackAi, vm.settings_ai_host},
    };
    if (tab == 1) {
        // 已安装扩展: community extensions first, then the built-in packs.
        float y = SettingsStoreBodyTop(lay, s);
        section(l10n::Pick(L"社区扩展", L"Community extensions"), y);
        y += 26 * s;
        if (vm.settings_store_installed.empty()) {
            painter_.DrawText(
                l10n::Pick(L"没有已安装的社区扩展", L"No community extensions installed"),
                D2D1::RectF(left + 4 * s, y, right, y + 22 * s), compositor_->SmallFormat(),
                theme.text_secondary);
            y += 30 * s;
        } else {
            for (const auto& ext : vm.settings_store_installed) {
                const D2D1_RECT_F row = D2D1::RectF(left, y, right, y + 64 * s);
                card(row);
                painter_.DrawText(ext.name + (ext.version.empty() ? L"" : L"  v" + ext.version),
                    D2D1::RectF(row.left + 16 * s, row.top + 10 * s, row.right - 120 * s,
                                row.top + 32 * s),
                    compositor_->TextFormat(), theme.text);
                if (!ext.permissions.empty())
                    DrawTextEndEllipsis(dc, compositor_->DwriteFactory(),
                                        compositor_->SmallFormat(), brTextSecondary_.get(),
                                        ext.permissions, row.left + 16 * s, row.top + 34 * s,
                                        (std::max)(1.0f, row.right - 140 * s - row.left), 20 * s);
                painter_.DrawBadge({D2D1::RectF(row.right - 112 * s, row.top + 16 * s,
                                                row.right - 16 * s, row.top + 40 * s),
                                    l10n::Pick(ext.enabled ? L"已启用" : L"已停用",
                                               ext.enabled ? L"Enabled" : L"Disabled"),
                                    ext.enabled ? fluent::BadgeKind::Success
                                                : fluent::BadgeKind::Neutral});
                y += 70 * s;
            }
        }
        y += 8 * s;
        section(l10n::Pick(L"内置增强包", L"Built-in packs"), y);
        y += 26 * s;
        for (const PackLine& pack : packs) {
            const D2D1_RECT_F row = D2D1::RectF(left, y, right, y + 46 * s);
            card(row);
            painter_.DrawText(l10n::Get(pack.name),
                D2D1::RectF(row.left + 16 * s, row.top, row.right - 130 * s, row.bottom),
                compositor_->TextFormat(), theme.text);
            painter_.DrawBadge({D2D1::RectF(row.right - 126 * s, row.top + 13 * s,
                                           row.right - 16 * s, row.bottom - 13 * s),
                                l10n::Get(pack.installed ? l10n::StringId::StoreInstalledBadge
                                                         : l10n::StringId::StoreNotInstalledBadge),
                                pack.installed ? fluent::BadgeKind::Accent
                                               : fluent::BadgeKind::Neutral});
            y += 52 * s;
        }
        return;
    }

    note(l10n::StringId::StoreManageHint, 62.0f);
    for (int index = 0; index < 3; ++index) {
        const PackLine& pack = packs[index];
        const D2D1_RECT_F row = SettingsStorePackRowRect(lay, s, tab, index);
        const bool hot = IsHovered(vm, H::SettingsStorePackRow, index);
        MakeBrush(dc, hot ? theme.fill_hover : theme.fill_input, brFillInput_);
        FillRoundedRect(dc, brFillInput_.get(), row.left, row.top, row.right - row.left,
                        row.bottom - row.top, 8 * s);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(row, 8 * s, 8 * s), brStrokeCard_.get(), 1.0f);
        painter_.DrawText(l10n::Get(pack.name),
            D2D1::RectF(row.left + 16 * s, row.top, row.right - 130 * s, row.bottom),
            compositor_->TextFormat(), theme.text);
        painter_.DrawBadge({D2D1::RectF(row.right - 126 * s, row.top + 13 * s,
                                       row.right - 16 * s, row.bottom - 13 * s),
                            l10n::Get(pack.installed ? l10n::StringId::StoreInstalledBadge
                                                     : l10n::StringId::StoreNotInstalledBadge),
                            pack.installed ? fluent::BadgeKind::Accent
                                           : fluent::BadgeKind::Neutral});
    }
    {
        const D2D1_RECT_F open_rc = SettingsStoreOpenPacksRect(lay, s);
        fluent::ControlState state{};
        state.enabled = true;
        state.hovered = IsHovered(vm, H::SettingsStoreOpenPacks);
        painter_.DrawButton({open_rc, l10n::Get(l10n::StringId::StoreOpenPacks), {},
                             fluent::ButtonKind::Primary, state});
    }
}

} // namespace luling::ui
