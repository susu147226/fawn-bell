#include "extension_manager.h"

#include "session.h"
#include "../common/json_value.h"
#include "../common/localization.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <map>

namespace luling::app {
namespace {

struct ExtensionState {
    bool enabled = true;
    bool granted = false;
};

std::wstring ExtensionsRoot() { return GetLulingDataDir() + L"\\extensions"; }
std::wstring StatePath() { return ExtensionsRoot() + L"\\state.json"; }

bool ReadUtf8(const std::wstring& path, std::string& out) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
    char buffer[16384];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, n);
    fclose(file);
    return true;
}

void WriteUtf8(const std::wstring& path, const std::string& text) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return;
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
}

std::map<std::wstring, ExtensionState> LoadStates() {
    std::map<std::wstring, ExtensionState> states;
    std::string body;
    if (!ReadUtf8(StatePath(), body)) return states;
    json::Value root;
    if (!json::Parse(body, root, nullptr) || !root.is_object()) return states;
    const json::Value* list = root.find("extensions");
    if (!list || !list->is_array()) return states;
    for (const auto& item : list->items()) {
        if (!item.is_object()) continue;
        ExtensionState state;
        state.enabled = item.get_bool("enabled", true);
        state.granted = item.get_bool("granted", false);
        const std::wstring id = json::Wide(item.get_string("id"));
        if (!id.empty()) states[id] = state;
    }
    return states;
}

void SaveStates(const std::map<std::wstring, ExtensionState>& states) {
    json::Value root = json::Value::Object();
    root.set("version", json::Value(1));
    json::Value list = json::Value::Array();
    for (const auto& [id, state] : states) {
        json::Value item = json::Value::Object();
        item.set("id", json::Value(json::Utf8(id)));
        item.set("enabled", json::Value(state.enabled));
        item.set("granted", json::Value(state.granted));
        list.push(std::move(item));
    }
    root.set("extensions", std::move(list));
    CreateDirectoryW(ExtensionsRoot().c_str(), nullptr);
    WriteUtf8(StatePath(), json::Serialize(root));
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring UnicodeToWide(const std::string& utf8) { return json::Wide(utf8); }

// The folder part of a path, for {folder}.
std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return path.substr(0, slash);
}

std::wstring Quote(const std::wstring& value) {
    if (value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring out = L"\"";
    for (wchar_t c : value) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L"\"";
    return out;
}

std::wstring ExpandToken(const std::wstring& arg, const std::vector<std::wstring>& selection) {
    std::wstring out = arg;
    const std::wstring first = selection.empty() ? std::wstring() : selection.front();
    auto replace_all = [](std::wstring& text, const std::wstring& token,
                          const std::wstring& value) {
        size_t pos = 0;
        while ((pos = text.find(token, pos)) != std::wstring::npos) {
            text.replace(pos, token.size(), value);
            pos += value.size();
        }
    };
    replace_all(out, L"{path}", first);
    replace_all(out, L"{folder}", ParentOf(first));
    replace_all(out, L"{name}", first.empty() ? std::wstring()
                                              : first.substr(first.find_last_of(L"\\/") + 1));
    return out;
}

} // namespace

std::vector<InstalledExtension> LoadInstalledExtensions() {
    std::vector<InstalledExtension> out;
    const std::wstring root = ExtensionsRoot();
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return out;
    const auto states = LoadStates();
    std::vector<std::wstring> dirs;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        dirs.emplace_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(dirs.begin(), dirs.end());
    for (const std::wstring& folder : dirs) {
        const std::wstring dir = root + L"\\" + folder;
        std::string body;
        if (!ReadUtf8(dir + L"\\manifest.json", body)) continue;
        json::Value manifest;
        if (!json::Parse(body, manifest, nullptr) || !manifest.is_object()) continue;
        InstalledExtension ext;
        ext.id = UnicodeToWide(manifest.get_string("id"));
        if (ext.id.empty()) ext.id = folder;
        ext.name = UnicodeToWide(manifest.get_string("name"));
        if (ext.name.empty()) ext.name = ext.id;
        ext.version = UnicodeToWide(manifest.get_string("version"));
        ext.dir = dir;
        if (const json::Value* permissions = manifest.find("permissions");
            permissions && permissions->is_array()) {
            for (const auto& permission : permissions->items())
                if (permission.is_string())
                    ext.permissions.push_back(UnicodeToWide(permission.as_string()));
        }
        if (const json::Value* commands = manifest.find("commands");
            commands && commands->is_array()) {
            for (const auto& item : commands->items()) {
                if (!item.is_object()) continue;
                ExtensionCommand cmd;
                cmd.id = UnicodeToWide(item.get_string("id"));
                cmd.name = UnicodeToWide(item.get_string("name"));
                cmd.exec = UnicodeToWide(item.get_string("exec"));
                if (cmd.name.empty() || cmd.exec.empty()) continue;
                if (const json::Value* args = item.find("args"); args && args->is_array()) {
                    for (const auto& arg : args->items())
                        if (arg.is_string())
                            cmd.args.push_back(UnicodeToWide(arg.as_string()));
                }
                ext.commands.push_back(std::move(cmd));
            }
        }
        if (const auto it = states.find(ext.id); it != states.end()) {
            ext.enabled = it->second.enabled;
            ext.granted = it->second.granted;
        }
        out.push_back(std::move(ext));
    }
    return out;
}

