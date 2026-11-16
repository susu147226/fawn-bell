// plugin_manifest.h — schema-2 plugin manifests.
//
// A plugin lives in %LOCALAPPDATA%\Luling\extensions\<id> alongside the v1
// (command-line) extensions. Its manifest declares a DLL, the capabilities it
// needs, and the contributions the host may show. The host verifies the
// manifest before anything in the folder is loaded.
//
//   {
//     "schema": 2, "id": "hello", "name": "示例插件", "version": "1.0.0",
//     "api_version": 1, "main": "plugin.dll",
//     "capabilities": ["ui.command"],
//     "contributes": {
//       "commands": [ { "id": "hello", "title": "示例：打开所在文件夹" } ]
//     }
//   }
#pragma once

#include <string>
#include <vector>

namespace luling::plugin {

struct PluginCommandSpec {
    std::wstring id;
    std::wstring title;
    std::wstring glyph;  // optional Segoe MDL2 code point as text, e.g. ""
};

struct PluginPreviewerSpec {
    std::vector<std::wstring> extensions;  // lower-case, without the dot
    int priority = 0;                      // higher wins between plugins
};

struct PluginColumnSpec {
    std::wstring id;
    std::wstring title;
    int width = 120;  // DIP
};

struct PluginPanelSpec {
    std::wstring id;
    std::wstring title;
};

struct PluginManifest {
    int schema = 0;
    std::wstring id;
    std::wstring name;
    std::wstring version;
    int api_version = 0;
    std::wstring main = L"plugin.dll";
    std::vector<std::wstring> capabilities;
    std::vector<PluginCommandSpec> commands;
    std::vector<PluginPreviewerSpec> previewers;
    std::vector<PluginColumnSpec> columns;
    std::vector<PluginPanelSpec> panels;
    std::wstring error;  // non-empty: the manifest is rejected

    bool ok() const { return error.empty(); }
};

// Parses one manifest body. `folder_name` supplies the id when the manifest
// omits it. Unknown capabilities, duplicate command ids, a wrong schema or a
// wrong api_version reject the manifest with a stable error token.
PluginManifest ParsePluginManifest(const std::string& utf8_json,
                                   const std::wstring& folder_name);

// True for a manifest the host may load.
bool IsLoadablePlugin(const PluginManifest& manifest);

// Capabilities understood by this build. Anything else is rejected.
const std::vector<std::wstring>& KnownCapabilities();
bool IsKnownCapability(const std::wstring& capability);

// Human-readable permission lines for the approval dialog.
std::vector<std::wstring> CapabilityLines(const std::vector<std::wstring>& capabilities);

// Command ids in the context menu and the command palette. The range sits
// between the Shell verb window (8000..40767) and the 新建 ids (100000+).
// An extension's index is its position in the sorted installed list.
constexpr int kPluginCommandBase = 50000;
constexpr int kPluginCommandsPerPlugin = 32;
constexpr int kPluginMaxCommands = kPluginCommandsPerPlugin;
constexpr int kPluginMaxCount = 256;
int PluginCommandId(int plugin_index, int command_index);
bool ParsePluginCommandId(int command, int& plugin_index, int& command_index);

// A folder name may become a directory name: no separators, no dots-only.
std::wstring SanitizePluginId(const std::wstring& id);

} // namespace luling::plugin
