#pragma once
// extension_menu.h — the 扩展 submenu shared by the item context menu and the
// --menushot verifier.

#include "extension_manager.h"
#include "../ui/fluent_menu.h"

#include <vector>

namespace luling::app {

// Zero or one row: a 扩展 submenu containing every enabled extension command.
// Command ids come from plugin::PluginCommandId, so the palette resolves them
// the same way.
std::vector<ui::FluentMenuItem> BuildExtensionMenuItems(
    const std::vector<InstalledExtension>& extensions);

} // namespace luling::app
