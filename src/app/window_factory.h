// window_factory.h — opens further Luling windows on the shared core.
//
// The first window is created by WinMain and owns the AppCore (its AppState
// lives until the process exits). Every later window gets an AppState bound to
// that core, owned here: WM_CREATE skips the process-level setup for it, and
// after WM_NCDESTROY the state is retired and freed on the host's next
// message, never inside the window's own message handling.
#pragma once
#include <windows.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "session.h"

namespace luling {

struct AppState;

// Style of every Luling main window: WS_CAPTION without WS_SYSMENU keeps DWM's
// minimize / maximize animations without its duplicate caption buttons.
constexpr DWORD kMainWindowStyle =
    WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;

struct WindowInit {
    std::wstring path;  // folder of the first tab ("" = This PC)
    // A window opened at a specific place instead of cascading next to its
    // opener (a tab dragged off the strip).
    std::optional<RECT> rect;
    // A folder opened from outside Luling: the monitor the request came from.
    HMONITOR monitor = nullptr;
    // A tab that moves in alive instead of a first tab opening.
    AppState* adopt_from = nullptr;
    size_t adopt_index = 0;
    // A window reopened from the session: its tabs, layout and place.
    const app::WindowSessionSnapshot* restore = nullptr;
};

// Creates and shows a window on `from`'s core at `init.path`, next to `from`
// and with its per-window layout (sidebar, details pane). Returns the new
// window, or nullptr when it could not be created.
HWND OpenLulingWindow(AppState& from, const WindowInit& init);

// A window's sidebar and details pane, for the session.
app::WindowLayoutSnapshot CaptureWindowLayout(const AppState& s);
void ApplyWindowLayout(AppState& s, const app::WindowLayoutSnapshot& layout);

// WM_NCDESTROY: hands the state of a window opened by OpenLulingWindow over
// for release. False for the first window, whose state WinMain owns.
bool RetireWindowState(AppState& s);
// Frees retired states (the host's WM_RELEASE_WINDOW_STATES, and WinMain
// before the core goes away).
void ReleaseRetiredWindowStates();
// WinMain after the message loop: destroys windows still open (an abnormal
// quit) and frees every state before the core goes away.
void ReleaseWindowStatesAtExit();

} // namespace luling
