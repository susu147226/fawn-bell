#pragma once
#include <windows.h>

namespace luling::common {

// Milliseconds the machine has actually been awake. GetTickCount64() keeps
// advancing across sleep, so idle reclamation that treats a night of sleep as a
// night of inactivity tears every host down the moment the user comes back.
inline ULONGLONG AwakeMilliseconds() noexcept {
    ULONGLONG ticks = 0;
    QueryUnbiasedInterruptTime(&ticks);
    return ticks / 10000u;
}

} // namespace luling::common
