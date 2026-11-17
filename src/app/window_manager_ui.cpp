#include "window_manager_ui.h"

#include "app_commands.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "app_state.h"
#include "single_instance_coordinator.h"
#include "window_arrange.h"
#include "window_factory.h"
#include "window_merge.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"

#include <algorithm>
#include <string>

namespace luling::app {
namespace {

BOOL CALLBACK CollectWindow(HWND hwnd, LPARAM param) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[128]{};
    if (!GetClassNameW(hwnd, cls, 128)) return TRUE;
    if (wcscmp(cls, SingleInstanceCoordinator::WindowClassName()) != 0) return TRUE;
    reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
    return TRUE;
}

RECT WorkAreaOf(HWND hwnd) {
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info))
        return info.rcWork;
    return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

void Place(HWND hwnd, const RECT& r) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring WindowName(HWND hwnd) {
    wchar_t title[256]{};
    if (GetWindowTextW(hwnd, title, 256) > 0) return title;
    return l10n::Get(l10n::StringId::AppName);
}

ui::FluentMenuItem Item(int command, std::wstring text, std::wstring glyph,
                        std::wstring shortcut = {}, bool enabled = true) {
    ui::FluentMenuItem item;
    item.command = command;
    item.text = std::move(text);
    item.glyph = std::move(glyph);
    item.shortcut = std::move(shortcut);
    item.enabled = enabled;
    return item;
}

const wchar_t* ArrangeLabel(int index) {
    switch (index) {
    case 0: return l10n::Pick(L"左右并排", L"Side by side");
    case 1: return l10n::Pick(L"上下堆叠", L"Stacked");
    case 2: return l10n::Pick(L"三列排列", L"Three columns");
    case 3: return l10n::Pick(L"平铺", L"Tile");
    case 4: return l10n::Pick(L"主次排列", L"Primary + secondary");
    case 5: return l10n::Pick(L"层叠", L"Cascade");
    case 6: return l10n::Pick(L"主窗在右", L"Primary on the right");
    case 7: return l10n::Pick(L"主窗在上", L"Primary on top");
    default: return l10n::Pick(L"合并窗口", L"Merge windows");
    }
}

void ArrangeGroup(const std::vector<HWND>& windows, HWND primary, const RECT& work,
                  WindowArrange kind, float scale) {
    if (windows.empty()) return;
    std::vector<HWND> order;
    if (primary && std::find(windows.begin(), windows.end(), primary) != windows.end())
        order.push_back(primary);
    for (HWND hwnd : windows)
        if (hwnd != primary) order.push_back(hwnd);
    std::vector<SIZE> sizes;
    sizes.reserve(order.size());
    for (HWND hwnd : order) {
        RECT r{};
        GetWindowRect(hwnd, &r);
        sizes.push_back({r.right - r.left, r.bottom - r.top});
    }
    const auto rects = CalculateWindowArrangement(
        static_cast<WindowArrangeLayout>(kind), work, order.size(),
        static_cast<int>(8.0f * scale), static_cast<int>(460.0f * scale),
        static_cast<int>(300.0f * scale), static_cast<int>(32.0f * scale), sizes);
    for (size_t i = 0; i < rects.size() && i < order.size(); ++i) Place(order[i], rects[i]);
}

} // namespace

std::vector<HWND> LulingWindows() {
    std::vector<HWND> windows;
    EnumWindows(CollectWindow, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

void ArrangeWindows(AppState& s, WindowArrange kind, bool all_monitors) {
    if (kind == WindowArrange::Merge) {
        MergeWindowsInto(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    const std::vector<HWND> all = LulingWindows();
    if (all.size() < 2) return;
    if (!all_monitors) {
        std::vector<HWND> group;
        const HMONITOR monitor = MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST);
        for (HWND hwnd : all)
            if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == monitor)
                group.push_back(hwnd);
        ArrangeGroup(group, s.hwnd, WorkAreaOf(s.hwnd), kind, s.scale);
        return;
    }
    std::vector<HMONITOR> monitors;
    for (HWND hwnd : all) {
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (std::find(monitors.begin(), monitors.end(), monitor) == monitors.end())
            monitors.push_back(monitor);
    }
    for (HMONITOR monitor : monitors) {
        std::vector<HWND> group;
        for (HWND hwnd : all)
            if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) == monitor)
                group.push_back(hwnd);
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfoW(monitor, &info))
            ArrangeGroup(group, group.empty() ? nullptr : group.front(), info.rcWork, kind,
                         s.scale);
    }
}

void OpenNewWindow(AppState& s) {
    WindowInit init;
    OpenWindowOrTab(s, init);
}

