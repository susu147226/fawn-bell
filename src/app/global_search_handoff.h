// global_search_handoff.h — Continue a quick search (Alt+Space popup) in the
// main window: same text, same name/content mode, same scope (#14).
#pragma once
#include <string>
#include "global_search_window.h"

namespace pulse {
struct AppState;

// pulse:search: path for the request, or empty when there is nothing to search.
std::wstring GlobalSearchHandoffPath(const GlobalSearchHandoff& request);
// Brings the main window forward. With a query, the results open in a new tab
// whose Back goes to the searched folder (or the new-tab location); without
// one, the toolbar search box opens instead, like Ctrl+K.
void ContinueGlobalSearchInMain(AppState& s, const GlobalSearchHandoff& request);
}
