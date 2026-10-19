#pragma once

namespace luling {
struct AppState;

// Starts a replacement process and exits this one. The replacement waits for the
// singleton mutex before taking over, so the window reappears without a second
// copy running beside it.
void RestartLuling(AppState& state);
}
