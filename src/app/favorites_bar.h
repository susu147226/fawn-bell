#pragma once
#include "app_runtime.h"

namespace luling {

// 收藏栏: the sidebar's 收藏夹 laid out as a row under the toolbar
// (Ctrl+Shift+B). The two are the same list and only one is shown at a time.
void ToggleFavoritesBar(AppState& s);

// Right-click on a favorites-bar slot: remove it or move it into a group.
void ShowFavoritesBarMenu(AppState& s, int index, POINT screen_pt);

} // namespace luling
