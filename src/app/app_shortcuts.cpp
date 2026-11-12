#include "app_shortcuts.h"

#include "shortcut_map.h"
#include "context_menu.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "app_ops_ui.h"
#include "app_hosted_edit.h"
#include "app_hover_preview.h"
#include "app_messages.h"
#include "ai_assistant.h"
#include "vertical_tabs.h"
#include "favorites_bar.h"
#include "window_merge.h"
#include "../common/localization.h"

namespace luling::app {
uint8_t ModifierBits() noexcept {
    uint8_t mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= kShortcutCtrl;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= kShortcutShift;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= kShortcutAlt;
    return mods;
}

bool RunShortcutCommand(AppState& s, int command) {
    using C = ShortcutCommand;
    app::Tab* tab = ActiveTab(s);
    switch (static_cast<C>(command)) {
    case C::Copy: MirrorSelectionToClipboard(s, false); return true;
    case C::Cut: MirrorSelectionToClipboard(s, true); return true;
    case C::Paste: PasteIntoCurrent(s); return true;
    case C::Undo:
        if (s.ops.CanUndo()) s.ops.Undo();
        else InvalidateRect(s.hwnd, nullptr, FALSE);
        return true;
    case C::Rename: ShowRenameOverlay(s); return true;
    case C::Delete: DeleteSelected(s, false); return true;
    case C::DeletePermanent: DeleteSelected(s, true); return true;
    case C::Refresh: DispatchMenuCommand(s, CmdRefresh); return true;
    case C::NewFolder: CreateNewItem(s, true); return true;
    case C::QuickPreview:
        if (!OpenHoverPreviewInQuickLook(s)) ToggleQuickPreview(s);
        return true;
    case C::Open: OpenSelected(s); return true;
    case C::Back: GoBack(s); return true;
    case C::Forward: GoForward(s); return true;
    case C::Up: GoUp(s); return true;
    case C::Properties: DispatchMenuCommand(s, CmdProperties); return true;
    case C::Filter:
        if (tab && IsAddressSearchResults(tab)) ShowAddressSearch(s);
        else ShowFilterEditor(s);
        return true;
    case C::AddressBar: ShowOmnibar(s, OmnibarMode::Path); return true;
    case C::SelectAll: DispatchMenuCommand(s, CmdSelectAll); return true;
    case C::NewTab: OpenNewTab(s); return true;
    case C::CloseTab: CloseActiveTab(s); return true;
    case C::NewWindow: PostMessageW(s.hwnd, WM_NEW_WINDOW, 0, 0); return true;
    case C::MergeWindows: MergeWindowsInto(s); return true;
    case C::Sidebar: ToggleSidebarCollapsed(s); return true;
    case C::Search: ShowAddressSearch(s); return true;
    case C::CommandBar: ShowOmnibar(s, OmnibarMode::Mixed); return true;
    case C::Project: ShowOmnibar(s, OmnibarMode::Project); return true;
    case C::AdvancedSearch: ShowAdvancedSearch(s); return true;
    case C::SelectPattern: DispatchMenuCommand(s, CmdSelectWildcard); return true;
    case C::InvertSelection: DispatchMenuCommand(s, CmdInvertSelection); return true;
    case C::BatchRename: ShowBatchRename(s); return true;
    case C::CopyPath: CopySelectedPath(s); return true;
    case C::AiAssistant: AiAssistantHotkey(s); return true;
    case C::LayoutSingle: ApplyLayoutPreset(s, LayoutPreset::Single); return true;
    case C::LayoutTwoVertical: ApplyLayoutPreset(s, LayoutPreset::TwoVertical); return true;
    case C::LayoutThree: ApplyLayoutPreset(s, LayoutPreset::Three); return true;
    case C::LayoutFour: ApplyLayoutPreset(s, LayoutPreset::FourGrid); return true;
    case C::CustomizeToolbar: ShowToolbarCustomize(s); return true;
    case C::FavoritesBar: ToggleFavoritesBar(s); return true;
    default: return false;
    }
}

void HandleShortcutCapture(AppState& s, unsigned vk) {
    const auto command = static_cast<ShortcutCommand>(s.shortcutCapture);
    s.shortcutCaptureHint.clear();
    if (vk == VK_ESCAPE) {
        s.shortcutCapture = -1;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    // Backspace/Delete without modifiers restores the stock key.
    const uint8_t mods = ModifierBits();
    if ((vk == VK_BACK || vk == VK_DELETE) && mods == 0) {
        ResetShortcut(s.appPrefs.shortcut_bindings, command);
        if (!s.isolatedTest) s.appPrefs.Save();
        s.shortcutCapture = -1;
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (!IsBindableShortcutKey(vk)) return;
    const ShortcutCommand owner =
        ShortcutOwner(s.appPrefs.shortcut_bindings, vk, mods, command);
    if (owner != ShortcutCommand::None) {
        s.shortcutCaptureHint = l10n::Pick(L"该快捷键已用于「", L"Already used by \"") +
            ShortcutCommandName(owner) + l10n::Pick(L"」", L"\"") +
            l10n::Pick(L"，请按其他组合或按 Esc 取消", L"; press another combination or Esc");
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    SetShortcut(s.appPrefs.shortcut_bindings, command, vk, mods);
    if (!s.isolatedTest) s.appPrefs.Save();
    s.shortcutCapture = -1;
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
}
