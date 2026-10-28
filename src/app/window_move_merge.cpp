// window_move_merge.cpp — a window dragged by its title bar onto another
// window's tab strip merges into it there (see window_move_merge.h).
#include "window_move_merge.h"

#include "app_internal.h"
#include "app_messages.h"
#include "app_runtime.h"
#include "window_merge.h"
#include "window_registry.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace luling {
namespace {

// A window being dragged must not hide the strip it is about to join.
constexpr BYTE kSeeThroughAlpha = 160;

struct MoveState {
    HWND moving = nullptr;
    HWND target = nullptr;
    int index = -1;
};

MoveState g_move;
// A caption press just happened: the move loop that follows may merge.
bool g_caption = false;
// Where the dragged window was before the drag: the merge undo opens it there.
WINDOWPLACEMENT g_before{sizeof(WINDOWPLACEMENT)};

AppState* WindowOf(AppState& any, HWND hwnd) {
    return hwnd ? any.core.windows.Lookup(hwnd) : nullptr;
}

void SetMovingAlpha(HWND hwnd, BYTE alpha) {
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool layered = (ex & WS_EX_LAYERED) != 0;
    if (alpha == 255) {
        if (layered) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
        return;
    }
    BYTE current = 255;
    DWORD flags = 0;
    if (layered && GetLayeredWindowAttributes(hwnd, nullptr, &current, &flags) && (flags & LWA_ALPHA) &&
        current == alpha)
        return;
    if (!layered) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
}

void ClearTarget(AppState& any) {
    if (AppState* target = WindowOf(any, g_move.target)) {
        if (target->windowDropIndex >= 0) {
            target->windowDropIndex = -1;
            InvalidateRect(target->hwnd, nullptr, FALSE);
        }
    }
    g_move.target = nullptr;
    g_move.index = -1;
}

} // namespace

// Where tabs released at `cursor` would land in `target`'s strip, or -1 when
// the cursor is not on that strip.
int TabStripDropIndex(AppState& target, POINT cursor) {
    ui::WindowViewModel vm = BuildVm(target, false);
    if (vm.tabs.empty()) return -1;
    const float width = static_cast<float>(target.compositor.Width());
    POINT client = cursor;
    ScreenToClient(target.hwnd, &client);
    const float scale = target.scale > 0.0f ? target.scale : 1.0f;
    const LONG slack = std::lround(12.0f * scale);
    const LONG strip_right = std::lround(width) - std::lround(3.0f * 46.0f * scale);
    LONG top = 0, bottom = 0, left = 0;
    bool any = false;
    int index = -1;
    for (int i = 0; i < static_cast<int>(vm.tabs.size()); ++i) {
        D2D1_RECT_F rc{};
        if (!target.renderer.TabItemRect(vm, width, i, &rc)) continue;
        if (!any) {
            top = std::lround(rc.top);
            bottom = std::lround(rc.bottom);
            left = std::lround(rc.left);
            any = true;
        } else {
            top = (std::min)(top, std::lround(rc.top));
            bottom = (std::max)(bottom, std::lround(rc.bottom));
        }
        if (client.x < std::lround((rc.left + rc.right) * 0.5f)) {
            index = i;
            break;
        }
    }
    if (!any) return -1;
    if (client.y < top - slack || client.y > bottom + slack || client.x < left - slack ||
        client.x > strip_right)
        return -1;
    return index < 0 ? static_cast<int>(vm.tabs.size()) : index;
}

void NoteTitleBarPress(AppState& s) {
    if (s.hwnd) g_caption = true;
}

void BeginWindowMove(AppState& s) {
    const bool caption = g_caption;
    g_caption = false;
    if (!caption || g_move.moving || !s.hwnd) return;
    if (s.core.windows.Count() < 2) return;  // nothing to merge with
    // A press on the frame resizes; the title bar is what a merge drag uses.
    POINT cursor{};
    RECT bounds{};
    GetCursorPos(&cursor);
    GetWindowRect(s.hwnd, &bounds);
    const int frame = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER) + 2;
    if (cursor.x <= bounds.left + frame || cursor.x >= bounds.right - frame ||
        cursor.y <= bounds.top + frame || cursor.y >= bounds.bottom - frame)
        return;
    g_move = MoveState{};
    g_move.moving = s.hwnd;
    g_before = WINDOWPLACEMENT{sizeof(WINDOWPLACEMENT)};
    GetWindowPlacement(s.hwnd, &g_before);
}

void TrackWindowMove(AppState& s) {
    if (g_move.moving != s.hwnd) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    AppState* found = nullptr;
    int index = -1;
    for (HWND below = GetWindow(s.hwnd, GW_HWNDNEXT); below; below = GetWindow(below, GW_HWNDNEXT)) {
        if (!IsWindowVisible(below) || IsIconic(below)) continue;
        RECT bounds{};
        if (!GetWindowRect(below, &bounds) || !PtInRect(&bounds, cursor)) continue;
        AppState* target = WindowOf(s, below);
        if (!target || target == &s) break;  // another program covers this spot
        found = target;
        index = TabStripDropIndex(*target, cursor);
        break;
    }
    const HWND next = found && index >= 0 ? found->hwnd : nullptr;
    if (g_move.target != next) {
        ClearTarget(s);
        g_move.target = next;
        g_move.index = index;
        if (found && index >= 0) {
            found->windowDropIndex = index;
            InvalidateRect(found->hwnd, nullptr, FALSE);
        }
    } else if (found && index >= 0 && found->windowDropIndex != index) {
        g_move.index = index;
        found->windowDropIndex = index;
        InvalidateRect(found->hwnd, nullptr, FALSE);
    }
    SetMovingAlpha(s.hwnd, next ? kSeeThroughAlpha : 255);
}

void EndWindowMove(AppState& s) {
    if (g_move.moving != s.hwnd) return;
    HWND target = g_move.target;
    const size_t index = g_move.index >= 0 ? static_cast<size_t>(g_move.index) : 0;
    // Esc during a system move puts the window back; do not merge then.
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) target = nullptr;
    if (AppState* t = WindowOf(s, g_move.target)) {
        t->windowDropIndex = -1;
        InvalidateRect(t->hwnd, nullptr, FALSE);
    }
    SetMovingAlpha(s.hwnd, 255);
    g_move = MoveState{};
    g_caption = false;
    // The move loop is still unwinding: merge once it has returned.
    if (target) PostMessageW(s.hwnd, WM_MERGE_WINDOW, reinterpret_cast<WPARAM>(target), static_cast<LPARAM>(index));
}

void FinishWindowMerge(AppState& s, HWND target, size_t index) {
    AppState* dst = s.core.windows.Lookup(target);
    if (!dst || dst == &s || !IsWindowVisible(target)) return;
    MergeWindowInto(s, *dst, index, g_before);
}

} // namespace luling
