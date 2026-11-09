// settings_pages_layout.h - Settings pages 0 and 7-10, split the way the app
// 1.0.58 groups them: 外观 (主题与颜色 / 背景与透明度 / 文字与语言),
// 文件列表 (打开与选择 / 显示哪些项目 / 列表外观), 标签页与窗口 (打开位置 /
// 标签页), 启动与集成 (启动与托盘 / 资源管理器), 文件操作 (删除 / 暂存区 /
// 变化记录 / 新手提示). Page 13 is the keyboard shortcut list.
// Included by ui_renderer_internal.h after SettingsLayout.
// 标签页管理 row geometry, shared by drawing and hit testing.
inline D2D1_RECT_F TabManageColorRect(const D2D1_RECT_F& row, float s) {
    const float cy = (row.top + row.bottom) * 0.5f;
    return D2D1::RectF(row.left + 16 * s, cy - 8 * s, row.left + 32 * s, cy + 8 * s);
}
inline D2D1_RECT_F TabManageCloseRect(const D2D1_RECT_F& row, float s) {
    return D2D1::RectF(row.right - 16 * s - 28 * s, row.top + 14 * s, row.right - 16 * s,
                       row.top + 42 * s);
}
inline D2D1_RECT_F TabManageRenameRect(const D2D1_RECT_F& row, float s) {
    const auto close = TabManageCloseRect(row, s);
    return D2D1::RectF(close.left - 8 * s - 56 * s, row.top + 14 * s, close.left - 8 * s,
                       row.top + 42 * s);
}
inline D2D1_RECT_F TabManageGroupRect(const D2D1_RECT_F& row, float s) {
    const auto rename = TabManageRenameRect(row, s);
    return D2D1::RectF(rename.left - 8 * s - 96 * s, row.top + 14 * s, rename.left - 8 * s,
                       row.top + 42 * s);
}

namespace pages_text {
inline const wchar_t* TabsManage() { return l10n::Pick(L"标签页管理", L"Manage tabs"); }
inline const wchar_t* TabsManageDesc() {
    return l10n::Pick(L"点一行切换到该标签；点色点换颜色；点分组打开分组菜单",
                      L"Click a row to switch, a dot to recolour, a group to edit it");
}
inline const wchar_t* TabRenameButton() { return l10n::Pick(L"改名", L"Rename"); }
inline const wchar_t* TabGroupHint() { return l10n::Pick(L"分组", L"Group"); }
inline const wchar_t* ThumbCacheTitle() { return l10n::Pick(L"缩略图缓存", L"Thumbnail cache"); }
inline const wchar_t* ThumbCacheDesc() {
    return l10n::Pick(L"耗时生成的缩略图保存在本机，再次打开文件夹时直接显示",
                      L"Slow thumbnails are kept on this PC so folders open instantly");
}
inline const wchar_t* ThumbCacheClear() { return l10n::Pick(L"清除缓存", L"Clear cache"); }
inline const wchar_t* AppearanceTheme() { return l10n::Pick(L"主题与颜色", L"Theme and colour"); }
inline const wchar_t* AppearanceBackdrop() { return l10n::Pick(L"背景与透明度", L"Background and transparency"); }
inline const wchar_t* AppearanceText() { return l10n::Pick(L"文字与语言", L"Text and language"); }
inline const wchar_t* FileListOpen() { return l10n::Pick(L"打开与选择", L"Open and select"); }
inline const wchar_t* FileListShow() { return l10n::Pick(L"显示哪些项目", L"What to show"); }
inline const wchar_t* FileListLook() { return l10n::Pick(L"列表外观", L"List appearance"); }
inline const wchar_t* TabsOpen() { return l10n::Pick(L"打开位置", L"Where to open"); }
inline const wchar_t* TabsTabs() { return l10n::Pick(L"标签页", L"Tabs"); }
inline const wchar_t* StartupTray() { return l10n::Pick(L"启动与托盘", L"Startup and tray"); }
inline const wchar_t* StartupShell() { return l10n::Pick(L"资源管理器", L"File Explorer"); }
inline const wchar_t* OpsDelete() { return l10n::Pick(L"删除", L"Delete"); }
inline const wchar_t* OpsStaging() { return l10n::Pick(L"暂存区", L"Staging"); }
inline const wchar_t* OpsChanges() { return l10n::Pick(L"变化记录", L"Change history"); }
inline const wchar_t* OpsChangeDaysDesc() {
    return l10n::Pick(L"在文件夹里标出多久以内的增删改", L"How far back the folder marks changes");
}
inline const wchar_t* OpsHints() { return l10n::Pick(L"新手提示", L"Tips"); }
inline const wchar_t* ShortcutNote() {
    return l10n::Pick(L"点击任意一行即可改键；Backspace 恢复默认，Esc 取消；在输入框里按键优先用于输入。",
                      L"Click a row to rebind it; Backspace restores its default and Esc cancels. While typing, keys type instead.");
}
} // namespace pages_text

