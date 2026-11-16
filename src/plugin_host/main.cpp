// Luling.Plugin.exe: runs plugin DLLs outside the UI process.
//
// The UI process starts this host with its own pid and a per-session token,
// then sends the list of installed plugins. The host validates every manifest
// again, loads the declared DLL, and routes command invocations over the pipe.
// A plugin that crashes or hangs takes this process down, not Luling.

#include "../plugin/plugin_api.h"
#include "../plugin/plugin_manifest.h"
#include "../plugin/plugin_protocol.h"
#include "../common/crash_reporter.h"
#include "../common/current_user_security.h"
#include "../common/runtime_log.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using luling::plugin::PluginLoadResult;
using luling::plugin::PluginLoadStatus;
using luling::plugin::PluginManifest;
using luling::plugin::PluginMsgType;

namespace {

constexpr ULONGLONG kIdleTimeoutMs = 60ull * 1000ull;
constexpr size_t kMaxManifestBytes = 1u << 20;

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string body;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
        size.QuadPart <= static_cast<LONGLONG>(kMaxManifestBytes)) {
        body.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (ReadFile(file, body.data(), static_cast<DWORD>(body.size()), &got, nullptr))
            body.resize(got);
        else
            body.clear();
    }
    CloseHandle(file);
    out = std::move(body);
    return !out.empty();
}

void __cdecl PluginLog(int level, const wchar_t* message) {
    (void)level;
    if (message) OutputDebugStringW(message);
}

// "C:\dir\Name.MD" -> "md"; empty when the leaf has no extension.
std::wstring LowerExtension(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return {};
    std::wstring extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return extension;
}

struct LoadedPlugin {
    std::wstring id;
    std::wstring dir;
    PluginManifest manifest;
    HMODULE module = nullptr;
    uint32_t (*abi)() = nullptr;
    int (*init)(const LulingPluginHost*) = nullptr;
    void (*shutdown)() = nullptr;
    int (*invoke)(const wchar_t*, const wchar_t* const*, uint32_t, wchar_t*, uint32_t) = nullptr;
    int (*render_preview)(const wchar_t*, uint32_t, uint8_t*, uint32_t, uint32_t*, uint32_t*,
                          wchar_t*, uint32_t) = nullptr;
    int (*column_values)(const wchar_t*, const wchar_t* const*, uint32_t, wchar_t*,
                         uint32_t) = nullptr;
    int (*panel_items)(const wchar_t*, uint32_t, wchar_t*, uint32_t) = nullptr;
    int (*panel_action)(const wchar_t*, const wchar_t*, wchar_t*, uint32_t) = nullptr;
    PluginLoadStatus status = PluginLoadStatus::LOAD_OK;
    std::wstring error;

    bool HasCommand(const std::wstring& command_id) const {
        return std::any_of(manifest.commands.begin(), manifest.commands.end(),
                           [&](const luling::plugin::PluginCommandSpec& spec) {
                               return spec.id == command_id;
                           });
    }
};

void UnloadPlugin(LoadedPlugin& plugin) {
    if (plugin.module) {
        if (plugin.shutdown) plugin.shutdown();
        FreeLibrary(plugin.module);
        plugin.module = nullptr;
    }
    plugin.abi = nullptr;
    plugin.init = nullptr;
    plugin.shutdown = nullptr;
    plugin.invoke = nullptr;
    plugin.render_preview = nullptr;
    plugin.column_values = nullptr;
    plugin.panel_items = nullptr;
    plugin.panel_action = nullptr;
}

