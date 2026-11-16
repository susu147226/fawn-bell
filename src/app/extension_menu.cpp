#include "extension_menu.h"

#include "context_menu.h"
#include "../common/localization.h"
#include "../plugin/plugin_manifest.h"

#include <algorithm>

namespace luling::app {

std::vector<ui::FluentMenuItem> BuildExtensionMenuItems(
    const std::vector<InstalledExtension>& extensions) {
    std::vector<ui::FluentMenuItem> commands;
    const size_t limit = std::min<size_t>(extensions.size(), 8);
    for (size_t e = 0; e < limit; ++e) {
        const InstalledExtension& ext = extensions[e];
        if (!ext.enabled) continue;
        const size_t command_limit =
            std::min<size_t>(ext.commands.size(), plugin::kPluginCommandsPerPlugin);
        for (size_t c = 0; c < command_limit; ++c) {
            ui::FluentMenuItem item;
            item.command = plugin::PluginCommandId(static_cast<int>(e), static_cast<int>(c));
            item.text = ext.name + L" · " + ext.commands[c].name;
            item.glyph = L"\xE7B8";
            commands.push_back(std::move(item));
        }
    }
    if (commands.empty()) return {};
    ui::FluentMenuItem menu;
    menu.command = CmdNone;
    menu.text = l10n::Pick(L"扩展", L"Extensions");
    menu.glyph = L"\xE7B8";
    menu.children = std::move(commands);
    menu.separator_after = true;
    return {std::move(menu)};
}

} // namespace luling::app