// The one-line description under each page's title, the app's wording.
inline const wchar_t* SettingsPageIntro(int page) {
    switch (page) {
    case 7: return l10n::Pick(L"打开方式、显示哪些文件，以及列表里的各种提示。",
                              L"Opening, which files to show, and the list's hints.");
    case 8: return l10n::Pick(L"启动时打开什么，新标签页和其他程序打开的文件夹去哪里。",
                              L"What opens at launch, and where new tabs and other programs open folders.");
    case 9: return l10n::Pick(L"开机启动、托盘，以及接管 Windows 里打开文件夹的方式。",
                              L"Startup, the tray, and taking over how Windows opens folders.");
    case 10: return l10n::Pick(L"删除确认、暂存区和文件夹变化记录。",
                               L"Delete confirmation, staging, and folder change history.");
    case 13: return l10n::Pick(L"鹿铃当前的键盘快捷键，和「帮助 › 快捷键」一致。",
                               L"Luling's current keyboard shortcuts, the same as Help › Shortcuts.");
    case 14: return l10n::Pick(L"下载和管理预览、媒体、图像与转换增强包。",
                               L"Download and manage preview, media, image and conversion packs.");
    default: return nullptr;
    }
}

inline float LayoutSettingsAppearance(SettingsLayout& l, const WindowViewModel& vm, float scale,
                                      float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20 * scale, right = l.content.right - 20 * scale;
    const bool narrow = right - left < 560 * scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y + h * scale); y = r.bottom; return r; };
    auto choice = [&](D2D1_RECT_F r, float width) {
        return D2D1::RectF(narrow ? r.left + 16 * scale : r.right - (width + 16) * scale,
                           r.bottom - 44 * scale, r.right - 16 * scale, r.bottom - 12 * scale);
    };
    auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
        auto c = choice(r, width);
        const float w = (c.right - c.left) / count;
        for (int i = 0; i < count; ++i) output[i] = D2D1::RectF(c.left + i * w, c.top, c.left + (i + 1) * w, c.bottom);
    };
    (void)painter;
    y += 24 * scale; l.section[0] = row(28);
    l.theme_row = row(narrow ? 142.0f : 112.0f);
    const float tw = (std::min)(96 * scale, (right - left - 32 * scale) / 3);
    const float tile_left = narrow ? left + 16 * scale : right - 16 * scale - 3 * tw;
    for (int i = 0; i < 3; ++i)
        l.theme_tile[i] = D2D1::RectF(tile_left + i * tw + 4 * scale, l.theme_row.bottom - 90 * scale,
                                      tile_left + (i + 1) * tw - 4 * scale, l.theme_row.bottom - 12 * scale);
    l.accent_card = row(96);
    const float picker = kBloomPickerDip * scale;
    l.accent_picker = D2D1::RectF(right - 16 * scale - picker,
                                  l.accent_card.top + (96 * scale - picker) / 2, right - 16 * scale,
                                  l.accent_card.top + (96 * scale + picker) / 2);
    {
        const float button_w = (std::min)(96.0f * scale, picker * 0.98f);
        const float button_h = 32.0f * scale;
        l.accent_toggle_button = D2D1::RectF(
            l.accent_picker.left - 12.0f * scale - button_w,
            l.accent_card.top + (96.0f * scale - button_h) * 0.5f,
            l.accent_picker.left - 12.0f * scale,
            l.accent_card.top + (96.0f * scale + button_h) * 0.5f);
    }
    l.accent_custom_card = {};
    if (vm.settings_accent_editor_open) {
        constexpr float kAccentRowH = 64.0f;
        l.accent_custom_card = row(52.0f + 3 * kAccentRowH);
        const float swatch = 30.0f * scale;
        l.accent_swatch = D2D1::RectF(right - 16 * scale - swatch,
                                      l.accent_custom_card.top + 12 * scale,
                                      right - 16 * scale,
                                      l.accent_custom_card.top + 12 * scale + swatch);
        l.accent_hex_rect = D2D1::RectF(l.accent_swatch.left - 92 * scale, l.accent_swatch.top,
                                        l.accent_swatch.left - 8 * scale, l.accent_swatch.bottom);
        for (int i = 0; i < 3; ++i) {
            l.accent_custom_row[i] = D2D1::RectF(
                left, l.accent_custom_card.top + (52.0f + i * kAccentRowH) * scale, right,
                l.accent_custom_card.top + (52.0f + (i + 1) * kAccentRowH) * scale);
            const auto c = choice(l.accent_custom_row[i], 282);
            const float w = (c.right - c.left) / 3;
            for (int k = 0; k < 3; ++k)
                l.accent_custom_cells[i][k] = D2D1::RectF(c.left + k * w, c.top,
                                                          c.left + (k + 1) * w, c.bottom);
        }
    }

    y += 24 * scale; l.section[1] = row(28);
    const int effect_columns = right - left < 440 * scale ? 2 : 4;
    const int effect_rows = kWindowEffectCount / effect_columns;
    const float effect_top = 66.0f;
    l.effect_card = row(effect_top + 82.0f * effect_rows + 12.0f);
    l.effect_choice = {};
    const float effect_gap = 8 * scale;
    const float effect_width = (right - left - 32 * scale - effect_gap * (effect_columns - 1)) / effect_columns;
    for (int i = 0; i < kWindowEffectCount; ++i) {
        const float tile_x = left + 16 * scale + (i % effect_columns) * (effect_width + effect_gap);
        const float tile_y = l.effect_card.top + effect_top * scale + (i / effect_columns) * 82 * scale;
        l.effect_row[i] = D2D1::RectF(tile_x, tile_y, tile_x + effect_width, tile_y + 74 * scale);
    }
    l.wallpaper_card = row(124);
    l.wallpaper_preview = D2D1::RectF(left + 16 * scale, l.wallpaper_card.top + 12 * scale,
                                      left + 112 * scale, l.wallpaper_card.top + 68 * scale);
    {
        const float cw = painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::Clear)) : 80 * scale;
        const float bw = painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::ChooseImage)) : 120 * scale;
        l.wallpaper_clear = D2D1::RectF(right - 16 * scale - cw, y - 44 * scale, right - 16 * scale, y - 12 * scale);
        l.wallpaper_choose = D2D1::RectF(l.wallpaper_clear.left - 8 * scale - bw, y - 44 * scale,
                                         l.wallpaper_clear.left - 8 * scale, y - 12 * scale);
    }
    l.wallpaper_look_card = row(narrow ? 98.0f : 64.0f); segments(l.wallpaper_look_card, l.wallpaper_look_row, 3, 282);
    l.wallpaper_blur_card = row(narrow ? 98.0f : 64.0f); segments(l.wallpaper_blur_card, l.wallpaper_blur_row, 3, 282);

    y += 24 * scale; l.section[2] = row(28);
    l.language_card = row(narrow ? 98.0f : 64.0f); l.language_choice = choice(l.language_card, 176);
    l.text_render_card = row(narrow ? 98.0f : 64.0f); segments(l.text_render_card, l.text_render_row, 3, 282);
    l.ui_font_size_card = row(narrow ? 98.0f : 64.0f); segments(l.ui_font_size_card, l.ui_font_size_row, 4, 340);
    l.title_clock_row = row(narrow ? 98.0f : 64.0f);
    y += 12 * scale; l.footer = row(28); y += 16 * scale;
    return y;
}

