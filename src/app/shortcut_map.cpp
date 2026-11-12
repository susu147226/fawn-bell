#include "shortcut_map.h"

#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "../ui/shortcut_help.h"

#include <algorithm>

namespace luling::app {
namespace {
using l10n::StringId;

struct DefaultBinding {
    ShortcutCommand command;
    ShortcutChord chords[2];
    int count;
};

constexpr uint8_t kCtrl = kShortcutCtrl;
constexpr uint8_t kShift = kShortcutShift;
constexpr uint8_t kAlt = kShortcutAlt;

// One source of truth for the rebindable commands and their stock keys.
constexpr DefaultBinding kDefaults[] = {
    {ShortcutCommand::Copy, {{L'C', kCtrl}, {}}, 1},
    {ShortcutCommand::Cut, {{L'X', kCtrl}, {}}, 1},
    {ShortcutCommand::Paste, {{L'V', kCtrl}, {}}, 1},
    {ShortcutCommand::Undo, {{L'Z', kCtrl}, {}}, 1},
    {ShortcutCommand::Rename, {{VK_F2, 0}, {}}, 1},
    {ShortcutCommand::Delete, {{VK_DELETE, 0}, {}}, 1},
    {ShortcutCommand::DeletePermanent, {{VK_DELETE, kShift}, {}}, 1},
    {ShortcutCommand::Refresh, {{VK_F5, 0}, {}}, 1},
    {ShortcutCommand::NewFolder, {{VK_F7, 0}, {}}, 1},
    {ShortcutCommand::QuickPreview, {{VK_SPACE, 0}, {}}, 1},
    {ShortcutCommand::Open, {{VK_RETURN, 0}, {VK_RETURN, kShift}}, 2},
    {ShortcutCommand::Back, {{VK_LEFT, kAlt}, {}}, 1},
    {ShortcutCommand::Forward, {{VK_RIGHT, kAlt}, {}}, 1},
    {ShortcutCommand::Up, {{VK_BACK, 0}, {VK_UP, kAlt}}, 2},
    {ShortcutCommand::Properties, {{VK_RETURN, kAlt}, {}}, 1},
    {ShortcutCommand::Filter, {{L'F', kCtrl}, {}}, 1},
    {ShortcutCommand::AddressBar, {{L'L', kCtrl}, {}}, 1},
    {ShortcutCommand::SelectAll, {{L'A', kCtrl}, {}}, 1},
    {ShortcutCommand::NewTab, {{L'T', kCtrl}, {}}, 1},
    {ShortcutCommand::CloseTab, {{L'W', kCtrl}, {}}, 1},
    {ShortcutCommand::NewWindow, {{L'N', kCtrl}, {}}, 1},
    {ShortcutCommand::MergeWindows, {{L'J', kCtrl | kShift}, {}}, 1},
    {ShortcutCommand::Sidebar, {{L'B', kCtrl}, {}}, 1},
    {ShortcutCommand::Search, {{L'K', kCtrl}, {}}, 1},
    {ShortcutCommand::CommandBar, {{L'K', kCtrl | kShift}, {}}, 1},
    {ShortcutCommand::Project, {{L'P', kCtrl | kShift}, {}}, 1},
    {ShortcutCommand::AdvancedSearch, {{L'F', kCtrl | kShift}, {}}, 1},
    {ShortcutCommand::SelectPattern, {{L'A', kCtrl | kShift}, {}}, 1},
    {ShortcutCommand::InvertSelection, {{L'I', kCtrl}, {}}, 1},
    {ShortcutCommand::BatchRename, {{L'R', kCtrl | kShift}, {}}, 1},
    {ShortcutCommand::CopyPath, {{L'C', kCtrl | kShift}, {}}, 1},
    {ShortcutCommand::AiAssistant, {{L'P', kCtrl}, {}}, 1},
    {ShortcutCommand::LayoutSingle, {{L'1', kCtrl}, {}}, 1},
    {ShortcutCommand::LayoutTwoVertical, {{L'2', kCtrl}, {}}, 1},
    {ShortcutCommand::LayoutThree, {{L'3', kCtrl}, {}}, 1},
    {ShortcutCommand::LayoutFour, {{L'4', kCtrl}, {}}, 1},
    {ShortcutCommand::CustomizeToolbar, {{VK_OEM_2, kCtrl}, {}}, 1},
    {ShortcutCommand::FavoritesBar, {{L'B', kCtrl | kShift}, {}}, 1},
};

const DefaultBinding* FindDefault(ShortcutCommand command) {
    for (const auto& entry : kDefaults)
        if (entry.command == command) return &entry;
    return nullptr;
}

std::wstring KeyName(uint32_t vk) {
    if (vk >= L'A' && vk <= L'Z') return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= L'0' && vk <= L'9') return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
    case VK_LEFT: return L"←";
    case VK_RIGHT: return L"→";
    case VK_UP: return L"↑";
    case VK_DOWN: return L"↓";
    case VK_DELETE: return L"Delete";
    case VK_BACK: return L"Backspace";
    case VK_RETURN: return L"Enter";
    case VK_SPACE: return L"Space";
    case VK_TAB: return L"Tab";
    case VK_INSERT: return L"Insert";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PgUp";
    case VK_NEXT: return L"PgDn";
    case VK_ESCAPE: return L"Esc";
    default: return std::wstring();
    }
}
}