bool SetExtensionEnabled(const std::wstring& id, bool enabled) {
    auto states = LoadStates();
    states[id].enabled = enabled;
    SaveStates(states);
    return true;
}

void GrantExtensionPermissions(const std::wstring& id) {
    auto states = LoadStates();
    states[id].granted = true;
    states[id].enabled = true;
    SaveStates(states);
}

void RevokeExtensionPermissions(const std::wstring& id) {
    auto states = LoadStates();
    states[id].granted = false;
    SaveStates(states);
}

std::vector<std::wstring> ExtensionPermissionLines(const InstalledExtension& ext) {
    std::vector<std::wstring> lines;
    for (const std::wstring& permission : ext.permissions) {
        if (permission == L"run-program")
            lines.push_back(l10n::Pick(L"运行扩展自带的程序",
                                       L"Run programs that come with the extension"));
        else if (permission == L"selection-paths")
            lines.push_back(l10n::Pick(L"读取当前选中文件的完整路径",
                                       L"Read the full paths of the selected files"));
        else
            lines.push_back(permission);
    }
    return lines;
}

int ExtensionCommandId(int extension_index, int command_index) {
    return kExtensionCommandBase + extension_index * kExtensionCommandsPerExtension +
           command_index;
}

bool ParseExtensionCommandId(int command, int& extension_index, int& command_index) {
    if (command < kExtensionCommandBase) return false;
    const int offset = command - kExtensionCommandBase;
    extension_index = offset / kExtensionCommandsPerExtension;
    command_index = offset % kExtensionCommandsPerExtension;
    return true;
}

bool RunExtensionCommand(const InstalledExtension& ext, const ExtensionCommand& cmd,
                         const std::vector<std::wstring>& selection, std::wstring& error) {
    // The program must live inside the extension folder: a manifest cannot point
    // at an arbitrary file on the disk.
    std::wstring exec = cmd.exec;
    if (exec.find_first_of(L"\\/") == std::wstring::npos || exec.find(L':') == std::wstring::npos)
        exec = ext.dir + L"\\" + exec;
    wchar_t full_exec[MAX_PATH]{};
    if (!GetFullPathNameW(exec.c_str(), ARRAYSIZE(full_exec), full_exec, nullptr)) {
        error = L"invalid exec path";
        return false;
    }
    wchar_t full_dir[MAX_PATH]{};
    if (!GetFullPathNameW(ext.dir.c_str(), ARRAYSIZE(full_dir), full_dir, nullptr)) {
        error = L"invalid extension folder";
        return false;
    }
    const std::wstring dir_prefix = std::wstring(full_dir) + L"\\";
    if (_wcsnicmp(full_exec, dir_prefix.c_str(), dir_prefix.size()) != 0) {
        error = L"exec outside the extension folder";
        return false;
    }

    std::wstring command_line = Quote(full_exec);
    for (const std::wstring& arg : cmd.args) {
        if (arg == L"{paths}") {
            for (const std::wstring& path : selection) command_line += L" " + Quote(path);
        } else {
            command_line += L" " + Quote(ExpandToken(arg, selection));
        }
    }
    // .cmd/.bat extensions run through the command processor; CreateProcess
    // cannot start a batch file directly.
    std::wstring application = full_exec;
    const size_t dot = exec.find_last_of(L'.');
    const bool batch = dot != std::wstring::npos &&
        (_wcsicmp(exec.c_str() + dot, L".cmd") == 0 || _wcsicmp(exec.c_str() + dot, L".bat") == 0);
    if (batch) {
        command_line = L"cmd.exe /c " + command_line;
        application.clear();
    }

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(application.empty() ? nullptr : application.c_str(), command_line.data(),
                        nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        ext.dir.c_str(), &si, &pi)) {
        error = L"could not start " + std::wstring(cmd.exec) + L" (error " +
                std::to_wstring(GetLastError()) + L")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace luling::app