inline float LayoutSettingsFileList(SettingsLayout& l, const WindowViewModel& vm, float scale,
                                    float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20 * scale, right = l.content.right - 20 * scale;
    const bool narrow = right - left < 560 * scale;
    (void)vm;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y + h * scale); y = r.bottom; return r; };
    auto choice = [&](D2D1_RECT_F r, float width) {
        return D2D1::RectF(narrow ? r.left + 16 * scale : r.right - (width + 16) * scale,
                           r.bottom - 44 * scale, r.right - 16 * scale, r.bottom - 12 * scale);
    };
    auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
        auto c = choice(r, width);
        const float w = (c.right - c.left) / count;
        for (int i = 0; i < count; ++i) output[i] = D2D1::RectF(c.left + i * w, c.top, c.left + (i + 1) * w, c.bottom);
    };
    y += 24 * scale; l.section[0] = row(28);
    l.blank_click_row = row(narrow ? 98.0f : 64.0f);
    segments(l.blank_click_row, l.blank_click_choice, 3, 282);
    l.single_click_row = row(narrow ? 98.0f : 64.0f);
    l.checkbox_row = row(narrow ? 98.0f : 64.0f);

    y += 24 * scale; l.section[1] = row(28);
    l.hidden_files_row = row(64);
    l.protected_files_row = row(64);
    l.hidden_shares_row = row(narrow ? 98.0f : 64.0f);

    y += 24 * scale; l.section[2] = row(28);
    l.density_card = row(narrow ? 98.0f : 64.0f);
    segments(l.density_card, l.density_row, 3, 282);
    l.performance_row = row(64);
    for (auto& list_row : l.list_style_row) list_row = row(64);
    l.thumb_cache_row = row(narrow ? 98.0f : 72.0f);
    {
        const float bw = painter ? painter->MeasureButtonWidth(pages_text::ThumbCacheClear()) : 96 * scale;
        l.thumb_cache_button = D2D1::RectF(l.thumb_cache_row.right - 16 * scale - bw,
                                            l.thumb_cache_row.top + 20 * scale,
                                            l.thumb_cache_row.right - 16 * scale,
                                            l.thumb_cache_row.top + 52 * scale);
    }
    l.folder_sort_card = row(narrow ? 98.0f : 64.0f);
    segments(l.folder_sort_card, l.folder_sort_row, 3, 282);
    l.sticky_parent_row = row(narrow ? 98.0f : 64.0f);
    l.tree_view_row = row(narrow ? 98.0f : 64.0f);
    l.svn_status_row = row(narrow ? 98.0f : 64.0f);

    y += 24 * scale; l.section[3] = row(28);
    l.mtime_style_card = row(narrow ? 98.0f : 72.0f);
    segments(l.mtime_style_card, l.mtime_style_row, 4, 420);
    l.mtime_days_card = row(narrow ? 98.0f : 72.0f);
    segments(l.mtime_days_card, l.mtime_days_row, 4, 360);
    l.mtime_palette_card = row(narrow ? 98.0f : 72.0f);
    segments(l.mtime_palette_card, l.mtime_palette_row, 6, 520);
    for (auto& mtime_row : l.mtime_opt_row) mtime_row = row(64);
    y += 24 * scale; l.section[4] = row(28);
    l.cloud_status_row = row(narrow ? 98.0f : 64.0f);
    y += 12 * scale; l.footer = row(28); y += 16 * scale;
    return y;
}

