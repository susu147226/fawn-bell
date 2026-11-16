#pragma once
// extension_manager.h — Installed extensions, their permissions, and running
// their commands.
//
// Two kinds share %LOCALAPPDATA%\Luling\extensions\<id>:
//   schema 1 (legacy): a manifest.json with "permissions" and "commands" whose
//     entries name a program to run.
//   schema 2 (plugin): a manifest.json with "api_version", "capabilities" and
//     "contributes", plus a DLL that Luling.Plugin.exe loads. Parsing lives in
//     plugin/plugin_manifest.h; this file only combines it with the user's
//     enable/approval state.
//
// state.json (version 2) records, per extension, whether it is enabled and the
// exact capabilities+version the user approved. A plugin that changes either
// needs a new approval.

#include "../common/windows_compat.h"
#include "../plugin/plugin_manifest.h"

#include <string>
#include <vector>

namespace luling::app {

struct ExtensionCommand {
    std::wstring id;
    std::wstring name;
    std::wstring exec;  // schema 1 only
    std::vector<std::wstring> args;
};

struct InstalledExtension {
    std::wstring id;
    std::wstring name;
    std::wstring version;
    std::wstring dir;
    int schema = 1;
    int api_version = 0;
    std::wstring main_module;  // schema 2 only
    std::vector<std::wstring> capabilities;
    std::vector<std::wstring> permissions;
    std::vector<ExtensionCommand> commands;
    std::vector<plugin::PluginPreviewerSpec> previewers;
    std::vector<plugin::PluginColumnSpec> columns;
    std::vector<plugin::PluginPanelSpec> panels;
    bool enabled = true;
    bool granted = false;

    bool is_plugin() const { return schema >= 2; }
};

// Every extension folder with a readable manifest.json.
std::vector<InstalledExtension> LoadInstalledExtensions();

bool SetExtensionEnabled(const std::wstring& id, bool enabled);
// Records the approval for the exact version and capability set shown to the
// user. A later manifest with different values no longer counts as granted.
void GrantExtensionCapabilities(const std::wstring& id, const std::wstring& version,
                                const std::vector<std::wstring>& capabilities);
void GrantExtensionPermissions(const std::wstring& id);
void RevokeExtensionPermissions(const std::wstring& id);

// Human-readable permission lines for the approval dialog.
std::vector<std::wstring> ExtensionPermissionLines(const InstalledExtension& ext);

// Runs one schema-1 command; returns false and fills `error` when it could not
// start. Schema-2 commands go through app::PluginClient instead.
bool RunExtensionCommand(const InstalledExtension& ext, const ExtensionCommand& cmd,
                         const std::vector<std::wstring>& selection, std::wstring& error);

} // namespace luling::app