bool LoadOnePlugin(const luling::plugin::PluginEntry& entry, LoadedPlugin& out) {
    out.id = entry.id;
    out.dir = entry.dir;
    if (out.dir.empty() ||
        (out.dir.front() != L'\\' && out.dir.find(L':') == std::wstring::npos)) {
        out.status = PluginLoadStatus::LOAD_PATH_INVALID;
        out.error = L"plugin folder is not absolute";
        return false;
    }
    std::string manifest_body;
    if (!ReadFileUtf8(out.dir + L"\\manifest.json", manifest_body)) {
        out.status = PluginLoadStatus::LOAD_MANIFEST_INVALID;
        out.error = L"manifest.json missing or unreadable";
        return false;
    }
    out.manifest = luling::plugin::ParsePluginManifest(manifest_body, entry.id);
    if (!out.manifest.ok()) {
        out.status = PluginLoadStatus::LOAD_MANIFEST_INVALID;
        out.error = out.manifest.error;
        return false;
    }
    if (out.manifest.id != entry.id) {
        out.status = PluginLoadStatus::LOAD_MANIFEST_INVALID;
        out.error = L"manifest id does not match its folder";
        return false;
    }
    const std::wstring dll_path = out.dir + L"\\" + out.manifest.main;
    if (GetFileAttributesW(dll_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        out.status = PluginLoadStatus::LOAD_MISSING;
        out.error = L"plugin module not found";
        return false;
    }
    out.module = LoadLibraryExW(dll_path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!out.module) {
        out.status = PluginLoadStatus::LOAD_MISSING;
        out.error = L"plugin module could not be loaded";
        return false;
    }
    out.abi = reinterpret_cast<uint32_t (*)()>(
        GetProcAddress(out.module, "luling_plugin_abi"));
    if (!out.abi || out.abi() != LULING_PLUGIN_ABI_VERSION) {
        out.status = PluginLoadStatus::LOAD_ABI_MISMATCH;
        out.error = L"plugin ABI mismatch";
        UnloadPlugin(out);
        return false;
    }
    out.init = reinterpret_cast<int (*)(const LulingPluginHost*)>(
        GetProcAddress(out.module, "luling_plugin_init"));
    out.shutdown = reinterpret_cast<void (*)()>(
        GetProcAddress(out.module, "luling_plugin_shutdown"));
    out.invoke = reinterpret_cast<int (*)(const wchar_t*, const wchar_t* const*, uint32_t,
                                          wchar_t*, uint32_t)>(
        GetProcAddress(out.module, "luling_plugin_invoke"));
    out.render_preview = reinterpret_cast<int (*)(const wchar_t*, uint32_t, uint8_t*, uint32_t,
                                                  uint32_t*, uint32_t*, wchar_t*, uint32_t)>(
        GetProcAddress(out.module, "luling_plugin_render_preview"));
    out.column_values = reinterpret_cast<int (*)(const wchar_t*, const wchar_t* const*, uint32_t,
                                                 wchar_t*, uint32_t)>(
        GetProcAddress(out.module, "luling_plugin_column_values"));
    out.panel_items = reinterpret_cast<int (*)(const wchar_t*, uint32_t, wchar_t*, uint32_t)>(
        GetProcAddress(out.module, "luling_plugin_panel_items"));
    out.panel_action = reinterpret_cast<int (*)(const wchar_t*, const wchar_t*, wchar_t*,
                                                uint32_t)>(
        GetProcAddress(out.module, "luling_plugin_panel_action"));
    if (!out.invoke) {
        out.status = PluginLoadStatus::LOAD_ABI_MISMATCH;
        out.error = L"luling_plugin_invoke is missing";
        UnloadPlugin(out);
        return false;
    }
    if (out.init) {
        const LulingPluginHost host{static_cast<uint32_t>(sizeof(LulingPluginHost)),
                                    LULING_PLUGIN_ABI_VERSION, out.dir.c_str(), &PluginLog};
        if (out.init(&host) != 0) {
            out.status = PluginLoadStatus::LOAD_INIT_FAILED;
            out.error = L"plugin init failed";
            UnloadPlugin(out);
            return false;
        }
    }
    out.status = PluginLoadStatus::LOAD_OK;
    return true;
}

bool SendResponse(HANDLE pipe, PluginMsgType type, uint32_t request_id,
                  const luling::ipc::PayloadWriter& writer) {
    luling::ipc::MsgHeader header;
    header.magic = luling::plugin::kPluginMagic;
    header.type = static_cast<uint32_t>(type);
    header.request_id = request_id;
    header.payload_size = static_cast<uint32_t>(writer.data().size());
    return luling::plugin::PipeWriteAll(pipe, &header, sizeof(header)) &&
           (writer.data().empty() ||
            luling::plugin::PipeWriteAll(pipe, writer.data().data(),
                                         static_cast<DWORD>(writer.data().size())));
}

// Waits for the next request, giving up when the UI process is gone or the
// host has been idle for a minute. Returns false when the loop should end.
bool WaitForRequest(HANDLE pipe, HANDLE ui_process, ULONGLONG& idle_since) {
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available > 0) return true;
        if (ui_process && WaitForSingleObject(ui_process, 0) == WAIT_OBJECT_0) return false;
        if (GetTickCount64() - idle_since >= kIdleTimeoutMs) return false;
        Sleep(100);
    }
}