inline float LayoutSettingsTabsWindows(SettingsLayout& l, const WindowViewModel& vm, float scale,
                                       float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20 * scale, right = l.content.right - 20 * scale;
    (void)vm;
    const bool narrow = right - left < 560 * scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y + h * scale); y = r.bottom; return r; };
    auto choice = [&](D2D1_RECT_F r, float width) {
        return D2D1::RectF(narrow ? r.left + 16 * scale : r.right - (width + 16) * scale,
                           r.bottom - 44 * scale, r.right - 16 * scale, r.bottom - 12 * scale);
    };
    auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
        auto c = choice(r, width);
        const float w = (c.right - c.left) / count;
        for (int i = 0; i < count; ++i) output[i] = D2D1::RectF(c.left + i * w, c.top, c.left + (i + 1) * w, c.bottom);
    };
    y += 24 * scale; l.section[0] = row(28);
    l.home_folder_card = row(narrow ? 98.0f : 64.0f);
    {
        const float rw = painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::ThisPc)) : 80 * scale;
        const float cw = painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::SettingsHomeFolderPick)) : 120 * scale;
        l.home_folder_reset = D2D1::RectF(right - 16 * scale - rw, y - 44 * scale, right - 16 * scale, y - 12 * scale);
        l.home_folder_choose = D2D1::RectF(l.home_folder_reset.left - 8 * scale - cw, y - 44 * scale,
                                           l.home_folder_reset.left - 8 * scale, y - 12 * scale);
    }
    l.startup_open_card = row(narrow ? 98.0f : 64.0f); segments(l.startup_open_card, l.startup_open_row, 2, 282);
    l.new_tab_open_card = row(narrow ? 98.0f : 64.0f); segments(l.new_tab_open_card, l.new_tab_open_row, 2, 282);

    y += 24 * scale; l.section[1] = row(28);
    l.close_last_tab_row = row(64);
    l.single_window_row = row(64);
    l.vertical_tabs_row = row(64);
    l.pinned_names_row = row(64);

    y += 24 * scale; l.section[2] = row(28);
    l.tab_manage_rows.clear();
    const size_t shown = std::min<size_t>(vm.settings_tab_manage.size(), 12);
    const float manage_row_h = 56 * scale;
    const float card_top = y;
    for (size_t i = 0; i < shown; ++i) {
        l.tab_manage_rows.push_back(D2D1::RectF(left, y, right, y + manage_row_h));
        y += manage_row_h;
    }
    if (shown == 0) y += manage_row_h;
    l.tab_manage_card = D2D1::RectF(left, card_top, right, y);

    y += 12 * scale; l.footer = row(28); y += 16 * scale;
    return y;
}

