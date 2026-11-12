#pragma once
// Program icons Windows shows inside This PC (百度网盘, WPS网盘, ...): entries
// under Explorer\MyComputer\NameSpace whose CLSID comes from a program rather
// than Windows itself, or that point at a real folder. Read-only.

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace luling::app {

struct ThisPcLauncher {
    std::wstring clsid;  // "{...}"
    std::wstring label;  // what Explorer shows
    std::wstring target; // existing folder from TargetFolderPath, else empty
    std::uint8_t brand = 1; // 1 other, 2 Baidu, 3 WPS
};

std::vector<ThisPcLauncher> CurrentThisPcLaunchers();

// full_path of a launcher row: this prefix + its CLSID.
inline constexpr wchar_t kThisPcLauncherPrefix[] = L"shell:::";

} // namespace luling::app