void ShowWindowManagerMenu(AppState& s) {
    if (!EnsureMenu(s)) return;
    const std::vector<HWND> windows = LulingWindows();
    const bool multi = windows.size() >= 2;
    std::vector<ui::FluentMenuItem> items;
    items.push_back(Item(CmdWindowNew, l10n::Pick(L"新建窗口", L"New window"),
                         L"\xE8A7", L"Ctrl+N"));
    items.push_back(Item(CmdWindowOverview, l10n::Pick(L"窗口总览", L"Window overview"),
                         L"\xE8A9"));
    std::wstring tab_name;
    if (const Tab* tab = ActiveTab(s)) {
        const size_t slash = tab->current_path.find_last_of(L"\\/");
        tab_name = slash == std::wstring::npos ? tab->current_path
                                               : tab->current_path.substr(slash + 1);
    }
    std::wstring move_text = l10n::Pick(L"标签页移到新窗口", L"Move tab to a new window");
    if (!tab_name.empty()) move_text += L" · " + tab_name;
    auto move_item = Item(CmdTabMoveToNewWindow, move_text, L"\xE8A7");
    move_item.separator_after = true;
    items.push_back(std::move(move_item));

    // Heading with the window / tab count, then the 3x3 arrangement tiles.
    ui::FluentMenuItem heading;
    heading.command = CmdNone;
    heading.heading = true;
    heading.text = l10n::Pick(L"窗口排列", L"Arrange windows");
    const size_t tabs = s.window_tabs.items.size();
    wchar_t count[96]{};
    swprintf_s(count, l10n::Pick(L"%u 个窗口 · %u 个标签页", L"%u windows · %u tabs"),
               static_cast<unsigned>(windows.size()), static_cast<unsigned>(tabs));
    heading.shortcut = count;
    heading.enabled = false;
    items.push_back(std::move(heading));
    for (int row = 0; row < 3; ++row) {
        ui::FluentMenuItem tile_row;
        tile_row.command = CmdNone;
        // The row itself is not clickable, but its tiles are: keep it enabled
        // so the swatch hit test and hover feedback run.
        tile_row.enabled = true;
        for (int col = 0; col < 3; ++col) {
            const int index = row * 3 + col;
            ui::FluentMenuSwatch tile;
            tile.command = CmdWindowArrangeBase + index;
            tile.label = ArrangeLabel(index);
            tile.layout_diagram = index;
            tile_row.quick_swatches.push_back(std::move(tile));
        }
        items.push_back(std::move(tile_row));
    }
    auto hint = Item(CmdNone,
        l10n::Pick(L"按住 Shift 排列所有显示器上的窗口",
                   L"Hold Shift to arrange windows on every monitor"),
        L"", {}, false);
    hint.heading = true;
    items.push_back(std::move(hint));
    if (!multi) {
        auto note = Item(CmdNone,
            l10n::Pick(L"至少打开两个窗口后再排列",
                       L"Open at least two windows to arrange them"),
            L"", {}, false);
        note.heading = true;
        items.push_back(std::move(note));
    }

    // Drop the menu from the title-bar button, right edges aligned.
    const float width = static_cast<float>(s.compositor.Width());
    const D2D1_RECT_F button = s.renderer.WindowManagerRect(width);
    POINT anchor{static_cast<LONG>(button.right),
                 static_cast<LONG>(button.bottom + 4.0f * s.scale)};
    ClientToScreen(s.hwnd, &anchor);
    RECT owner{};
    GetWindowRect(s.hwnd, &owner);
    anchor.x = (std::max)(owner.left + static_cast<LONG>(8.0f * s.scale),
                          anchor.x - static_cast<LONG>(330.0f * s.scale));
    const int command = s.menu->TrackPopup(anchor, std::move(items));
    if (command == CmdNone) return;
    if (command == CmdWindowNew) {
        OpenNewWindow(s);
    } else if (command == CmdWindowOverview) {
        ShowWindowOverview(s);
    } else if (command >= CmdWindowArrangeBase &&
               command < CmdWindowArrangeBase + static_cast<int>(WindowArrange::Count)) {
        const bool all_monitors = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        ArrangeWindows(s, static_cast<WindowArrange>(command - CmdWindowArrangeBase),
                       all_monitors);
    } else {
        DispatchMenuCommand(s, command);
    }
}

void ShowWindowOverview(AppState& s) {
    if (!EnsureMenu(s)) return;
    const std::vector<HWND> windows = LulingWindows();
    if (windows.empty()) return;
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < windows.size(); ++i) {
        auto item = Item(CmdWindowSwitchBase + static_cast<int>(i), WindowName(windows[i]),
                         L"", {}, true);
        if (windows[i] == s.hwnd) {
            item.radio = true;
            item.radio_group = true;
        }
        items.push_back(std::move(item));
    }
    // Drop the menu from the title-bar button, right edges aligned.
    const float width = static_cast<float>(s.compositor.Width());
    const D2D1_RECT_F button = s.renderer.WindowManagerRect(width);
    POINT anchor{static_cast<LONG>(button.right),
                 static_cast<LONG>(button.bottom + 4.0f * s.scale)};
    ClientToScreen(s.hwnd, &anchor);
    RECT owner{};
    GetWindowRect(s.hwnd, &owner);
    anchor.x = (std::max)(owner.left + static_cast<LONG>(8.0f * s.scale),
                          anchor.x - static_cast<LONG>(330.0f * s.scale));
    const int command = s.menu->TrackPopup(anchor, std::move(items));
    if (command < CmdWindowSwitchBase) return;
    const size_t index = static_cast<size_t>(command - CmdWindowSwitchBase);
    if (index >= windows.size()) return;
    if (IsIconic(windows[index])) ShowWindow(windows[index], SW_RESTORE);
    SetForegroundWindow(windows[index]);
}

} // namespace luling::app