uint32_t PackShortcut(ShortcutCommand command, uint32_t vk, uint8_t mods) noexcept {
    return (static_cast<uint32_t>(command) << 16) | ((vk & 0xFF) << 8) | (mods & 0x7);
}

ShortcutChord UnpackShortcut(uint32_t packed) noexcept {
    return ShortcutChord{ (packed >> 8) & 0xFF, static_cast<uint8_t>(packed & 0x7) };
}

std::vector<ShortcutChord> DefaultShortcuts(ShortcutCommand command) {
    const DefaultBinding* entry = FindDefault(command);
    if (!entry) return {};
    std::vector<ShortcutChord> out;
    for (int i = 0; i < entry->count; ++i) out.push_back(entry->chords[i]);
    return out;
}

std::vector<ShortcutChord> EffectiveShortcuts(const std::vector<uint32_t>& overrides,
                                              ShortcutCommand command) {
    for (const uint32_t packed : overrides) {
        if (static_cast<int>(packed >> 16) == static_cast<int>(command))
            return { UnpackShortcut(packed) };
    }
    return DefaultShortcuts(command);
}

ShortcutCommand FindShortcutCommand(const std::vector<uint32_t>& overrides, uint32_t vk,
                                    uint8_t mods) {
    const ShortcutChord chord{ vk, mods };
    for (int i = 0; i < static_cast<int>(ShortcutCommand::Count); ++i) {
        const auto command = static_cast<ShortcutCommand>(i);
        for (const auto& bound : EffectiveShortcuts(overrides, command))
            if (bound == chord) return command;
    }
    return ShortcutCommand::None;
}

ShortcutCommand ShortcutOwner(const std::vector<uint32_t>& overrides, uint32_t vk,
                              uint8_t mods, ShortcutCommand except) {
    const ShortcutChord chord{ vk, mods };
    for (int i = 0; i < static_cast<int>(ShortcutCommand::Count); ++i) {
        const auto command = static_cast<ShortcutCommand>(i);
        if (command == except) continue;
        for (const auto& bound : EffectiveShortcuts(overrides, command))
            if (bound == chord) return command;
    }
    return ShortcutCommand::None;
}

void SetShortcut(std::vector<uint32_t>& overrides, ShortcutCommand command, uint32_t vk,
                 uint8_t mods) {
    const uint32_t command_bits = static_cast<uint32_t>(command) << 16;
    for (size_t i = 0; i < overrides.size();) {
        if ((overrides[i] & 0xFFFF0000u) == command_bits) overrides.erase(overrides.begin() + i);
        else ++i;
    }
    // A binding equal to the stock one needs no override entry.
    const auto defaults = DefaultShortcuts(command);
    if (defaults.size() == 1 && defaults[0] == ShortcutChord{ vk, mods }) return;
    overrides.push_back(PackShortcut(command, vk, mods));
}

void ResetShortcut(std::vector<uint32_t>& overrides, ShortcutCommand command) {
    const uint32_t command_bits = static_cast<uint32_t>(command) << 16;
    std::erase_if(overrides, [&](uint32_t packed) {
        return (packed & 0xFFFF0000u) == command_bits;
    });
}

void ResetAllShortcuts(std::vector<uint32_t>& overrides) {
    overrides.clear();
}

