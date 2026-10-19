#include "app_restart.h"
#include "app_state.h"

#include <windows.h>
#include <string>

namespace luling {

void RestartLuling(AppState& state) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return;
    std::wstring command = L"\"" + std::wstring(path, length) + L"\" --restart";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process))
        return;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    // Release before the window goes away so the replacement can become primary
    // immediately instead of waiting out the mutex.
    state.single_instance.Release();
    DestroyWindow(state.hwnd);
}

} // namespace luling