void HandleHello(HANDLE pipe, uint32_t request_id, luling::ipc::PayloadReader& reader,
                 std::vector<std::unique_ptr<LoadedPlugin>>& plugins) {
    uint32_t count = 0;
    if (!reader.GetU32(count) || count > static_cast<uint32_t>(luling::plugin::kPluginMaxCount))
        return;
    for (auto& plugin : plugins) UnloadPlugin(*plugin);
    plugins.clear();
    std::vector<PluginLoadResult> results;
    for (uint32_t i = 0; i < count; ++i) {
        luling::plugin::PluginEntry entry;
        if (!reader.GetString(entry.id) || !reader.GetString(entry.dir)) return;
        auto plugin = std::make_unique<LoadedPlugin>();
        LoadOnePlugin(entry, *plugin);
        results.push_back({plugin->id, static_cast<uint32_t>(plugin->status), plugin->error});
        plugins.push_back(std::move(plugin));
    }
    luling::ipc::PayloadWriter writer;
    writer.PutU32(static_cast<uint32_t>(results.size()));
    for (const PluginLoadResult& result : results) {
        writer.PutString(result.id);
        writer.PutU32(result.status);
        writer.PutString(result.error);
    }
    SendResponse(pipe, luling::plugin::RSP_HELLO, request_id, writer);
}

void HandleInvoke(HANDLE pipe, uint32_t request_id, luling::ipc::PayloadReader& reader,
                  std::vector<std::unique_ptr<LoadedPlugin>>& plugins) {
    std::wstring plugin_id;
    std::wstring command_id;
    std::vector<std::wstring> paths;
    uint32_t status = luling::plugin::INVOKE_UNKNOWN_PLUGIN;
    std::wstring message;
    if (reader.GetString(plugin_id) && reader.GetString(command_id) &&
        reader.TryStringArray(paths)) {
        auto it = std::find_if(plugins.begin(), plugins.end(),
                               [&](const std::unique_ptr<LoadedPlugin>& plugin) {
                                   return plugin->id == plugin_id;
                               });
        if (it == plugins.end()) {
            status = luling::plugin::INVOKE_UNKNOWN_PLUGIN;
        } else if ((*it)->status != PluginLoadStatus::LOAD_OK) {
            status = luling::plugin::INVOKE_NOT_LOADED;
            message = (*it)->error;
        } else if (!(*it)->HasCommand(command_id)) {
            status = luling::plugin::INVOKE_UNKNOWN_COMMAND;
        } else {
            std::vector<const wchar_t*> raw_paths;
            raw_paths.reserve(paths.size());
            for (const std::wstring& path : paths) raw_paths.push_back(path.c_str());
            wchar_t buffer[2048]{};
            const int rc = (*it)->invoke(command_id.c_str(),
                                         raw_paths.empty() ? nullptr : raw_paths.data(),
                                         static_cast<uint32_t>(raw_paths.size()), buffer,
                                         static_cast<uint32_t>(std::size(buffer)));
            status = rc == 0 ? luling::plugin::INVOKE_OK : luling::plugin::INVOKE_PLUGIN_FAILED;
            // A plugin may report something useful on success too.
            message.assign(buffer);
        }
    }
    luling::ipc::PayloadWriter writer;
    writer.PutU32(status);
    writer.PutString(message);
    SendResponse(pipe, luling::plugin::RSP_INVOKE, request_id, writer);
}