inline float LayoutSettingsStartupIntegration(SettingsLayout& l, const WindowViewModel& vm, float scale,
                                              float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20 * scale, right = l.content.right - 20 * scale;
    const bool narrow = right - left < 560 * scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y + h * scale); y = r.bottom; return r; };
    auto choice = [&](D2D1_RECT_F r, float width) {
        return D2D1::RectF(narrow ? r.left + 16 * scale : r.right - (width + 16) * scale,
                           r.bottom - 44 * scale, r.right - 16 * scale, r.bottom - 12 * scale);
    };
    auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
        auto c = choice(r, width);
        const float w = (c.right - c.left) / count;
        for (int i = 0; i < count; ++i) output[i] = D2D1::RectF(c.left + i * w, c.top, c.left + (i + 1) * w, c.bottom);
    };
    auto caption_height = [&](std::wstring_view value, float width) {
        return value.empty() ? 0.0f : painter
            ? painter->MeasureWrappedCaptionHeight(value, (std::max)(40 * scale, width)) : 42 * scale;
    };
    y += 24 * scale; l.section[0] = row(28);
    l.startup_row[0] = row(64);
    l.start_in_tray_row = row(64);
    l.startup_row[1] = row(64);
    l.explorer_fallback_row = row(narrow ? 98.0f : 64.0f);
    l.notify_icon_card = row(narrow ? 98.0f : 64.0f);
    segments(l.notify_icon_card, l.notify_icon_row, 3, 282);
    l.tray_icon_card = row(narrow ? 98.0f : 64.0f);
    segments(l.tray_icon_card, l.tray_icon_row, 3, 282);

    y += 24 * scale; l.section[1] = row(28);
    const float integration_top = y;
    {
        const auto badge = MakeIntegrationBadge(vm);
        const float badge_w = painter ? painter->MeasureBadgeWidth(badge.text) : 96 * scale;
        const float switch_left = right - 60 * scale;
        float text_right = switch_left - 24 * scale - badge_w;
        const bool stacked = text_right - (left + 54 * scale) < 220 * scale;
        if (stacked) text_right = switch_left - 12 * scale;
        const float desc_h = caption_height(l10n::Get(l10n::StringId::IntegrationMasterDesc), text_right - left - 54 * scale);
        const float body = 35 * scale + desc_h + (stacked ? 30 * scale : 0.0f) + 14 * scale;
        l.default_manager_row = row((std::max)(64.0f, body / scale));
        const float top = l.default_manager_row.top;
        l.integration_badge = stacked
            ? D2D1::RectF(left + 54 * scale, top + 35 * scale + desc_h + 6 * scale,
                          left + 54 * scale + badge_w, top + 35 * scale + desc_h + 28 * scale)
            : D2D1::RectF(text_right + 12 * scale, top + 21 * scale, text_right + 12 * scale + badge_w, top + 43 * scale);
        l.integration_text_right = text_right;
    }
    if (IntegrationHasProblem(vm)) {
        const bool failed = vm.settings_integration_state == 3;
        const float bar_left = left + 16 * scale, bar_right = right - 16 * scale;
        const float msg_h = caption_height(IntegrationProblemMessage(vm), bar_right - bar_left - 60 * scale);
        const auto retry_text = l10n::Get(failed ? l10n::StringId::IntegrationRetry : l10n::StringId::IntegrationReapply);
        const float retry_w = !vm.settings_integration_can_retry ? 0.0f
            : painter ? painter->MeasureButtonWidth(retry_text) : 96 * scale;
        const float restore_w = !vm.settings_integration_can_restore ? 0.0f
            : painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::IntegrationRestore)) : 200 * scale;
        const float button_left = bar_left + 52 * scale, button_right = bar_right - 12 * scale;
        const bool stack = retry_w > 0 && restore_w > 0 && button_left + retry_w + 8 * scale + restore_w > button_right;
        const float buttons_h = (retry_w > 0 || restore_w > 0) ? (stack ? 72 * scale : 32 * scale) : 0.0f;
        y += 4 * scale;
        l.integration_bar = D2D1::RectF(bar_left, y, bar_right,
                                        y + 32 * scale + msg_h + (buttons_h > 0 ? 10 * scale + buttons_h : 0.0f) + 12 * scale);
        y = l.integration_bar.bottom + 12 * scale;
        const float button_top = l.integration_bar.top + 32 * scale + msg_h + 10 * scale;
        if (retry_w > 0)
            l.integration_retry = D2D1::RectF(button_left, button_top, (std::min)(button_left + retry_w, button_right),
                                              button_top + 32 * scale);
        if (restore_w > 0) {
            const float x = retry_w > 0 && !stack ? l.integration_retry.right + 8 * scale : button_left;
            const float top = retry_w > 0 && stack ? button_top + 40 * scale : button_top;
            l.integration_restore = D2D1::RectF(x, top, (std::min)(x + restore_w, button_right), top + 32 * scale);
        }
    }
    l.integration_list_head = row(32);
    const float item_text_left = left + 112 * scale, item_text_right = right - 16 * scale;
    auto item_row = [&](l10n::StringId desc, float extra) {
        const float h = caption_height(l10n::Get(desc), item_text_right - item_text_left);
        return row((std::max)(52.0f, (31 * scale + h + extra + 12 * scale) / scale));
    };
    l.startup_row[2] = item_row(l10n::StringId::IntegrationFoldersDesc, 0);
    l.this_pc_row = item_row(l10n::StringId::SettingsThisPcDesc, 0);
    l.win_e_row = item_row(l10n::StringId::SettingsWinEDesc, 0);
    {
        float x = item_text_left, chip_y = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const float w = (std::min)(item_text_right - item_text_left,
                painter ? painter->MeasureBadgeWidth(l10n::Get(kIntegrationChips[i])) : 120 * scale);
            if (x > item_text_left && x + w > item_text_right) { x = item_text_left; chip_y += 28 * scale; }
            l.integration_chip[i] = D2D1::RectF(x, chip_y, x + w, chip_y + 22 * scale);
            x += w + 6 * scale;
        }
        const float desc_h = caption_height(l10n::Get(l10n::StringId::IntegrationExperimentalDesc),
                                            item_text_right - item_text_left);
        l.explorer_windows_row = item_row(l10n::StringId::IntegrationExperimentalDesc, 8 * scale + chip_y + 22 * scale);
        const float chip_top = l.explorer_windows_row.top + 31 * scale + desc_h + 8 * scale;
        for (auto& chip : l.integration_chip) { chip.top += chip_top; chip.bottom += chip_top; }
    }
    const auto hint = IntegrationHint(vm);
    if (!hint.empty())
        l.integration_hint = row((4 * scale + caption_height(hint, item_text_right - (left + 78 * scale)) + 14 * scale) / scale);
    else
        y += 6 * scale;
    l.shell_tags_row = row(64);
    l.integration_card = D2D1::RectF(left, integration_top, right, y);
    y += 12 * scale; l.footer = row(28); y += 16 * scale;
    return y;
}