bool ShortcutOverridden(const std::vector<uint32_t>& overrides, ShortcutCommand command) {
    const uint32_t command_bits = static_cast<uint32_t>(command) << 16;
    return std::any_of(overrides.begin(), overrides.end(), [&](uint32_t packed) {
        return (packed & 0xFFFF0000u) == command_bits;
    });
}

bool IsBindableShortcutKey(uint32_t vk) noexcept {
    if (vk == 0) return false;
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        return false;
    default:
        return true;
    }
}

std::wstring DescribeShortcut(const ShortcutChord& chord) {
    std::wstring text;
    if (chord.mods & kShortcutCtrl) text += L"Ctrl ";
    if (chord.mods & kShortcutShift) text += L"Shift ";
    if (chord.mods & kShortcutAlt) text += L"Alt ";
    text += KeyName(chord.vk);
    return text;
}

std::wstring ShortcutCommandName(ShortcutCommand command) {
    switch (command) {
    case ShortcutCommand::Copy: return l10n::Get(StringId::Copy);
    case ShortcutCommand::Cut: return l10n::Get(StringId::Cut);
    case ShortcutCommand::Paste: return l10n::Get(StringId::Paste);
    case ShortcutCommand::Undo: return l10n::Get(StringId::Undo);
    case ShortcutCommand::Rename: return l10n::Get(StringId::Rename);
    case ShortcutCommand::Delete: return l10n::Get(StringId::Delete);
    case ShortcutCommand::DeletePermanent: return l10n::Get(StringId::PermanentDelete);
    case ShortcutCommand::Refresh: return l10n::Get(StringId::Refresh);
    case ShortcutCommand::NewFolder: return l10n::Get(StringId::NewFolder);
    case ShortcutCommand::QuickPreview: return l10n::Get(StringId::QuickPreview);
    case ShortcutCommand::Open: return l10n::Get(StringId::HelpOpen);
    case ShortcutCommand::Back: return l10n::Get(StringId::Back);
    case ShortcutCommand::Forward: return l10n::Get(StringId::Forward);
    case ShortcutCommand::Up: return l10n::Get(StringId::Up);
    case ShortcutCommand::Properties: return l10n::Get(StringId::Properties);
    case ShortcutCommand::Filter: return l10n::Get(StringId::HelpFilter);
    case ShortcutCommand::AddressBar: return l10n::Get(StringId::HelpGoToPath);
    case ShortcutCommand::SelectAll: return l10n::Get(StringId::SelectAll);
    case ShortcutCommand::NewTab: return l10n::Get(StringId::TooltipNewTab);
    case ShortcutCommand::CloseTab: return l10n::Get(StringId::TabClose);
    case ShortcutCommand::NewWindow: return l10n::Get(StringId::NewWindow);
    case ShortcutCommand::MergeWindows: return l10n::Get(StringId::MergeWindows);
    case ShortcutCommand::Sidebar: return l10n::Get(StringId::HelpSidebar);
    case ShortcutCommand::Search: return l10n::Get(StringId::Search);
    case ShortcutCommand::CommandBar: return l10n::Get(StringId::HelpCommandBar);
    case ShortcutCommand::Project: return l10n::Get(StringId::HelpProject);
    case ShortcutCommand::AdvancedSearch: return l10n::Get(StringId::AdvancedSearch);
    case ShortcutCommand::SelectPattern: return l10n::Get(StringId::HelpSelectPattern);
    case ShortcutCommand::InvertSelection: return l10n::Get(StringId::InvertSelection);
    case ShortcutCommand::BatchRename: return l10n::Get(StringId::BatchRename);
    case ShortcutCommand::CopyPath: return l10n::Get(StringId::CopyPath);
    case ShortcutCommand::AiAssistant: return l10n::Pick(L"AI 助手", L"AI assistant");
    case ShortcutCommand::LayoutSingle: return l10n::Get(StringId::LayoutSingle);
    case ShortcutCommand::LayoutTwoVertical: return l10n::Get(StringId::LayoutVertical);
    case ShortcutCommand::LayoutThree: return l10n::Get(StringId::LayoutThree);
    case ShortcutCommand::LayoutFour: return l10n::Get(StringId::LayoutFour);
    case ShortcutCommand::CustomizeToolbar: return l10n::Pick(L"自定义工具栏", L"Customize toolbar");
    case ShortcutCommand::FavoritesBar: return l10n::Pick(L"收藏栏", L"Favorites bar");
    default: return {};
    }
}

