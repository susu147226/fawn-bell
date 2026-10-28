// window_merge.h — merging windows into one, and keeping pinned tabs alive
// when one of several windows closes.
#pragma once
#include <windows.h>

#include <cstddef>

namespace luling {
struct AppState;

// Moves every tab of `src` into `dst`, in order, at `insert_at`: pinned tabs
// join the end of dst's pinned region, each group keeps its run under a new
// id, and src's active tab becomes dst's. Leaves `src` without tabs.
size_t MergeTabsInto(AppState& src, AppState& dst, size_t insert_at);

// "Merge all windows": moves the tabs of every other window into `dst`, in
// order, and closes those windows. Offers one undo in `dst`. Returns how many
// windows were merged.
size_t MergeWindowsInto(AppState& dst);

// Merges window `src` into `dst` at `insert_at`, closes `src` and offers the
// undo in `dst`. `before` is src's placement before it was dragged there.
// Returns false (and leaves both windows as they were) when a tab could not
// move. Touch nothing of `src` afterwards.
bool MergeWindowInto(AppState& src, AppState& dst, size_t insert_at, const WINDOWPLACEMENT& before);

// One of several windows closing: its pinned tabs move, in order, to the end
// of `dst`'s pinned tabs, except those showing a folder `dst` already pins.
// `dst` keeps its active tab. Returns how many moved.
size_t MovePinnedTabsInto(AppState& src, AppState& dst);

// WM_UNDO_WINDOW_MERGE: reopens the windows of the last merge into `dst` at
// their old places, with the tabs of theirs that are still in `dst`.
void UndoWindowMerge(AppState& dst);

} // namespace luling
