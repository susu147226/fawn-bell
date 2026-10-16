#include "temp_files.h"

#include <atomic>
#include <cwchar>
#include <iterator>

namespace luling::common {

const std::wstring& TempDirectory() {
    static const std::wstring directory = [] {
        wchar_t buffer[MAX_PATH * 2]{};
        const DWORD count = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
        return count > 0 && count < std::size(buffer) ? std::wstring(buffer, count) : std::wstring{};
    }();
    return directory;
}

std::wstring MakeTempPath(const std::wstring& prefix, const std::wstring& extension) {
    const std::wstring& directory = TempDirectory();
    if (directory.empty()) return {};
    static std::atomic<uint32_t> counter{ 0 };
    return directory + prefix + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(counter.fetch_add(1)) + extension;
}

bool DeleteTempFile(const std::wstring& path) {
    if (path.empty()) return false;
    const std::wstring& directory = TempDirectory();
    if (directory.empty()) return false;
    wchar_t full[MAX_PATH * 4]{};
    if (GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr) == 0)
        return false;
    // Refuse anything that does not resolve inside %TEMP%. This is the guard that
    // makes a wrong scratch path harmless.
    if (_wcsnicmp(full, directory.c_str(), directory.size()) != 0) return false;
    return DeleteFileW(path.c_str()) != FALSE;
}

} // namespace luling::common
