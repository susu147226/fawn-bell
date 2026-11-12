// app_prefs.h — General app settings (startup, close-to-tray).
#pragma once
#include "folder_view_prefs.h"
#include "folder_sort_prefs.h"
#include "entry_group.h"
#include "../ui/details_column_set.h"
#include <cstdint>
#include <string>
#include <vector>

namespace luling::app {

struct AppPrefs {
    bool persist = true;
    // 自定义工具栏: the New button's main part makes 0 a folder, 1 the type
    // made last (toolbar_last_new: 0 folder, 1 text document), 2 nothing: the
    // whole button opens the New menu. toolbar_pins is a bitmask of builtin
    // commands pinned after the file commands.
    int toolbar_new_primary = 0;
    int toolbar_last_new = 0;
    uint32_t toolbar_pins = 0;
    // Programs pinned to the toolbar (自定义工具栏 → 固定的软件).
    std::vector<std::wstring> toolbar_apps;
    // Quick look trigger (Settings > 快速预览): 0 a card appears after the
    // pointer rests on a file (hover_preview_delay_ms), Space still opens the
    // full preview; 1 only Space previews.
    int preview_trigger = 0;
    int hover_preview_delay_ms = 500; // 300 / 500 / 800
    // The card's size step: 0 small, 1 medium, 2 large. Ctrl enlarges it.
    int hover_card_size = 1;
    bool launch_on_startup = false;
    bool keep_running_on_close = false;
    // The first close that leaves Luling in the tray explains how to come back
    // and how to exit; later closes stay quiet.
    bool tray_hint_shown = false;
    // notify_icon_mode: the notification-area icon while closing keeps Luling
    // running: 0 always, 1 only while the window is closed to it, 2 never
    // (a second launch brings the window back; #57).
    int notify_icon_mode = 0;
    bool open_folders_in_luling = false;
    bool take_over_win_e = false;
    bool take_over_this_pc = false; // HKCU This PC open verb (default_file_manager.h)
    bool integration_enabled = false;
    bool integration_folders = true;
    bool integration_win_e = true;
    bool integration_this_pc = true;
    bool integration_configured = false; // runtime: new-format preferences were loaded
    bool integration_residual = false; // runtime: some association still belongs to Luling
    bool integration_incomplete = false;
    bool take_over_explorer_windows = false; // experimental choice; runtime is gated by integration_enabled
    // True once the user toggled the row above. Existing installs that never
    // touched it adopt the takeover together with folder integration.
    bool take_over_explorer_windows_set = false;
    bool shell_tag_menu = false;    // File Explorer "Luling tags" submenu (shell_tag_menu.cpp syncs HKCU)   // registry is the source of truth (not in app.json)
    bool verify_copies = false;
    bool show_status_performance = false;
    // Title-bar clock card (time, date, lunar date, festivals, schedules).
    bool show_title_clock = true;
    bool show_pinned_tab_names = true;
    // Details list presentation.
    bool list_smart_date = true;
    bool list_zebra_rows = true;
    bool list_size_bar = false;
    bool list_tag_name_color = false; // tint tagged names with their first tag's color
    bool list_selection_outline = false; // accent outline around selected items (#78)
    // Grid thumbnails: default-program badge, plus playing time on videos.
    bool list_thumbnail_badges = true;
    bool vertical_tabs = false;       // tabs as the first sidebar section
    bool sidebar_collapsed = false;   // sidebar folded to its icon rail (Ctrl+B)
    // 0 folders first, 1 follow the sort direction, 2 mixed with files
    int folder_sort_mode = 0;
    // Optional Details columns for every folder (ui/details_column_set.h bits).
    uint32_t details_columns = ui::kDetailsColumnsDefault;
    // Where Luling opens. home_folder is the default location; empty means This PC.
    // startup_open: 0 restore the last tabs, 1 open the default location.
    // new_tab_open: 0 the current folder, 1 the default location.
    int startup_open = 0;
    int new_tab_open = 0;
    // Closing the only tab closes the window (app/last_tab_close.h).
    bool close_window_with_last_tab = false;
    // Single-window mode: every open-in-window request becomes a tab here.
    bool single_window = false;
    bool confirm_recycle_delete = false; // ask before Delete moves items to the Recycle Bin
    // Sign-in launches (Run value with --startup) stay hidden behind the tray icon.
    bool start_in_tray = false;
    std::wstring home_folder;
    // Text rendering: 0 auto (LumaText), 1 sharp (pixel-snapped DirectWrite), 2 smooth
    int text_render = 0;
    // Interface font size in percent of the built-in sizes: 90 / 100 / 112 / 125.
    int ui_font_scale = 100;
    FolderViewPrefs folder_views;
    FolderSortPrefs folder_sorts;
    FolderGroupPrefs folder_groups;
    bool search_pinyin = true;
    bool global_search_enabled = false;
    uint32_t global_search_modifiers = 1; // MOD_ALT
    uint32_t global_search_key = 32; // VK_SPACE
    bool show_hidden_files = false;
    // 收藏栏: the starred list as a row under the toolbar instead of the
    // sidebar's 收藏夹 section (Ctrl+Shift+B).
    bool favorites_bar = false;
    // Hidden + system attributes; File Explorer keeps these behind a separate option.
    bool show_protected_os_files = false;
    // Double click on empty list space: 0 nothing, 1 back, 2 up (blank_pane_click.h).
    int blank_click_action = 0;
    // Copy / move into a folder that already has the name: 0 ask, 1 keep both
    // (the new item gets a copy suffix), 2 replace the existing one.
    int collision_policy = 0;
    // A single click opens the item (a drag still selects); off by default.
    bool single_click_open = false;
    // A checkbox leads every row so multi-selection does not need Ctrl.
    bool list_checkboxes = false;
    bool show_hidden_shares = false;   // \\server lists $ admin shares
    // A folder Luling cannot open (gone, not a folder, no access) opens in
    // Windows Explorer instead of an empty tab.
    bool explorer_fallback = true;
    // 修改时间提示: mark items whose mtime is recent. Style: 0 off, 1 dot,
    // 2 time bar, 3 text only. Days: 1 / 3 / 7 / 30. Palette: 0 accent,
    // 1 cool-warm, 2 teal, 3 amber, 4 aurora, 5 graphite.
    int mtime_style = 0;
    int mtime_days = 7;
    int mtime_palette = 0;
    bool mtime_dim_old = true;
    bool mtime_bold_today = true;
    bool mtime_in_icons = true;
    bool mtime_folders = false;
    bool mtime_flash = true;
    // OneDrive Files On-Demand state shown after the name.
    bool cloud_status = true;
    // A pinned parent-folder row at the top of a scrolled list; clicking it
    // goes up one level.
    bool sticky_parent = true;
    // Inline file tree in the details list: folders expand to show children.
    bool tree_view = true;
    // SVN working-copy status after the name (needs svn.exe).
    bool svn_status = false;
    bool change_tracking_enabled = false;
    int change_tracking_days = 7;
    // system / zh-CN / en-US
    int theme_mode = -1; // legacy session theme, or 0 system / 1 light / 2 dark
    std::wstring language = L"system";
    // none / acrylic-material / mica / mica-alt  (legacy dwm-blur → acrylic)
    std::wstring window_effect = L"mica-alt";
    std::wstring background_image;
    // Interface transparency 0..90 (json panel_transparency). 25/50/75 match the
    // former subtle/balanced/vivid levels; applies to image, Acrylic and Mica.
    int wallpaper_look = 50;
    int wallpaper_blur = 14; // wallpaper blur in DIPs 0..40 (json wallpaper_blur_px)
    int row_height = 34; // file-list row height in DIPs (24..48)
    int sidebar_width = 224; // DIPs
    bool address_search_current = false;
    bool address_search_content = false;
    int tray_icon_size = 48; // staging-tray deck icon edge in DIPs (32..64)
    // Staging tray (spec A1, issue #126): off by default, and adding to it is
    // always an explicit action — copy/cut only touch the clipboard.
    bool staging_tray = false;
    // Interaction hints: status-bar context hints + one-time teaching tips.
    bool show_hints = true;
    // Background update checks and their "update available" toasts (Settings > About).
    bool auto_check_updates = true;
    uint32_t tips_seen = 0; // bit per app::TeachTip already shown or dismissed
    // Empty = theme default, or Windows when explicitly selected.
    std::wstring accent_rgb;
    bool accent_follow_system = false;
    // The custom accent as HSL (the Appearance sliders); kept alongside
    // accent_rgb so dragging never round-trips through RGB quantization.
    int accent_h = 206;
    int accent_s = 100;
    int accent_l = 38;
    // Tag colors the user added via the custom color dialog (0xRRGGBB),
    // appended after the seven Finder defaults in the swatch strip.
    std::vector<uint32_t> custom_tag_colors;
    // 快捷键 overrides (shortcut_map.h): packed command << 16 | vk << 8 | mods.
    std::vector<uint32_t> shortcut_bindings;
    int duplicate_scan_scope = 0; // 0 folder, 1 drive, 2 all local disks
    std::wstring duplicate_scan_folder;
    std::wstring duplicate_scan_drive;
    // Version that last ran with these prefs; drives the one-time "updated" toast.
    std::wstring last_seen_version;
    std::wstring tray_dests; // staging tray: recent drop folders, newest first, '|'-joined (max 3)
    bool had_file = false; // runtime only: app.json existed when Load() ran
    bool load_failed = false; // blocks saving defaults over an unreadable/damaged existing file