inline float LayoutSettingsFileOps(SettingsLayout& l, const WindowViewModel& vm, float scale,
                                   float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20 * scale, right = l.content.right - 20 * scale;
    (void)vm;
    const bool narrow = right - left < 560 * scale;
    auto row = [&](float h) { auto r = D2D1::RectF(left, y, right, y + h * scale); y = r.bottom; return r; };
    auto choice = [&](D2D1_RECT_F r, float width) {
        return D2D1::RectF(narrow ? r.left + 16 * scale : r.right - (width + 16) * scale,
                           r.bottom - 44 * scale, r.right - 16 * scale, r.bottom - 12 * scale);
    };
    auto segments = [&](D2D1_RECT_F r, D2D1_RECT_F* output, int count, float width) {
        auto c = choice(r, width);
        const float w = (c.right - c.left) / count;
        for (int i = 0; i < count; ++i) output[i] = D2D1::RectF(c.left + i * w, c.top, c.left + (i + 1) * w, c.bottom);
    };
    y += 24 * scale; l.section[0] = row(28);
    l.confirm_delete_row = row(64);

    y += 24 * scale; l.section[1] = row(28);
    l.staging_tray_row = row(64);

    y += 24 * scale; l.section[2] = row(28);
    l.change_tracking_row = row(64);
    l.change_days_row = row(narrow ? 98.0f : 64.0f);
    segments(l.change_days_row, l.change_days, 3, 282);

    y += 24 * scale; l.section[3] = row(28);
    l.hints_row = row(64);
    l.hints_reset_row = row(64);
    {
        const float rw = painter ? painter->MeasureButtonWidth(l10n::Get(l10n::StringId::HintsResetButton)) : 80 * scale;
        l.hints_reset_button = D2D1::RectF(right - 16 * scale - rw, l.hints_reset_row.top + 16 * scale,
                                           right - 16 * scale, l.hints_reset_row.top + 48 * scale);
    }

    y += 24 * scale; l.section[4] = row(28);
    l.collision_row = row(narrow ? 98.0f : 64.0f);
    segments(l.collision_row, l.collision_choice, 3, 282);
    y += 12 * scale; l.footer = row(28); y += 16 * scale;
    return y;
}

inline float LayoutSettingsShortcuts(SettingsLayout& l, const WindowViewModel& vm, float scale,
                                     float y, const fluent::Painter* painter) {
    (void)painter;
    const float left = l.content.left + 20 * scale, right = l.content.right - 20 * scale;
    y += 44 * scale; // page title and intro sit above the cards
    l.shortcut_cards.clear();
    l.shortcut_titles.clear();
    l.shortcut_rows.clear();
    if (!vm.settings_shortcuts) return y;
    // the app's grid: groups are cards, two columns when there is room, each
    // column filled independently so the cards do not interleave.
    const float header = 40 * scale, row_h = 36 * scale, pad = 8 * scale, gap = 14 * scale;
    const bool two_columns = right - left >= 720 * scale;
    const float card_w = two_columns ? (right - left - gap) * 0.5f : right - left;
    const float col_x[2] = {left, left + card_w + gap};
    float col_y[2] = {y, y};
    for (const auto& group : *vm.settings_shortcuts) {
        const float height = header + static_cast<float>(group.rows.size()) * row_h + pad;
        const int col = two_columns ? (col_y[0] <= col_y[1] ? 0 : 1) : 0;
        const float top = col_y[col];
        const D2D1_RECT_F card = D2D1::RectF(col_x[col], top, col_x[col] + card_w, top + height);
        l.shortcut_cards.push_back(card);
        l.shortcut_titles.push_back(
            D2D1::RectF(card.left + 14 * scale, card.top + 9 * scale, card.right - 14 * scale,
                        card.top + 31 * scale));
        float row_y = card.top + header;
        for (size_t i = 0; i < group.rows.size(); ++i) {
            l.shortcut_rows.push_back(
                D2D1::RectF(card.left, row_y, card.right, row_y + row_h));
            row_y += row_h;
        }
        col_y[col] = card.bottom + gap;
    }
    y = (std::max)(col_y[0], col_y[1]) - gap + 10 * scale;
    l.shortcut_reset = D2D1::RectF(right - 118 * scale, y, right, y + 32 * scale);
    y += 44 * scale;
    l.footer = D2D1::RectF(left, y, right, y + 24 * scale);
    return l.footer.bottom + 20 * scale;
}

