// debug_log.h — one human-readable log for diagnosing menu, copy and pack
// failures. Separate from the numeric runtime journal: this one may carry
// paths and names, so it stays on this machine and rotates at 2 MB.
#pragma once
#include <windows.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

namespace luling::diagnostics {

struct DebugLogState {
    std::mutex mutex;
    std::wstring path;
    bool enabled = false;
};

inline DebugLogState& DebugLogStateRef() {
    static DebugLogState state;
    return state;
}

inline bool InitializeDebugLog(const std::wstring& data_root) {
    DebugLogState& state = DebugLogStateRef();
    std::lock_guard lock(state.mutex);
    state.path.clear();
    state.enabled = false;
    if (data_root.empty()) return false;
    wchar_t setting[8]{};
    if (GetEnvironmentVariableW(L"LULING_DEBUG_LOG", setting, ARRAYSIZE(setting)) == 1 &&
        setting[0] == L'0')
        return false;
    CreateDirectoryW(data_root.c_str(), nullptr);
    state.path = data_root + L"\\luling_debug.log";
    state.enabled = true;
    return true;
}

inline bool DebugLogEnabled() { return DebugLogStateRef().enabled; }

// Device paths are long "luling:shell:..." hex strings. Logging them through a
// fixed swprintf_s buffer terminates the process once the path does not fit, so
// every path that reaches the log goes through this clamp instead.
inline std::wstring LogText(std::wstring_view text, size_t limit = 200) {
    if (text.size() <= limit) return std::wstring(text);
    std::wstring shortened(text.substr(0, limit));
    shortened += L"…";
    return shortened;
}

inline void DebugLog(const char* component, std::wstring_view message) {
    DebugLogState& state = DebugLogStateRef();
    if (!state.enabled) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[96]{};
    swprintf_s(stamp, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%hs] ",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
               now.wMilliseconds, component);
    std::wstring line = stamp;
    line.append(message);
    line += L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), bytes,
                        nullptr, nullptr);
    std::lock_guard lock(state.mutex);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(state.path.c_str(), GetFileExInfoStandard, &info) &&
        ((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) >
            2ull * 1024ull * 1024ull) {
        const std::wstring previous = state.path + L".1";
        MoveFileExW(state.path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE file = CreateFileW(state.path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
}

} // namespace luling::diagnostics
