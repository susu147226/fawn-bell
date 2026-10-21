// window_placement.h — keep the main window on a monitor that still exists.
//
// A saved session rect means nothing once the monitor it lived on is gone, and
// Windows restores the window to those coordinates anyway: off-screen, with no
// way back for a user who cannot see it (B26, §9.3).
#pragma once
#include <windows.h>
#include <vector>

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

// GetWindowPlacement reports rcNormalPosition in workspace coordinates: they
// are relative to the work area of the window's monitor, so a taskbar or
// another app bar docked at the top or left shifts them against screen
// coordinates. CreateWindowEx takes screen coordinates.
RECT WorkspaceToScreenRect(const RECT& workspace, const RECT& monitor, const RECT& work);

// Screen rectangle for a saved rcNormalPosition, using the monitor it lies on.
RECT RestoredWindowRect(const RECT& workspace);

// Where a new window opens next to its opener. `opener` is the opener's
// normal-state screen rectangle, `work` the work area of its monitor.
// `offset` pixels down and right; when that leaves the work area, the work
// area's top-left. The size is kept, shrunk to fit.
RECT CascadeWindowRect(const RECT& opener, const RECT& work, int offset);

// Where a folder opened from another monitor gets its new window: `rect`
// keeps its place relative to the work area's top-left on `to_work`, its size
// shrunk to fit and its position pulled back inside. Without a target work
// area, `rect`.
RECT MoveToWorkArea(const RECT& rect, const RECT& from_work, const RECT& to_work);

// Where a window of a restored session opens: `saved` is its screen
// rectangle, `on_monitor` whether that still overlaps a monitor, `work` the
// work area it opens in, `taken` the rectangles restored before it.
RECT PlaceSessionWindow(const RECT& saved, bool on_monitor, const RECT& work,
                        const std::vector<RECT>& taken, int offset);

} // namespace luling::app