// 增强包 (page 14): only the downloadable packs, each with its install
// (or remove) button and update button. The usage switches live on 快速预览.
inline float LayoutSettingsEnhance(SettingsLayout& l, const WindowViewModel& vm, float scale,
                                   float y, const fluent::Painter* painter) {
    const float left = l.content.left + 20 * scale, right = l.content.right - 20 * scale;
    auto caption_height = [&](std::wstring_view text, float width) {
        if (text.empty()) return 0.0f;
        const float h = painter ? painter->MeasureWrappedCaptionHeight(text, (std::max)(40.0f * scale, width)) : 20 * scale;
        return h > 0.0f ? h : 20 * scale;
    };
    auto button_w = [&](std::wstring_view text) {
        return painter ? painter->MeasureButtonWidth(text) : 96 * scale;
    };
    auto section = [&](D2D1_RECT_F& out) {
        y += 22 * scale;
        out = D2D1::RectF(left, y, right, y + 26 * scale);
        y = out.bottom + 8 * scale;
    };
    auto pack_card = [&](D2D1_RECT_F& card, D2D1_RECT_F& badge_rc, D2D1_RECT_F& primary,
                         D2D1_RECT_F& update, std::wstring_view desc, std::wstring_view primary_label,
                         std::wstring_view update_label, const pack_text::Badge& badge) {
        const float inner = 16 * scale;
        const float buttons = button_w(primary_label) + (update_label.empty() ? 0.0f : button_w(update_label) + 8 * scale);
        const float text_w = (std::max)(120 * scale, right - left - inner * 2 - buttons - 16 * scale);
        const float desc_h = caption_height(desc, text_w);
        const float h = (std::max)(96.0f * scale, 42 * scale + desc_h + 40 * scale);
        card = D2D1::RectF(left, y, right, y + h);
        const float bw = painter ? painter->MeasureBadgeWidth(badge.text) : 64 * scale;
        badge_rc = D2D1::RectF(card.right - inner - bw, card.top + 16 * scale, card.right - inner,
                            card.top + 38 * scale);
        const float bh = 32 * scale;
        const float py = card.bottom - inner - bh;
        const float pw = button_w(primary_label);
        primary = D2D1::RectF(card.right - inner - pw, py, card.right - inner, py + bh);
        if (!update_label.empty()) {
            const float uw = button_w(update_label);
            update = D2D1::RectF(primary.left - 8 * scale - uw, py, primary.left - 8 * scale, py + bh);
        }
        y = card.bottom + 12 * scale;
    };
    // The bundled AI pack (Luling.AI.exe + this broker) heads the page.
    y += 22 * scale;
    l.ai_section = D2D1::RectF(left, y, right, y + 26 * scale);
    y = l.ai_section.bottom + 8 * scale;
    {
        const float inner = 16 * scale;
        const std::wstring desc = l10n::Pick(
            L"Luling.AI.exe · MCP 宿主：让 Claude 等外部 AI 应用查找、整理你的文件；权限、确认和活动记录在「AI 与 MCP」里设置。",
            L"Luling.AI.exe · MCP host: lets Claude and other AI apps find and organise your files; permissions, confirmations and the activity log live in AI & MCP.");
        const float desc_h = caption_height(desc, right - left - inner * 2);
        const float h = (std::max)(96.0f * scale, 42 * scale + desc_h + 40 * scale);
        l.ai_card = D2D1::RectF(left, y, right, y + h);
        const std::wstring badge = vm.settings_ai_host
            ? l10n::Pick(L"已安装", L"Installed") : l10n::Pick(L"未安装", L"Not installed");
        const float bw = painter ? painter->MeasureBadgeWidth(badge) : 64 * scale;
        l.ai_badge = D2D1::RectF(l.ai_card.right - inner - bw, l.ai_card.top + 16 * scale,
                                 l.ai_card.right - inner, l.ai_card.top + 38 * scale);
        y = l.ai_card.bottom + 12 * scale;
    }
    section(l.pack_media_section);
    pack_card(l.pack_card, l.pack_badge, l.pack_primary, l.pack_update, pack_text::MediaDesc(),
              pack_text::Primary(vm), pack_text::MediaUpdate(vm), pack_text::MediaBadge(vm));
    section(l.pack_images_section);
    pack_card(l.pack_images_card, l.pack_images_badge, l.pack_images_primary, l.pack_images_update,
              pack_text::ImagesDesc(), pack_text::ImagesPrimary(vm), pack_text::ImagesUpdate(vm),
              pack_text::ImagesBadge(vm));
    pack_card(l.pack_raw_card, l.pack_raw_badge, l.pack_raw_primary, l.pack_raw_update,
              pack_text::RawDesc(), pack_text::RawPrimary(vm), pack_text::RawUpdate(vm),
              pack_text::RawBadge(vm));
    section(l.pack_archive_section);
    pack_card(l.pack_archive_card, l.pack_archive_badge, l.pack_archive_primary, l.pack_archive_update,
              pack_text::ArchiveDesc(), pack_text::ArchivePrimary(vm), pack_text::ArchiveUpdate(vm),
              pack_text::ArchiveBadge(vm));
    section(l.pack_transcode_section);
    pack_card(l.pack_transcode_card, l.pack_transcode_badge, l.pack_transcode_primary,
              l.pack_transcode_update, pack_text::TranscodeDesc(), pack_text::TranscodePrimary(vm),
              pack_text::TranscodeUpdate(vm), pack_text::TranscodeBadge(vm));

    // 安装全部: one pass over every pack that is missing.
    y += 24 * scale;
    {
        const float install_all_w = button_w(l10n::Get(l10n::StringId::PackInstallAll));
        l.pack_install_all = D2D1::RectF(right - install_all_w, y, right, y + 32 * scale);
        y = l.pack_install_all.bottom + 24 * scale;
    }

    // 媒体来源: the installed FFmpeg pack, or one the user already has.
    y += 22 * scale;
    l.pack_section = D2D1::RectF(left, y, right, y + 26 * scale);
    y = l.pack_section.bottom + 8 * scale;
    {
        const float inner = 16 * scale;
        const bool source_switch = pack_text::ShowsEnable(vm);
        const float source_text_width = right - left - (source_switch ? 88 : 32) * scale;
        const float status_h = caption_height(pack_text::SourceStatus(vm), source_text_width) + 47 * scale;
        l.pack_source_status = D2D1::RectF(left, y, right, y + status_h);
        if (source_switch) {
            const float cy = (l.pack_source_status.top + l.pack_source_status.bottom) * 0.5f;
            l.pack_enable = D2D1::RectF(right - inner - 42 * scale, cy - 16 * scale,
                                        right - inner, cy + 16 * scale);
        }
        y = l.pack_source_status.bottom;
        const float custom_h = caption_height(pack_text::CustomDesc(), right - left - 126 * scale) + 47 * scale;
        l.pack_custom_row = D2D1::RectF(left, y, right, y + custom_h);
        y = l.pack_custom_row.bottom;
        if (pack_text::ShowsPath(vm)) {
            const float path_h = pack_text::ShowsDetect(vm) ? 110 * scale : 90 * scale;
            l.pack_path_row = D2D1::RectF(left, y, right, y + path_h);
            const float browse_w = button_w(pack_text::Browse());
            const float button_top = l.pack_path_row.bottom - 44 * scale;
            l.pack_browse = D2D1::RectF(right - inner - browse_w, button_top,
                                        right - inner, button_top + 32 * scale);
            if (pack_text::ShowsDetect(vm)) {
                const float detect_w = button_w(pack_text::UseDetected());
                l.pack_detect = D2D1::RectF(l.pack_browse.left - 8 * scale - detect_w, button_top,
                                            l.pack_browse.left - 8 * scale, button_top + 32 * scale);
            }
            y = l.pack_path_row.bottom;
        }
        l.pack_source_group = D2D1::RectF(left, l.pack_section.bottom + 8 * scale, right, y);
    }

    // 高级: background updates and the uninstall choice.
    y += 22 * scale;
    l.pack_advanced_section = D2D1::RectF(left, y, right, y + 26 * scale);
    y = l.pack_advanced_section.bottom + 8 * scale;
    {
        const float group_top = y;
        const float auto_h = caption_height(pack_text::AutoUpdateDesc(), right - left - 126 * scale) + 47 * scale;
        l.pack_auto_update_row = D2D1::RectF(left, y, right, y + auto_h);
        y = l.pack_auto_update_row.bottom;
        const float remove_h = caption_height(pack_text::RemoveDesc(), right - left - 126 * scale) + 47 * scale;
        l.pack_remove_row = D2D1::RectF(left, y, right, y + remove_h);
        y = l.pack_remove_row.bottom;
        l.pack_group = D2D1::RectF(left, group_top, right, y);
    }
    y += 12 * scale;
    l.pack_note = D2D1::RectF(left, y, right,
                              y + caption_height(pack_text::Note(), right - left - 8 * scale) + 4 * scale);
    y = l.pack_note.bottom + 24 * scale;

    l.footer = D2D1::RectF(left, y, right, y + 24 * scale);
    return y + 40 * scale;
}

inline float LayoutSettingsPages(SettingsLayout& l, const WindowViewModel& vm, float scale, float y,
                                 const fluent::Painter* painter) {
    switch (vm.settings_page) {
    case 0: return LayoutSettingsAppearance(l, vm, scale, y, painter);
    case 7: return LayoutSettingsFileList(l, vm, scale, y, painter);
    case 8: return LayoutSettingsTabsWindows(l, vm, scale, y, painter);
    case 9: return LayoutSettingsStartupIntegration(l, vm, scale, y, painter);
    case 10: return LayoutSettingsFileOps(l, vm, scale, y, painter);
    case 13: return LayoutSettingsShortcuts(l, vm, scale, y, painter);
    case 14: return LayoutSettingsEnhance(l, vm, scale, y, painter);
    default: return y;
    }
}
