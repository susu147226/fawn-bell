#pragma once
#include <string>
#include <vector>
#include "../ui/fluent_menu.h"

namespace luling {
struct AppState;

// Address-bar path completion (Pulse 1.0.59 parity).
//
// The address drop-down is one popup for both jobs: it starts as the visit
// history and, as soon as the typed text differs from the text the editor
// opened with, its rows become path completion. Swapping the contents of the
// open popup instead of closing it and opening another is what keeps the menu's
// modal loop from nesting and stranding a window over the folder view.

// Rows for the completion half of that drop-down.
std::vector<ui::FluentMenuItem> AddressCompletionRows(AppState& s, const std::wstring& query);

// Runs a completion row. Returns true when the drop-down should reopen (a
// folder was accepted and its contents are next); sets `navigated` when the
// row left the address editor.
bool RunAddressCompletionRow(AppState& s, int command, bool& navigated);

// A keystroke in the address editor arms a short debounce and posts
// WM_ADDRESS_COMPLETE. It is only needed when no drop-down is showing; an open
// one rebuilds itself from the edit's text.
void RequestAddressCompletion(AppState& s);

// Runs when the posted message arrives: opens the completion list if nothing is
// on screen already.
void TickAddressCompletion(AppState& s);

// Address navigation that understands a file: an existing file navigates to its
// folder and is selected there, instead of being loaded as a folder and failing.
void NavigateToAddressTarget(AppState& s, const std::wstring& target);

} // namespace luling
