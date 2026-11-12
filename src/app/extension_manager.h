#pragma once
// extension_manager.h — Installed extensions, their permissions, and running
// their commands.
//
// An extension is a folder under %LOCALAPPDATA%\Luling\extensions\<id> with a
// manifest.json:
//   {
//     "id": "hello", "name": "示例扩展", "version": "1.0.0",
//     "permissions": ["run-program", "selection-paths"],
//     "commands": [
//       { "id": "hello", "name": "示例：打开所在文件夹",
//         "exec": "hello.exe", "args": ["{folder}"] }
//     ]
//   }
// Commands run as child processes with no console and the extension folder as
// their working directory, only after the user approves the permissions.

#include "../common/windows_compat.h"

#include <string>
#include <vector>

namespace luling::app {

struct ExtensionCommand {
    std::wstring id;
    std::wstring name;
    std::wstring exec;
    std::vector<std::wstring> args;
};

struct InstalledExtension {
    std::wstring id;
    std::wstring name;
    std::wstring version;
    std::wstring dir;
    std::vector<std::wstring> permissions;
    std::vector<ExtensionCommand> commands;
    bool enabled = true;
    bool granted = false;
};

// Every extension folder with a readable manifest.json.
std::vector<InstalledExtension> LoadInstalledExtensions();

bool SetExtensionEnabled(const std::wstring& id, bool enabled);
void GrantExtensionPermissions(const std::wstring& id);
void RevokeExtensionPermissions(const std::wstring& id);

// Human-readable permission lines for the approval dialog.
std::vector<std::wstring> ExtensionPermissionLines(const InstalledExtension& ext);

// Command ids used by the context menu: 33100 + extension * 32 + command.
constexpr int kExtensionCommandBase = 33100;
constexpr int kExtensionCommandsPerExtension = 32;
int ExtensionCommandId(int extension_index, int command_index);
bool ParseExtensionCommandId(int command, int& extension_index, int& command_index);

// Runs one command; returns false and fills `error` when it could not start.
bool RunExtensionCommand(const InstalledExtension& ext, const ExtensionCommand& cmd,
                         const std::vector<std::wstring>& selection, std::wstring& error);

} // namespace luling::app
