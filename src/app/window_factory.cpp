// window_factory.cpp — see window_factory.h.
#include "window_factory.h"

#include "app_messages.h"
#include "app_state.h"
#include "single_instance_coordinator.h"
#include "window_placement.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace luling {
namespace {

// UI thread only.
std::vector<std::unique_ptr<AppState>> g_windows;
std::vector<std::unique_ptr<AppState>> g_retired;

// The layout a new window starts with: the opener's sidebar and details pane,
// and its theme. Process-wide preferences live in the shared core already.
void InheritWindowLayout(const AppState& from, AppState& to) {
    to.sidebarCollapsedMask = from.sidebarCollapsedMask;
    to.sidebarHiddenMask = from.sidebarHiddenMask;
    to.sidebarOrder = from.sidebarOrder;
    to.sidebarQuickAccessHiddenMask = from.sidebarQuickAccessHiddenMask;
    to.starredExpanded = from.starredExpanded;
    to.showDetailsPanel = from.showDetailsPanel;
    to.detailsPanelWidth = from.detailsPanelWidth;
    to.detailsPreviewOnly = from.detailsPreviewOnly;
    to.detailsPreviewEnabled = from.detailsPreviewEnabled;
    to.detailsPreviewExpansion = from.detailsPreviewExpansion;
    to.showFps = from.showFps;
    to.forceStatusPerformance = from.forceStatusPerformance;
    to.themeOverride = from.themeOverride;
    to.darkMode = from.darkMode;
}

// Normal-state screen rectangle of a window (empty when it has none).
RECT NormalScreenRect(HWND hwnd) {
    WINDOWPLACEMENT placement{sizeof(placement)};
    MONITORINFO monitor{sizeof(monitor)};
    if (!hwnd || !GetWindowPlacement(hwnd, &placement) ||
        !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
        return RECT{};
    return app::WorkspaceToScreenRect(placement.rcNormalPosition, monitor.rcMonitor, monitor.rcWork);
}

// Normal-state screen rectangle of `from`, cascaded by 32 DIP within its
// monitor's work area. A maximized opener does not pass its maximized state on.
RECT CascadedRect(const AppState& from) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(from.hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return RECT{};
    RECT normal{};
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (IsIconic(from.hwnd) || IsZoomed(from.hwnd)) {
        if (!GetWindowPlacement(from.hwnd, &placement)) return RECT{};
        normal = app::WorkspaceToScreenRect(placement.rcNormalPosition, monitor.rcMonitor, monitor.rcWork);
    } else if (!GetWindowRect(from.hwnd, &normal)) {
        return RECT{};
    }
    if (normal.right <= normal.left || normal.bottom <= normal.top) return RECT{};
    const int offset = static_cast<int>(std::lround(32.0f * (from.scale > 0.0f ? from.scale : 1.0f)));
    return app::CascadeWindowRect(normal, monitor.rcWork, offset);
}

} // namespace

HWND OpenLulingWindow(AppState& from, const WindowInit& init) {
    ReleaseRetiredWindowStates();
    auto owned = std::make_unique<AppState>(from.core);
    AppState* const state = owned.get();
    InheritWindowLayout(from, *state);
    state->has_window_start = true;
    state->window_start_path = init.path;
    state->window_adopt_from = init.adopt_from;
    state->window_adopt_index = init.adopt_index;
    g_windows.push_back(std::move(owned));

    RECT rect = init.rect ? *init.rect : CascadedRect(from);
    const HMONITOR from_monitor = MonitorFromWindow(from.hwnd, MONITOR_DEFAULTTONEAREST);
    if (!init.rect && init.monitor && init.monitor != from_monitor) {
        MONITORINFO there{sizeof(there)};
        MONITORINFO here{sizeof(here)};
        const RECT normal = NormalScreenRect(from.hwnd);
        if (normal.right > normal.left && GetMonitorInfoW(init.monitor, &there) && GetMonitorInfoW(from_monitor, &here))
            rect = app::MoveToWorkArea(normal, here.rcWork, there.rcWork);
    }
    const bool placed = rect.right > rect.left && rect.bottom > rect.top;
    const HWND hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP, app::SingleInstanceCoordinator::WindowClassName(), L"Luling", kMainWindowStyle,
        placed ? rect.left : CW_USEDEFAULT, placed ? rect.top : CW_USEDEFAULT,
        placed ? rect.right - rect.left : CW_USEDEFAULT, placed ? rect.bottom - rect.top : CW_USEDEFAULT,
        nullptr, nullptr, GetModuleHandleW(nullptr), state);
    if (!hwnd) {
        // A failed WM_CREATE still destroys the window, which retires the
        // state; otherwise it was never handed to a window at all.
        const auto it = std::find_if(g_windows.begin(), g_windows.end(),
                                     [state](const std::unique_ptr<AppState>& w) { return w.get() == state; });
        if (it != g_windows.end()) g_windows.erase(it);
        return nullptr;
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    return hwnd;
}

WindowLayoutSnapshot CaptureWindowLayout(const AppState& s) {
    WindowLayoutSnapshot layout;
    layout.sidebar_collapsed = static_cast<int>(s.sidebarCollapsedMask);
    layout.sidebar_hidden = static_cast<int>(s.sidebarHiddenMask);
    layout.sidebar_order = s.sidebarOrder;
    layout.quick_access_hidden = static_cast<int>(s.sidebarQuickAccessHiddenMask);
    layout.starred_expanded = s.starredExpanded;
    layout.details_panel = s.showDetailsPanel;
    layout.details_panel_width = static_cast<int>(std::lround(s.detailsPanelWidth));
    layout.details_preview_only = s.detailsPreviewOnly;
    layout.details_preview = s.detailsPreviewEnabled;
    return layout;
}

void ApplyWindowLayout(AppState& s, const WindowLayoutSnapshot& layout) {
    s.sidebarCollapsedMask = static_cast<uint32_t>(layout.sidebar_collapsed);
    s.sidebarHiddenMask = static_cast<uint32_t>(layout.sidebar_hidden);
    if (!layout.sidebar_order.empty()) s.sidebarOrder = layout.sidebar_order;
    s.sidebarQuickAccessHiddenMask = static_cast<uint32_t>(layout.quick_access_hidden);
    s.starredExpanded = layout.starred_expanded;
    s.showDetailsPanel = layout.details_panel;
    s.detailsPanelWidth = static_cast<float>(layout.details_panel_width);
    s.detailsPreviewOnly = layout.details_preview_only;
    s.detailsPreviewEnabled = layout.details_preview;
}

bool RetireWindowState(AppState& s) {
    const auto it = std::find_if(g_windows.begin(), g_windows.end(),
                                 [&s](const std::unique_ptr<AppState>& w) { return w.get() == &s; });
    if (it == g_windows.end()) return false;  // the WinMain-owned first window
    g_retired.push_back(std::move(*it));
    g_windows.erase(it);
    if (s.hwnd && s.core.host.Hwnd())
        PostMessageW(s.core.host.Hwnd(), WM_RELEASE_WINDOW_STATES, 0, 0);
    return true;
}

void ReleaseRetiredWindowStates() {
    g_retired.clear();
}

void ReleaseWindowStatesAtExit() {
    g_windows.clear();
    g_retired.clear();
}

} // namespace luling