void HandlePreview(HANDLE pipe, uint32_t request_id, luling::ipc::PayloadReader& reader,
                   std::vector<std::unique_ptr<LoadedPlugin>>& plugins) {
    std::wstring path;
    uint32_t requested = 0;
    uint32_t status = luling::plugin::PREVIEW_NONE;
    std::wstring message;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;
    if (reader.GetString(path) && reader.GetU32(requested) && !path.empty()) {
        const uint32_t max_px =
            std::clamp(requested, 32u, luling::plugin::kPreviewMaxEdge);
        const std::wstring extension = LowerExtension(path);
        LoadedPlugin* best = nullptr;
        int best_priority = -1000000;
        for (auto& plugin : plugins) {
            if (plugin->status != PluginLoadStatus::LOAD_OK || !plugin->render_preview) continue;
            for (const auto& spec : plugin->manifest.previewers) {
                if (std::find(spec.extensions.begin(), spec.extensions.end(), extension) ==
                    spec.extensions.end())
                    continue;
                if (spec.priority > best_priority) {
                    best_priority = spec.priority;
                    best = plugin.get();
                }
            }
        }
        if (best) {
            bgra.assign(static_cast<size_t>(max_px) * max_px * 4, 0);
            wchar_t buffer[1024]{};
            uint32_t out_w = 0;
            uint32_t out_h = 0;
            const int rc = best->render_preview(
                path.c_str(), max_px, bgra.data(), static_cast<uint32_t>(bgra.size()), &out_w,
                &out_h, buffer, static_cast<uint32_t>(std::size(buffer)));
            if (rc == 1) {
                status = luling::plugin::PREVIEW_NONE;
            } else if (rc != 0) {
                status = luling::plugin::PREVIEW_FAILED;
                message.assign(buffer);
            } else if (out_w == 0 || out_h == 0 || out_w > max_px || out_h > max_px) {
                status = luling::plugin::PREVIEW_FAILED;
                message = L"invalid preview size";
            } else {
                status = luling::plugin::PREVIEW_OK;
                width = out_w;
                height = out_h;
                bgra.resize(static_cast<size_t>(out_w) * out_h * 4);
            }
            if (status != luling::plugin::PREVIEW_OK) bgra.clear();
        }
    }
    luling::ipc::PayloadWriter writer;
    writer.PutU32(status);
    writer.PutU32(width);
    writer.PutU32(height);
    writer.PutString(message);
    if (status == luling::plugin::PREVIEW_OK && !bgra.empty())
        writer.PutBytes(bgra.data(), bgra.size());
    SendResponse(pipe, luling::plugin::RSP_PREVIEW, request_id, writer);
}

