// app_internal.h — Aggregate declarations for Luling window TUs.
#pragma once
#include "app_state.h"
#include "app_runtime.h"
#include "app_navigation.h"
#include "app_ops_ui.h"
#include "app_commands.h"
#include "app_hosted_edit.h"
#include "app_input.h"
#include "app_change_tracking.h"
#include "../common/crash_reporter.h"
namespace luling { void SuspendContentSearches(AppState& s); }

namespace luling::app {

// Message pumps get the same containment as WndProc: a C++ exception leaving a
// dispatched procedure has to cost one message, not the window (acceptance 20,
// B0). Not every procedure a message reaches is ours. SEH faults stay with
// WndProc's own __except, which catch(...) cannot see from here.
inline void DispatchGuarded(MSG& msg) {
    try {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    } catch (...) {
        luling::crash::AddBreadcrumb(2, msg.message, 0);
    }
}

} // namespace luling::app
