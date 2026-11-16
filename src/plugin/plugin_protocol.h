#pragma once
// plugin_protocol.h — Luling <-> Luling.Plugin.exe wire protocol.
//
// Transport: named pipe `\\.\pipe\luling_plugin_<ui-pid>_<token>`, byte mode,
// synchronous I/O. The UI process starts the host with that pid and token and
// is the only client.
//
// Framing: the same 16-byte header and field encoding as ipc/protocol.h
// (PayloadWriter / PayloadReader), with a different magic.
//
//   client -> host : REQ_HELLO (plugin list), REQ_INVOKE, REQ_PING, REQ_SHUTDOWN
//   host -> client : RSP_HELLO, RSP_INVOKE, RSP_PONG
//
// One request at a time in order; RSP_INVOKE answers exactly once.

#include "../ipc/protocol.h"

#include <windows.h>
#include <string>
#include <vector>

namespace luling::plugin {

constexpr uint32_t kPluginMagic = 0x50474C50;  // 'PLGP'
constexpr uint32_t kPluginProtocolVersion = 1;
// An invoke spends up to this long inside plugin code before the client kills
// the host; the DLL cannot be interrupted any other way.
constexpr uint32_t kInvokeTimeoutMs = 20000;

enum PluginMsgType : uint32_t {
    REQ_HELLO = 1,
    REQ_INVOKE = 2,
    REQ_PING = 3,
    REQ_SHUTDOWN = 4,
    REQ_PREVIEW = 5,
    REQ_COLUMNS = 6,
    REQ_PANEL = 7,
    REQ_PANEL_ACTION = 8,
    RSP_HELLO = 101,
    RSP_INVOKE = 102,
    RSP_PONG = 103,
    RSP_PREVIEW = 105,
    RSP_COLUMNS = 106,
    RSP_PANEL = 107,
    RSP_PANEL_ACTION = 108,
};

enum PluginLoadStatus : uint32_t {
    LOAD_OK = 0,
    LOAD_MISSING = 1,       // no DLL at the declared path
    LOAD_ABI_MISMATCH = 2,  // missing or unsupported luling_plugin_abi
    LOAD_INIT_FAILED = 3,
    LOAD_PATH_INVALID = 4,
    LOAD_MANIFEST_INVALID = 5,
};

enum PluginInvokeStatus : uint32_t {
    INVOKE_OK = 0,
    INVOKE_UNKNOWN_PLUGIN = 1,
    INVOKE_UNKNOWN_COMMAND = 2,
    INVOKE_NOT_LOADED = 3,
    INVOKE_PLUGIN_FAILED = 4,
    INVOKE_TIMEOUT = 5,
};

enum PluginPreviewStatus : uint32_t {
    PREVIEW_OK = 0,
    PREVIEW_NONE = 1,      // no loaded plugin claims this extension
    PREVIEW_FAILED = 2,
    PREVIEW_NOT_LOADED = 3,
};

// Preview requests are clamped to this edge. 1024 is 4 MiB of BGRA, well
// inside the 16 MiB payload cap, and matches the details pane's draw size.
constexpr uint32_t kPreviewMaxEdge = 1024;
constexpr uint32_t kPreviewDefaultEdge = 1024;

// Column values are one string per path, joined by this unit separator.
constexpr wchar_t kColumnValueSeparator = L'\x1F';
constexpr uint32_t kColumnMaxRows = 512;

// Panel items: records joined by U+001E, fields inside a record by U+001F.
constexpr wchar_t kPanelRecordSeparator = L'\x1E';
constexpr uint32_t kPanelMaxItems = 32;

inline std::wstring PluginPipeName(uint32_t ui_pid, uint32_t token) {
    return L"\\\\.\\pipe\\luling_plugin_" + std::to_wstring(ui_pid) + L"_" +
           std::to_wstring(token);
}

// Synchronous blocking I/O on a byte-mode pipe (no FILE_FLAG_OVERLAPPED).
inline bool PipeReadAll(HANDLE pipe, void* data, DWORD bytes) {
    auto* out = static_cast<unsigned char*>(data);
    while (bytes) {
        DWORD got = 0;
        if (!ReadFile(pipe, out, bytes, &got, nullptr) || got == 0) return false;
        out += got;
        bytes -= got;
    }
    return true;
}

inline bool PipeWriteAll(HANDLE pipe, const void* data, DWORD bytes) {
    auto* in = static_cast<const unsigned char*>(data);
    while (bytes) {
        DWORD written = 0;
        if (!WriteFile(pipe, in, bytes, &written, nullptr) || written == 0) return false;
        in += written;
        bytes -= written;
    }
    return true;
}

struct PluginEntry {
    std::wstring id;
    std::wstring dir;
};

struct PluginLoadResult {
    std::wstring id;
    uint32_t status = LOAD_OK;
    std::wstring error;
};

} // namespace luling::plugin
