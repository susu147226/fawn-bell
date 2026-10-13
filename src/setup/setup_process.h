#pragma once
// Closing running Luling before files are replaced. Port of the
// "closing running Luling" section of installer/LulingSetup.iss.

#include <string>
#include <vector>

namespace luling::setup {

enum class CloseState { Done = 0, Failed = 1, Busy = 2 };

// Asks every Luling (window class LulingMainWindow) whose exe lives in `dirs` to
// exit with the Luling.PrepareUpdateShutdown.v1 handshake. Unattended updates
// never terminate a UI process that did not acknowledge; they keep waiting
// while it reports busy, exactly like the Inno script.
CloseState CloseLulingForUpdate(const std::vector<std::wstring>& dirs, bool unattended_update);

// Stops the LulingIndex service (when it runs from `dirs`) and ends
// Luling.Index.exe / Luling.Preview.exe / luling_shell.exe from `dirs`.
void StopLulingHosts(const std::vector<std::wstring>& dirs, bool touch_service);
bool WaitUntilImageGone(const wchar_t* image, const std::vector<std::wstring>& dirs, unsigned timeout_ms);

}  // namespace luling::setup