    void ResetToDefaults();
    bool Load();
    bool Save() const;
    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);
    void MigrateIntegration();
    // Re-applies a takeover the user left enabled but whose registry values an
    // outside cleanup removed. Never takes over a verb that now belongs to
    // another program.
    void RepairDriftedShellIntegration();
    bool ReadIntegrationResidual() const;
    bool ReadIntegrationIncomplete() const;

    // HKCU Run key is the source of truth; call after Load() and on toggle.
    bool ReadLaunchOnStartup() const;
    bool ApplyLaunchOnStartup(bool on);

    // HKCU Directory/Drive open verbs; call after Load() and on toggle.
    bool ReadFolderOpen() const;
    bool ApplyFolderOpen(bool on);

    // HKCU "File Explorer" launch verb used by Win+E and the taskbar Explorer
    // pin ({52205fd8-...}\shell\opennewwindow). Original values are kept by
    // shell_integration_registry and restored only while Luling still owns them.
    bool ReadWinE() const;
    bool ApplyWinE(bool on);

    bool StoreBackgroundImage(const std::wstring& source_path);
    void ClearBackgroundImage();
};

std::wstring FolderOpenCommandLine(const std::wstring& exe);
bool FolderOpenCommandIsOurs(const std::wstring& command, const std::wstring& exe);
bool ParseAccentRgb(const std::wstring& text, uint32_t& rgb) noexcept;
// Menu row height for a file-list row height: 2 DIPs taller, kept within
// 28..40, so 紧凑/标准/宽松 (28/34/40) give 30/36/40 (#27).
int MenuRowHeightDip(int list_row_height) noexcept;
// Interface font size: only 90/100/112/125 are kept, anything else is 100.
int NormalizeUiFontScale(int percent) noexcept;
// List row height actually used: the chosen density, raised so larger
// interface fonts still fit (112% -> at least 32, 125% -> at least 35).
int EffectiveRowHeightDip(int list_row_height, int ui_font_scale) noexcept;

constexpr int kPreviewTriggerHover = 0;
constexpr int kPreviewTriggerSpace = 1;
inline int NormalizePreviewTrigger(int value) noexcept {
    return value == kPreviewTriggerSpace ? kPreviewTriggerSpace : kPreviewTriggerHover;
}
inline int NormalizeHoverPreviewDelay(int ms) noexcept {
    return ms == 300 || ms == 800 ? ms : 500;
}
constexpr int kHoverCardSmall = 0;
constexpr int kHoverCardMedium = 1;
constexpr int kHoverCardLarge = 2;
inline int NormalizeHoverCardSize(int value) noexcept {
    return value == kHoverCardSmall || value == kHoverCardLarge ? value : kHoverCardMedium;
}

} // namespace luling::app
