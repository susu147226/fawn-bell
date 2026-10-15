// window_placement.h — keep the main window on a monitor that still exists.
//
// A saved session rect means nothing once the monitor it lived on is gone, and
// Windows restores the window to those coordinates anyway: off-screen, with no
// way back for a user who cannot see it (B26, §9.3).
#pragma once
#include <windows.h>

namespace luling::app {

// Launch size of the main window, in DIPs.
inline constexpr float kDefaultWindowWidthDip = 1600.0f;
inline constexpr float kDefaultWindowHeightDip = 960.0f;

// Moves `rect` onto the nearest monitor's work area when too little of it stays
// reachable, shrinking it to fit a smaller monitor. Returns true when the rect
// changed. Callers holding a window pass the rect from GetWindowRect.
bool ClampWindowRectToVisibleWorkArea(RECT& rect);

// Same, applied to a live window. Iconic and maximized windows are left alone:
// Windows already re-homes those on a topology change. Returns true when moved.
bool EnsureWindowVisibleOnMonitor(HWND hwnd);

// Restores, un-maximizes and re-centres the window at the launch size on the
// primary monitor — the way back for a window stranded on a removed display.
void ResetWindowPosition(HWND hwnd, float scale);

} // namespace luling::app
