// operation_reveal.h — after a rename or a move into the folder on screen,
// the results are selected and scrolled into view.
#pragma once
#include <string>
#include <vector>

namespace luling {
struct AppState;

// Remembers the results that landed in the folder the active tab shows. The
// listing refresh is asynchronous, so the selection waits for the snapshot.
void QueueOperationReveal(AppState& s, const std::vector<std::wstring>& destinations);

// UI tick: once the refreshed listing has the results, select them and scroll
// the first one into view. Gives up after a few seconds.
void TickOperationReveal(AppState& s, unsigned long long now);

} // namespace luling
