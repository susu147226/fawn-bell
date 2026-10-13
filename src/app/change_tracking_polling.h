#pragma once
#include <windows.h>

namespace luling::app {
inline bool ShouldPollChangeTracking(HWND window) {
    return window && IsWindowVisible(window) && !IsIconic(window);
}
}
