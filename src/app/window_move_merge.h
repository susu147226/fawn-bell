// window_move_merge.h — dragging a whole window by its title bar onto another
// window's tab strip merges it there.
#pragma once
#include <windows.h>
#include <cstddef>

namespace luling {
struct AppState;

// WM_NCLBUTTONDOWN with HTCAPTION: a title-bar drag may start. Resizing by the
// frame must never merge, so only a caption press arms the drop.
void NoteTitleBarPress(AppState& s);

// Where a tab or a window released at `cursor` (screen px) would land in
// `target`'s tab strip, or -1 when the cursor is not on that strip.
int TabStripDropIndex(AppState& target, POINT cursor);

// WM_ENTERSIZEMOVE: a title-bar drag started. Nothing when there is only one
// window to merge with.
void BeginWindowMove(AppState& s);
// WM_MOVING: marks the tab strip under the cursor and fades the dragged
// window so the target stays readable.
void TrackWindowMove(AppState& s);
// WM_EXITSIZEMOVE: released over a strip merges once the move loop returns;
// anywhere else (or after Esc) just clears the marks.
void EndWindowMove(AppState& s);
// WM_MERGE_WINDOW: the move loop has ended; merge `s` into `target` with the
// undo toast. `s` may be gone afterwards.
void FinishWindowMerge(AppState& s, HWND target, size_t index);

} // namespace luling
