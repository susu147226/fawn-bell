#pragma once

#include "app_state.h"

namespace luling::app {
// Runs one rebindable command (shortcut_map.h); false when the command has no
// handler in this context.
bool RunShortcutCommand(AppState& s, int command);
// Settings > 快捷键 row is waiting for a key: bind, reset or cancel it.
void HandleShortcutCapture(AppState& s, unsigned vk);
// Ctrl/Shift/Alt as shortcut_map modifier bits, from the live key state.
uint8_t ModifierBits() noexcept;
}
