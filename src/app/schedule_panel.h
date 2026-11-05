// schedule_panel.h — modal list/create panel for the title-bar clock card.
#pragma once
#include <windows.h>
#include "schedule_store.h"

namespace luling::app {

// Shows the schedule panel next to the clock card and blocks until it closes.
// The caller keeps ownership of the store; every change is saved immediately.
void ShowSchedulePanel(HWND owner, ScheduleStore& store, float scale, bool dark,
                       POINT screen_anchor);

} // namespace luling::app
