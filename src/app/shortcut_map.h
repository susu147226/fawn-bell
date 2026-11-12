#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace luling::ui { struct ShortcutGroupView; }

namespace luling::app {
// Keyboard commands the user may rebind on Settings > 快捷键. The legacy key
// chain in app_input.cpp no longer handles these, so a remapped key is the only
// key that runs them.
enum class ShortcutCommand : int {
    None = -1,
    Copy = 0,
    Cut,
    Paste,
    Undo,
    Rename,
    Delete,
    DeletePermanent,
    Refresh,
    NewFolder,
    QuickPreview,
    Open,
    Back,
    Forward,
    Up,
    Properties,
    Filter,
    AddressBar,
    SelectAll,
    NewTab,
    CloseTab,
    NewWindow,
    MergeWindows,
    Sidebar,
    Search,
    CommandBar,
    Project,
    AdvancedSearch,
    SelectPattern,
    InvertSelection,
    BatchRename,
    CopyPath,
    AiAssistant,
    LayoutSingle,
    LayoutTwoVertical,
    LayoutThree,
    LayoutFour,
    CustomizeToolbar,
    FavoritesBar,
    Count,
};

constexpr uint8_t kShortcutCtrl = 1;
constexpr uint8_t kShortcutShift = 2;
constexpr uint8_t kShortcutAlt = 4;

struct ShortcutChord {
    uint32_t vk = 0;
    uint8_t mods = 0;
};

inline bool operator==(const ShortcutChord& a, const ShortcutChord& b) noexcept {
    return a.vk == b.vk && a.mods == b.mods;
}

// app.json stores an override as one packed int per command.
uint32_t PackShortcut(ShortcutCommand command, uint32_t vk, uint8_t mods) noexcept;
ShortcutChord UnpackShortcut(uint32_t packed) noexcept;

std::vector<ShortcutChord> DefaultShortcuts(ShortcutCommand command);
std::vector<ShortcutChord> EffectiveShortcuts(const std::vector<uint32_t>& overrides,
                                              ShortcutCommand command);
// The command the chord runs right now, or None.
ShortcutCommand FindShortcutCommand(const std::vector<uint32_t>& overrides, uint32_t vk,
                                    uint8_t mods);
// Another command already using the chord, or None when it is free.
ShortcutCommand ShortcutOwner(const std::vector<uint32_t>& overrides, uint32_t vk,
                              uint8_t mods, ShortcutCommand except);
void SetShortcut(std::vector<uint32_t>& overrides, ShortcutCommand command, uint32_t vk,
                 uint8_t mods);
void ResetShortcut(std::vector<uint32_t>& overrides, ShortcutCommand command);
void ResetAllShortcuts(std::vector<uint32_t>& overrides);
bool ShortcutOverridden(const std::vector<uint32_t>& overrides, ShortcutCommand command);
// The chord is a keypress that can be bound (not a bare modifier).
bool IsBindableShortcutKey(uint32_t vk) noexcept;

std::wstring DescribeShortcut(const ShortcutChord& chord);
std::wstring ShortcutCommandName(ShortcutCommand command);

// Settings > 快捷键 cards, with the rebindable rows carrying their command.
std::vector<ui::ShortcutGroupView> ShortcutGroupsWithPrefs(
    const std::vector<uint32_t>& overrides);
}
