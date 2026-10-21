#include "global_search_controller.h"
#include "global_search_handoff.h"
#include "app_internal.h"
#include "../common/localization.h"
#include "../ui/quick_preview_command.h"

namespace luling {
namespace {
// Test instances leave the hotkey to the installed Luling. Selftest builds can
// opt in (LULING_TEST_GLOBAL_HOTKEY) to drive the popup with the hotkey stored
// in their own isolated data dir.
bool HotkeyAllowed(const AppState& s) {
    if (!s.isolatedTest) return true;
#ifdef LULING_WITH_SELFTEST
    return GetEnvironmentVariableW(L"LULING_TEST_GLOBAL_HOTKEY", nullptr, 0) > 0;
#else
    return false;
#endif
}
}

void ApplyGlobalSearchSettings(AppState& s) {
    if (!HotkeyAllowed(s)) return;
    const bool enabled = s.appPrefs.global_search_enabled;
    const bool ok = s.globalSearchHotkey.Update(ServiceWindow(s), enabled,
        s.appPrefs.global_search_modifiers, s.appPrefs.global_search_key);
    s.settings.SetGlobalSearchError(ok ? std::wstring{} : l10n::Get(
        s.globalSearchHotkey.Error() == ERROR_HOTKEY_ALREADY_REGISTERED
            ? l10n::StringId::GlobalSearchConflict : l10n::StringId::GlobalSearchRegisterFailed));
    if (!enabled || !ok) s.globalSearchWindow.Hide();
    s.tray_controller.SetVisible(app::TrayIconWanted(s.appPrefs.keep_running_on_close || enabled,
                                                     s.appPrefs.notify_icon_mode, s.hidden_to_tray));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool HandleGlobalSearchHotkeyCapture(AppState& s, unsigned key) {
    if (!s.settings.global_search_hotkey_capturing()) return false;
    if (!IsSettingsTab(ActiveTab(s))) {
        s.settings.CancelGlobalSearchHotkeyCapture();
        return false;
    }
    UINT modifiers = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= MOD_WIN;
    const bool consumed = s.settings.CaptureGlobalSearchHotkey(key, modifiers);
    if (consumed) InvalidateRect(s.hwnd, nullptr, FALSE);
    return consumed;
}

// The popup hands over a result row rather than an item in the active list, so
// the preview is built from the row itself. Space toggles it the same way it
// does in the file list (§8.8.2).
void ShowGlobalSearchPreview(AppState& s, std::wstring path, std::wstring name) {
    if (s.quickPreview.visible()) {
        s.quickPreview.Close();
        return;
    }
    if (path.empty()) return;
    // The search popup is WS_EX_TOPMOST, so a preview opened underneath it is
    // invisible. Give the screen to the preview; the popup keeps its query, so
    // Alt+Space brings the same results straight back.
    s.globalSearchWindow.Hide();
    ui::QuickPreviewItem item;
    item.path = std::move(path);
    item.name = std::move(name);
    const auto effect = s.appPrefs.background_image.empty()
        ? ui::WindowEffectFromId(s.appPrefs.window_effect)
        : ui::WindowEffect::None;
    s.quickPreview.Show(item, s.darkMode, effect, s.safeMode, nullptr);
    s.quickPreviewSelection.Reset(item.path);
}

void ToggleGlobalSearch(AppState& s) {
    if (!s.appPrefs.global_search_enabled || !s.globalSearchHotkey.Registered()) return;
    if (s.globalSearchWindow.Visible()) {
        s.globalSearchWindow.Hide();
        return;
    }
    std::wstring folder;
    s.globalSearchWindow.SetAppearance(s.darkMode, ui::WindowEffectFromId(s.appPrefs.window_effect),
        s.appPrefs.background_image, s.accentColor);
    if (const auto* tab = ActiveTab(s); tab && !fs::IsVirtualPath(tab->current_path)) folder = tab->current_path;
    s.globalSearchWindow.SetHandoffHandler([&s](const GlobalSearchHandoff& request) {
        ContinueGlobalSearchInMain(s, request);
    });
    s.globalSearchWindow.SetPreviewHandler([&s](std::wstring path, std::wstring name) {
        ShowGlobalSearchPreview(s, std::move(path), std::move(name));
    });
    if (!s.globalSearchWindow.Show(s.hwnd, s.darkMode, s.scale, folder, s.appPrefs.search_pinyin)) {
        s.tray_controller.RestoreWindow();
        s.notification_toast.ShowError(s.hwnd, l10n::Get(l10n::StringId::GlobalSearch),
            l10n::Get(l10n::StringId::GlobalSearchOpenFailed));
    }
}

void ShutdownGlobalSearch(AppState& s) {
    s.globalSearchHotkey.Reset();
    s.globalSearchWindow.Shutdown();
}
}
