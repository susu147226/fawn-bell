#pragma once
#include <windows.h>

#include <vector>

namespace luling::app {

enum class WindowArrangeLayout {
    SideBySide,
    Stacked,
    Columns,
    Tile,
    Main,
    Cascade,
    MainRight,
    MainTop
};

// Rectangles for `count` windows inside `work`, in layout order. Empty when
// the work area cannot fit the minimum window size.
std::vector<RECT> CalculateWindowArrangement(WindowArrangeLayout layout, RECT work,
                                             size_t count, int gap, int min_width,
                                             int min_height, int cascade_step = 32,
                                             const std::vector<SIZE>& original_sizes = {});

} // namespace luling::app