void HandleColumns(HANDLE pipe, uint32_t request_id, luling::ipc::PayloadReader& reader,
                   std::vector<std::unique_ptr<LoadedPlugin>>& plugins) {
    std::wstring plugin_id;
    std::wstring column_id;
    std::vector<std::wstring> paths;
    uint32_t status = luling::plugin::PREVIEW_NONE;
    std::wstring values;
    if (reader.GetString(plugin_id) && reader.GetString(column_id) &&
        reader.TryStringArray(paths) &&
        paths.size() <= static_cast<size_t>(luling::plugin::kColumnMaxRows)) {
        LoadedPlugin* target = nullptr;
        for (auto& plugin : plugins)
            if (plugin->id == plugin_id) {
                target = plugin.get();
                break;
            }
        const bool declared =
            target && std::any_of(target->manifest.columns.begin(),
                                  target->manifest.columns.end(),
                                  [&](const luling::plugin::PluginColumnSpec& spec) {
                                      return spec.id == column_id;
                                  });
        if (!target || !declared || !target->column_values) {
            status = luling::plugin::PREVIEW_NONE;
        } else if (paths.empty()) {
            status = luling::plugin::PREVIEW_OK;
        } else {
            std::vector<const wchar_t*> raw;
            raw.reserve(paths.size());
            for (const std::wstring& path : paths) raw.push_back(path.c_str());
            std::vector<wchar_t> buffer(256 * 1024);
            const int rc = target->column_values(column_id.c_str(), raw.data(),
                                                 static_cast<uint32_t>(raw.size()),
                                                 buffer.data(),
                                                 static_cast<uint32_t>(buffer.size()));
            if (rc == 1) {
                status = luling::plugin::PREVIEW_NONE;
            } else if (rc != 0) {
                status = luling::plugin::PREVIEW_FAILED;
            } else {
                status = luling::plugin::PREVIEW_OK;
                values.assign(buffer.data());
            }
        }
    }
    luling::ipc::PayloadWriter writer;
    writer.PutU32(status);
    writer.PutString(values);
    SendResponse(pipe, luling::plugin::RSP_COLUMNS, request_id, writer);
}

void HandlePanel(HANDLE pipe, uint32_t request_id, luling::ipc::PayloadReader& reader,
                 std::vector<std::unique_ptr<LoadedPlugin>>& plugins) {
    std::wstring panel_id;
    uint32_t requested = 0;
    uint32_t status = luling::plugin::PREVIEW_NONE;
    std::wstring items;
    if (reader.GetString(panel_id) && reader.GetU32(requested)) {
        const uint32_t max_items =
            std::clamp(requested, 1u, luling::plugin::kPanelMaxItems);
        LoadedPlugin* target = nullptr;
        for (auto& plugin : plugins) {
            if (!plugin->panel_items) continue;
            const bool declared =
                std::any_of(plugin->manifest.panels.begin(), plugin->manifest.panels.end(),
                            [&](const luling::plugin::PluginPanelSpec& spec) {
                                return spec.id == panel_id;
                            });
            if (declared) {
                target = plugin.get();
                break;
            }
        }
        if (!target) {
            status = luling::plugin::PREVIEW_NONE;
        } else {
            std::vector<wchar_t> buffer(64 * 1024);
            const int rc = target->panel_items(panel_id.c_str(), max_items, buffer.data(),
                                               static_cast<uint32_t>(buffer.size()));
            if (rc == 1) {
                status = luling::plugin::PREVIEW_NONE;
            } else if (rc != 0) {
                status = luling::plugin::PREVIEW_FAILED;
            } else {
                status = luling::plugin::PREVIEW_OK;
                items.assign(buffer.data());
            }
        }
    }
    luling::ipc::PayloadWriter writer;
    writer.PutU32(status);
    writer.PutString(items);
    SendResponse(pipe, luling::plugin::RSP_PANEL, request_id, writer);
}

