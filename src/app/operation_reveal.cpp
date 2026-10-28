// operation_reveal.cpp — see operation_reveal.h.
#include "operation_reveal.h"

#include "app_internal.h"
#include "app_input.h"
#include "app_runtime.h"
#include "../fs/fs_enum.h"

#include <cwchar>
#include <utility>

namespace luling {
namespace {

std::wstring LeafOf(const std::wstring& path) {
    const size_t at = path.find_last_of(L"\\/");
    return at == std::wstring::npos ? path : path.substr(at + 1);
}

} // namespace

void QueueOperationReveal(AppState& s, const std::vector<std::wstring>& destinations) {
    const app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || fs::IsVirtualPath(tab->current_path)) return;
    std::vector<std::wstring> names;
    for (const auto& path : destinations) {
        if (path.empty()) continue;
        const std::wstring parent = fs::ParentPath(path);
        if (parent.empty() || _wcsicmp(parent.c_str(), tab->current_path.c_str()) != 0) continue;
        names.push_back(LeafOf(path));
    }
    if (names.empty()) return;
    s.reveal_names = std::move(names);
    s.reveal_folder = tab->current_path;
    s.reveal_until = GetTickCount64() + 5000;
    s.reveal_next_try = 0;
}

void TickOperationReveal(AppState& s, unsigned long long now) {
    if (s.reveal_names.empty()) return;
    if (now >= s.reveal_until) {
        s.reveal_names.clear();
        s.reveal_folder.clear();
        return;
    }
    if (now < s.reveal_next_try) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->loading || !tab->snapshot ||
        _wcsicmp(tab->current_path.c_str(), s.reveal_folder.c_str()) != 0) {
        s.reveal_next_try = now + 120;
        return;
    }
    std::vector<std::wstring> found;
    int first = -1;
    for (size_t i = 0; i < tab->EntryCount(); ++i) {
        if (!tab->EntryVisible(static_cast<int>(i))) continue;
        const std::wstring& name = tab->EntryAt(i).name;
        for (const auto& want : s.reveal_names) {
            if (_wcsicmp(name.c_str(), want.c_str()) != 0) continue;
            found.push_back(want);
            if (first < 0) first = static_cast<int>(i);
            break;
        }
    }
    if (found.empty()) {
        // The listing may still be the held one; the next tick tries again.
        s.reveal_next_try = now + 120;
        return;
    }
    tab->RemapSelection(found, found.front(), false);
    if (first >= 0) EnsureRowVisible(s, *tab, first);
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
    s.reveal_names.clear();
    s.reveal_folder.clear();
}

} // namespace luling
