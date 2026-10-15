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

} // namespace luling::app