void HandlePanelAction(HANDLE pipe, uint32_t request_id, luling::ipc::PayloadReader& reader,
                       std::vector<std::unique_ptr<LoadedPlugin>>& plugins) {
    std::wstring panel_id;
    std::wstring action_id;
    uint32_t status = luling::plugin::PREVIEW_NONE;
    std::wstring message;
    if (reader.GetString(panel_id) && reader.GetString(action_id) && !action_id.empty()) {
        LoadedPlugin* target = nullptr;
        for (auto& plugin : plugins) {
            if (!plugin->panel_action) continue;
            const bool declared =
                std::any_of(plugin->manifest.panels.begin(), plugin->manifest.panels.end(),
                            [&](const luling::plugin::PluginPanelSpec& spec) {
                                return spec.id == panel_id;
                            });
            if (declared) {
                target = plugin.get();
                break;
            }
        }
        if (!target) {
            status = luling::plugin::PREVIEW_NONE;
        } else {
            wchar_t buffer[1024]{};
            const int rc = target->panel_action(panel_id.c_str(), action_id.c_str(), buffer,
                                                static_cast<uint32_t>(std::size(buffer)));
            status = rc == 0 ? luling::plugin::PREVIEW_OK : luling::plugin::PREVIEW_FAILED;
            if (rc != 0) message.assign(buffer);
        }
    }
    luling::ipc::PayloadWriter writer;
    writer.PutU32(status);
    writer.PutString(message);
    SendResponse(pipe, luling::plugin::RSP_PANEL_ACTION, request_id, writer);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    luling::crash::Initialize({luling::crash::ProcessRole::Plugin, false, {}});
    struct DiagnosticsShutdown {
        ~DiagnosticsShutdown() { luling::crash::Shutdown(); }
    } diagnostics_shutdown;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (__argc < 3) return 2;
    const DWORD ui_pid = wcstoul(__wargv[1], nullptr, 10);
    const uint32_t token = static_cast<uint32_t>(wcstoul(__wargv[2], nullptr, 10));
    if (ui_pid == 0 || token == 0) return 2;
    const std::wstring pipe_name = luling::plugin::PluginPipeName(ui_pid, token);
    luling::CurrentUserSecurityAttributes security;
    if (!security) return 3;
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                       PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 64 * 1024, 64 * 1024, 0, security.get());
    if (pipe == INVALID_HANDLE_VALUE) return 3;
    HANDLE ui_process = OpenProcess(SYNCHRONIZE, FALSE, ui_pid);
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(pipe);
        if (ui_process) CloseHandle(ui_process);
        return 4;
    }
    std::vector<std::unique_ptr<LoadedPlugin>> plugins;
    ULONGLONG last_activity = GetTickCount64();
    bool ok = true;
    while (ok) {
        if (!WaitForRequest(pipe, ui_process, last_activity)) break;
        luling::ipc::MsgHeader header{};
        if (!luling::plugin::PipeReadAll(pipe, &header, sizeof(header))) break;
        if (header.magic != luling::plugin::kPluginMagic ||
            header.payload_size > luling::ipc::kMaxPayload)
            break;
        std::vector<uint8_t> payload(header.payload_size);
        if (header.payload_size &&
            !luling::plugin::PipeReadAll(pipe, payload.data(), header.payload_size))
            break;
        last_activity = GetTickCount64();
        luling::ipc::PayloadReader reader(payload.data(), payload.size());
        switch (header.type) {
        case luling::plugin::REQ_HELLO:
            HandleHello(pipe, header.request_id, reader, plugins);
            break;
        case luling::plugin::REQ_INVOKE:
            HandleInvoke(pipe, header.request_id, reader, plugins);
            break;
        case luling::plugin::REQ_PREVIEW:
            HandlePreview(pipe, header.request_id, reader, plugins);
            break;
        case luling::plugin::REQ_COLUMNS:
            HandleColumns(pipe, header.request_id, reader, plugins);
            break;
        case luling::plugin::REQ_PANEL:
            HandlePanel(pipe, header.request_id, reader, plugins);
            break;
        case luling::plugin::REQ_PANEL_ACTION:
            HandlePanelAction(pipe, header.request_id, reader, plugins);
            break;
        case luling::plugin::REQ_PING:
            SendResponse(pipe, luling::plugin::RSP_PONG, header.request_id,
                         luling::ipc::PayloadWriter{});
            break;
        case luling::plugin::REQ_SHUTDOWN:
            ok = false;
            break;
        default:
            break;
        }
    }
    for (auto& plugin : plugins) UnloadPlugin(*plugin);
    plugins.clear();
    if (ui_process) CloseHandle(ui_process);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return 0;
}
