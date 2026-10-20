#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace luling::index {
// Opt-in, cross-process monotonic timestamps. No file IO in normal operation.
inline void TraceSearch(const char* stage, uint64_t revision = 0, std::wstring_view path = {}) {
    // Tracing is diagnostics only: never let it change the caller's GetLastError
    // (the first call reads an environment variable, which sets 203 on absence).
    const DWORD saved_error = GetLastError();
    struct RestoreError { DWORD error; ~RestoreError() { SetLastError(error); } } restore{saved_error};
    static const std::wstring destination = [] {
        std::wstring value(32768, L'\0');
        const DWORD n = GetEnvironmentVariableW(L"LULING_SEARCH_TRACE", value.data(), static_cast<DWORD>(value.size()));
        if (!n || n >= value.size()) return std::wstring{};
        value.resize(n); return value;
    }();
    if (destination.empty()) return;
    LARGE_INTEGER tick{}, frequency{}; QueryPerformanceCounter(&tick); QueryPerformanceFrequency(&frequency);
    std::wstring record = std::to_wstring(tick.QuadPart) + L"," + std::to_wstring(frequency.QuadPart) + L"," +
        std::to_wstring(GetCurrentProcessId()) + L",";
    for (const char* p = stage; *p; ++p) record += static_cast<wchar_t>(*p);
    record += L"," + std::to_wstring(revision) + L",\"";
    for (wchar_t c : path) { if (c == L'"') record += L'"'; record += c == L'\n' || c == L'\r' ? L' ' : c; }
    record += L"\"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, record.data(), static_cast<int>(record.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, record.data(), static_cast<int>(record.size()), utf8.data(), bytes, nullptr, nullptr);
    HANDLE file = CreateFileW(destination.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) { DWORD written = 0; WriteFile(file, utf8.data(), bytes, &written, nullptr); CloseHandle(file); }
}
}