namespace {
ui::ShortcutRowView Row(ShortcutCommand command, const std::vector<uint32_t>& overrides) {
    ui::ShortcutRowView row;
    row.action = ShortcutCommandName(command);
    row.command = static_cast<int>(command);
    const auto chords = EffectiveShortcuts(overrides, command);
    for (size_t i = 0; i < chords.size(); ++i) {
        if (i) row.keys += L" / ";
        row.keys += DescribeShortcut(chords[i]);
    }
    return row;
}

ui::ShortcutRowView StaticRow(StringId label, std::wstring keys) {
    ui::ShortcutRowView row;
    row.action = l10n::Get(label);
    row.keys = std::move(keys);
    return row;
}
}

std::vector<ui::ShortcutGroupView> ShortcutGroupsWithPrefs(
    const std::vector<uint32_t>& overrides) {
    return {
        {l10n::Get(StringId::HelpGroupNav),
         {Row(ShortcutCommand::Open, overrides), Row(ShortcutCommand::Back, overrides),
          Row(ShortcutCommand::Forward, overrides), Row(ShortcutCommand::Up, overrides),
          Row(ShortcutCommand::Refresh, overrides),
          StaticRow(StringId::HelpGoToPath, L"Ctrl L / Alt D / F4"),
          StaticRow(StringId::HelpMoveFocus, L"↑ ↓ PgUp PgDn Home End")}},
        {l10n::Get(StringId::HelpGroupFiles),
         {Row(ShortcutCommand::QuickPreview, overrides), Row(ShortcutCommand::Copy, overrides),
          Row(ShortcutCommand::Cut, overrides), Row(ShortcutCommand::Paste, overrides),
          Row(ShortcutCommand::CopyPath, overrides), Row(ShortcutCommand::Undo, overrides),
          Row(ShortcutCommand::Rename, overrides), Row(ShortcutCommand::BatchRename, overrides),
          Row(ShortcutCommand::NewFolder, overrides), Row(ShortcutCommand::Delete, overrides),
          Row(ShortcutCommand::DeletePermanent, overrides),
          Row(ShortcutCommand::Properties, overrides),
          StaticRow(StringId::HelpToggleTag, L"Ctrl Shift 1–7")}},
        {l10n::Get(StringId::HelpGroupSearch),
         {Row(ShortcutCommand::Search, overrides), Row(ShortcutCommand::CommandBar, overrides),
          Row(ShortcutCommand::Project, overrides), Row(ShortcutCommand::Filter, overrides),
          Row(ShortcutCommand::AdvancedSearch, overrides),
          Row(ShortcutCommand::AiAssistant, overrides)}},
        {l10n::Get(StringId::HelpGroupSelect),
         {Row(ShortcutCommand::SelectAll, overrides), Row(ShortcutCommand::InvertSelection, overrides),
          Row(ShortcutCommand::SelectPattern, overrides),
          StaticRow(StringId::SelectionHint, l10n::Get(StringId::SelectionKeys))}},
        {l10n::Get(StringId::HelpGroupTabs),
         {Row(ShortcutCommand::NewTab, overrides), Row(ShortcutCommand::CloseTab, overrides),
          Row(ShortcutCommand::NewWindow, overrides), Row(ShortcutCommand::MergeWindows, overrides),
          Row(ShortcutCommand::Sidebar, overrides),
          Row(ShortcutCommand::LayoutSingle, overrides),
          Row(ShortcutCommand::LayoutTwoVertical, overrides),
          Row(ShortcutCommand::LayoutThree, overrides),
          Row(ShortcutCommand::LayoutFour, overrides),
          Row(ShortcutCommand::CustomizeToolbar, overrides),
          Row(ShortcutCommand::FavoritesBar, overrides),
          StaticRow(StringId::HelpSwitchTab, L"Ctrl Tab / Ctrl Shift Tab"),
          StaticRow(StringId::HelpNextPane, L"F6"),
          StaticRow(StringId::HelpMarkTarget, L"Ctrl D"),
          StaticRow(StringId::HelpTransferTarget, L"Ctrl Alt C / X"),
          StaticRow(StringId::HelpMoveWindow,
                    l10n::Pick(L"Alt + 拖动标签", L"Alt + drag a tab"))}},
    };
}
}
