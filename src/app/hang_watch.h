// hang_watch.h — Diagnostic UI-stall sampler, enabled by --hang-watch.
// A background thread probes the main window with WM_NULL; when the UI thread
// stops answering for kStallMs it samples every thread's stack (suspend +
// RtlVirtualUnwind, no allocation while suspended) and appends the frames to
// a text log. luling.exe frames are written as RVAs (resolve with luling.map);
// system modules are resolved to their nearest export via dbghelp.
#pragma once
#include <windows.h>
#include <string>

namespace luling::app::hang {

void Start(HWND hwnd, const std::wstring& log_path);
void Stop();

} // namespace luling::app::hang
