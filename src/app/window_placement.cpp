#include "window_placement.h"
#include <algorithm>

namespace luling::app {
namespace {

// A window stays usable while this much of its title bar sits inside a work
// area; below it the frame cannot be grabbed or un-maximized any more.
constexpr LONG kMinReachableWidth = 64;

struct Reachability {
    RECT window{};
    LONG reachable_width = 0;
};

BOOL CALLBACK MeasureWorkArea(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* reach = reinterpret_cast<Reachability*>(data);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    // Only the title strip matters: a window whose body pokes onto a monitor but
    // whose frame is off-screen still cannot be dragged back.
    RECT title = reach->window;
    title.bottom = (std::min)(title.bottom, title.top + kMinReachableWidth);
    RECT overlap{};
    if (!IntersectRect(&overlap, &title, &info.rcWork)) return TRUE;
    reach->reachable_width = (std::max)(reach->reachable_width, overlap.right - overlap.left);
    return TRUE;
}

} // namespace

bool ClampWindowRectToVisibleWorkArea(RECT& rect) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
    Reachability reach;
    reach.window = rect;
    EnumDisplayMonitors(nullptr, nullptr, MeasureWorkArea, reinterpret_cast<LPARAM>(&reach));
    if (reach.reachable_width >= kMinReachableWidth) return false;

    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info)) return false;
    const LONG work_w = info.rcWork.right - info.rcWork.left;
    const LONG work_h = info.rcWork.bottom - info.rcWork.top;
    const LONG width = (std::min)(rect.right - rect.left, work_w);
    const LONG height = (std::min)(rect.bottom - rect.top, work_h);
    rect.left = info.rcWork.left + (work_w - width) / 2;
    rect.top = info.rcWork.top + (work_h - height) / 2;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return true;
}

bool EnsureWindowVisibleOnMonitor(HWND hwnd) {
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd) || IsZoomed(hwnd)) return false;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return false;
    if (!ClampWindowRectToVisibleWorkArea(rect)) return false;
    SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

void ResetWindowPosition(HWND hwnd, float scale) {
    if (!hwnd) return;
    if (IsIconic(hwnd) || IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info))
        return;
    const LONG work_w = info.rcWork.right - info.rcWork.left;
    const LONG work_h = info.rcWork.bottom - info.rcWork.top;
    const LONG width = (std::min)(static_cast<LONG>(kDefaultWindowWidthDip * scale), work_w);
    const LONG height = (std::min)(static_cast<LONG>(kDefaultWindowHeightDip * scale), work_h);
    SetWindowPos(hwnd, nullptr, info.rcWork.left + (work_w - width) / 2,
                 info.rcWork.top + (work_h - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

RECT WorkspaceToScreenRect(const RECT& workspace, const RECT& monitor, const RECT& work) {
    RECT screen = workspace;
    OffsetRect(&screen, work.left - monitor.left, work.top - monitor.top);
    return screen;
}

RECT RestoredWindowRect(const RECT& workspace) {
    RECT bounds{}, work{};
    HMONITOR monitor = MonitorFromRect(&workspace, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return workspace;
    bounds = info.rcMonitor;
    work = info.rcWork;
    RECT screen = WorkspaceToScreenRect(workspace, bounds, work);
    // Near a monitor edge the unconverted rectangle can pick the neighbour;
    // the converted one names the monitor whose work area it belongs to.
    HMONITOR converted = MonitorFromRect(&screen, MONITOR_DEFAULTTONEAREST);
    if (converted != monitor && converted && GetMonitorInfoW(converted, &info))
        screen = WorkspaceToScreenRect(workspace, info.rcMonitor, info.rcWork);
    return screen;
}

RECT CascadeWindowRect(const RECT& opener, const RECT& work, int offset) {
    const LONG work_w = work.right - work.left;
    const LONG work_h = work.bottom - work.top;
    const bool has_work = work_w > 0 && work_h > 0;
    LONG w = opener.right - opener.left;
    LONG h = opener.bottom - opener.top;
    if (has_work) {
        w = (std::min)(w, work_w);
        h = (std::min)(h, work_h);
    }
    RECT r{opener.left + offset, opener.top + offset, 0, 0};
    if (has_work && (r.left < work.left || r.top < work.top ||
                     r.left + w > work.right || r.top + h > work.bottom)) {
        r.left = work.left;
        r.top = work.top;
    }
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

RECT MoveToWorkArea(const RECT& rect, const RECT& from_work, const RECT& to_work) {
    const LONG work_w = to_work.right - to_work.left;
    const LONG work_h = to_work.bottom - to_work.top;
    if (work_w <= 0 || work_h <= 0) return rect;
    const LONG w = (std::min)(rect.right - rect.left, work_w);
    const LONG h = (std::min)(rect.bottom - rect.top, work_h);
    LONG left = to_work.left + (rect.left - from_work.left);
    LONG top = to_work.top + (rect.top - from_work.top);
    left = (std::max)(to_work.left, (std::min)(left, to_work.right - w));
    top = (std::max)(to_work.top, (std::min)(top, to_work.bottom - h));
    return RECT{left, top, left + w, top + h};
}

RECT PlaceSessionWindow(const RECT& saved, bool on_monitor, const RECT& work,
                        const std::vector<RECT>& taken, int offset) {
    RECT r = saved;
    if (!on_monitor) {
        LONG w = saved.right - saved.left;
        LONG h = saved.bottom - saved.top;
        if (work.right > work.left && work.bottom > work.top) {
            w = (std::min)(w, work.right - work.left);
            h = (std::min)(h, work.bottom - work.top);
        }
        r = {work.left, work.top, work.left + w, work.top + h};
    }
    const auto clashes = [&taken](const RECT& c) {
        return std::any_of(taken.begin(), taken.end(),
                           [&c](const RECT& t) { return t.left == c.left && t.top == c.top; });
    };
    // Each step lands on a new position or wraps to the top-left, so
    // taken.size() + 1 steps are enough unless every position is taken.
    for (size_t step = 0; step <= taken.size() && clashes(r); ++step)
        r = CascadeWindowRect(r, work, offset);
    return r;
}

} // namespace luling::app
