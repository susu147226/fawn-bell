#pragma once
// Window management: the title-bar button, its menu, window arrangement and
// the window switcher. Only reachable when single-window mode is off.

#include <windows.h>
#include <vector>

namespace luling {
struct AppState;
}

namespace luling::app {

enum class WindowArrange {
    SideBySide = 0,   // 左右并排
    Stacked,          // 上下堆叠
    ThreeColumns,     // 三列排列
    Tile,             // 平铺
    PrimarySecondary, // 主次排列
    Cascade,          // 层叠
    PrimaryRight,     // 主窗在右
    PrimaryTop,       // 主窗在上
    Merge,            // 合并窗口
    Count
};

// Every visible top-level Luling window, in z order.
std::vector<HWND> LulingWindows();

// Arranges the windows on the monitor of `s` (or, with `all_monitors`, the
// windows of each monitor inside that monitor's work area).
void ArrangeWindows(AppState& s, WindowArrange kind, bool all_monitors);

// The title-bar menu: new window, overview, move tab, arrangements.
void ShowWindowManagerMenu(AppState& s);
// A flat list of open windows to switch to.
void ShowWindowOverview(AppState& s);
void OpenNewWindow(AppState& s);

} // namespace luling::app
