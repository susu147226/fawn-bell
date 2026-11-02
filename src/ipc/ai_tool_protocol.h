// ai_tool_protocol.h - The Luling.AI.exe (MCP host) and luling.exe (broker)
// wire format: one length-prefixed UTF-8 JSON object per message over a named
// pipe. The host speaks MCP on stdio; the broker owns permissions, the
// confirmation dialogs and the activity log.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace luling::ipc::ai {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\Luling.AI.broker.v1";
// The pipe protocol version; the host refuses a broker that answers another.
inline constexpr int kProtocolVersion = 1;

// 4-byte big-endian length, then the UTF-8 payload.
inline bool WriteFrame(HANDLE pipe, std::string_view payload) {
    if (!pipe || pipe == INVALID_HANDLE_VALUE || payload.size() > 0x7FFFFFFF) return false;
    const auto n = static_cast<uint32_t>(payload.size());
    uint8_t header[4] = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                         static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
    DWORD written = 0;
    if (!WriteFile(pipe, header, 4, &written, nullptr) || written != 4) return false;
    const char* data = payload.data();
    size_t left = payload.size();
    while (left > 0) {
        const DWORD chunk = static_cast<DWORD>((left > 0x10000) ? 0x10000 : left);
        if (!WriteFile(pipe, data, chunk, &written, nullptr) || written != chunk) return false;
        data += chunk;
        left -= chunk;
    }
    return true;
}

inline bool ReadExact(HANDLE pipe, void* buffer, size_t bytes) {
    auto* out = static_cast<char*>(buffer);
    size_t left = bytes;
    while (left > 0) {
        DWORD got = 0;
        if (!ReadFile(pipe, out, static_cast<DWORD>(left), &got, nullptr) || got == 0) return false;
        out += got;
        left -= got;
    }
    return true;
}

inline bool ReadFrame(HANDLE pipe, std::string& payload) {
    uint8_t header[4] = {};
    if (!ReadExact(pipe, header, 4)) return false;
    const uint32_t n = (static_cast<uint32_t>(header[0]) << 24) |
                       (static_cast<uint32_t>(header[1]) << 16) |
                       (static_cast<uint32_t>(header[2]) << 8) | header[3];
    if (n > 16 * 1024 * 1024) return false;
    payload.assign(n, '\0');
    return ReadExact(pipe, payload.data(), n);
}

} // namespace luling::ipc::ai
