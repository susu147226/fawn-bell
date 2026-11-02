#pragma once
#include "ui_compositor.h"

#include <string>
#include <vector>

namespace luling::ui {
// Settings > 快捷键 (page 13): the same groups the help dialog lists.
struct ShortcutRowView {
    std::wstring action;
    std::wstring keys;
    // app::ShortcutCommand that can be rebound, or -1 for a read-only row.
    int command = -1;
};
struct ShortcutGroupView {
    std::wstring title;
    std::vector<ShortcutRowView> rows;
};
std::vector<ShortcutGroupView> ShortcutGroupsForSettings();

struct ShortcutHelpLayout {
    D2D1_RECT_F card{}, close{}, body{};
    float content_height = 0;
    float max_scroll = 0;
    bool narrow = false;
};
// Pass the live groups (settings page model) so a rebound key shows here too;
// null draws the stock list.
ShortcutHelpLayout LayoutShortcutHelp(float width, float height, float scale,
                                      const std::vector<ShortcutGroupView>* groups = nullptr);
void DrawShortcutHelp(Compositor& compositor, bool dark, D2D1_COLOR_F accent,
                      float scale, float scroll, bool close_hover = false,
                      const std::vector<ShortcutGroupView>* groups = nullptr);
void ShowShortcutHelp(HWND owner, bool dark, D2D1_COLOR_F accent,
                      const std::vector<ShortcutGroupView>* groups = nullptr);
}
