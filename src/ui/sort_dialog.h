#pragma once
// sort_dialog.h — 排序 panel (the toolbar's Sort button), laid out like
// Pulse's: the sort keys on the left, a sample list and the direction /
// folder-order segments on the right, 取消 / 应用 at the bottom.

#include <d2d1.h>
#include <windows.h>

namespace luling::ui {

struct SortDialogSpec {
    int column = 0;      // ui::SortColumn value
    int direction = 0;   // 0 ascending, 1 descending
    int folder_mode = 0; // app::FolderSortMode value
};

// Opens the panel under the Sort button at `anchor` (screen pixels). Returns
// true when the user pressed 应用; `spec` then holds the choices.
bool ShowSortDialog(HWND owner, POINT anchor, SortDialogSpec& spec, bool dark,
                    D2D1_COLOR_F accent);

} // namespace luling::ui
