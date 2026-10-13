// app_prompts.h — Yes/no questions asked from app code, in the Luling confirm dialog.
#pragma once

#include <cstddef>

namespace luling {
struct AppState;

bool ConfirmClearRecent(AppState& s);
bool ConfirmClearDiagnostics(AppState& s);
// Privacy notice, then whether to include the index service. False = cancelled.
bool ConfirmDiagnosticsExport(AppState& s, bool& include_service);
// True = retry the recoverable operations, false = discard them.
bool AskRetryRecovery(AppState& s, size_t count, bool uncertain_destructive, bool duplicate_cleanup = false);
} // namespace luling
