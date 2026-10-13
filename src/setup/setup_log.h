#pragma once
// Append-only UTF-8 setup log (%TEMP%\Luling-Setup-<timestamp>.log by default).

#include <string>

namespace luling::setup {

void LogOpen(const std::wstring& path);
const std::wstring& LogPath();
void Log(const std::wstring& line);

}  // namespace luling::setup
