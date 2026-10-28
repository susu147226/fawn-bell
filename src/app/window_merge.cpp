// window_merge.cpp — merging windows into one, and handing pinned tabs of a
// closing window to the one that stays.
#include "window_merge.h"

#include "app_internal.h"
#include "app_messages.h"
#include "app_runtime.h"
#include "session.h"
#include "window_factory.h"
#include "window_placement.h"
#include "../common/localization.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace luling {
namespace {

struct MergedWindow {
    RECT rect{};
    bool maximized = false;
    app::WindowLayoutSnapshot layout;
    std::vector<app::LayoutTab*> tabs;
    std::vector<int> groups;  // each tab's group id in the source window
    std::vector<app::TabGroup> tab_groups;
    size_t active = 0;
};

// The last merge into this window, for its undo toast. One at a time: a
// second merge replaces it, as the toast does.
std::vector<MergedWindow> g_merge_undo;
HWND g_merge_owner = nullptr;

int IndexOf(const app::WindowTabs& tabs, const app::LayoutTab* tab) {
    for (size_t i = 0; i < tabs.items.size(); ++i)
        if (tabs.items[i].get() == tab) return static_cast<int>(i);
    return -1;
}

std::wstring TabFolder(const app::LayoutTab& tab) {
    const app::Tab* view = tab.ActiveFolder();
    return view ? view->current_path : std::wstring();
}

void Activate(AppState& s, const app::LayoutTab* tab) {
    const int index = IndexOf(s.window_tabs, tab);
    if (index < 0) return;
    RememberLayoutFocus(s);
    s.window_tabs.SwitchTab(static_cast<size_t>(index));
    BindCurrentLayout(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Everything the undo needs about the window that is about to lose its tabs.
void FillMergeEntry(AppState& src, MergedWindow& entry) {
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (GetWindowPlacement(src.hwnd, &placement)) {
        entry.rect = app::RestoredWindowRect(placement.rcNormalPosition);
        entry.maximized = placement.showCmd == SW_SHOWMAXIMIZED;
    }
    entry.layout = CaptureWindowLayout(src);
    entry.active = src.window_tabs.active;
    entry.tabs.clear();
    entry.groups.clear();
    entry.tab_groups.clear();
    for (const auto& tab : src.window_tabs.items) {
        entry.tabs.push_back(tab.get());
        entry.groups.push_back(tab->tab_group);
    }
    entry.tab_groups = src.window_tabs.tab_groups;
}

// Merges `src` into `dst`, hides and destroys it. The tabs must all leave:
// otherwise what moved goes back and the window stays.
bool MergeOne(AppState& src, AppState& dst, size_t insert_at, MergedWindow& entry) {
    if (&src == &dst || src.window_tabs.items.empty()) return false;
    FillMergeEntry(src, entry);
    const HWND hwnd = src.hwnd;
    ShowWindow(hwnd, SW_HIDE);  // it must not paint while its tabs leave
    MergeTabsInto(src, dst, insert_at);
    if (!src.window_tabs.items.empty()) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        return false;
    }
    DestroyWindow(hwnd);
    return true;
}

void ShowMergeToast(AppState& dst, size_t merged) {
    wchar_t title[160]{};
    if (merged == 1) {
        swprintf_s(title, L"%s", l10n::Get(l10n::StringId::WindowMerged).c_str());
    } else {
        swprintf_s(title, l10n::Get(l10n::StringId::WindowsMerged).c_str(), static_cast<int>(merged));
    }
    dst.notification_toast.Show(dst.hwnd, title, l10n::Get(l10n::StringId::Undo), true,
                                WM_UNDO_WINDOW_MERGE);
}

} // namespace

size_t MergeTabsInto(AppState& src, AppState& dst, size_t insert_at) {
    app::WindowTabs& from = src.window_tabs;
    app::WindowTabs& to = dst.window_tabs;
    app::LayoutTab* const active = from.Active();

    // Groups come along under ids of their own; runs stay contiguous because
    // InsertTab never lands inside one.
    std::vector<std::pair<int, int>> ids;
    for (const app::TabGroup& group : from.tab_groups) {
        app::TabGroup copy = group;
        copy.id = to.next_tab_group_id++;
        to.tab_groups.push_back(copy);
        ids.emplace_back(group.id, copy.id);
    }

    size_t pinned_at = 0;
    while (pinned_at < to.items.size() && to.items[pinned_at]->pinned) ++pinned_at;
    size_t loose_at = std::clamp(insert_at, pinned_at, to.items.size());
    pinned_at = (std::min)(insert_at, pinned_at);
    while (loose_at > 0 && loose_at < to.items.size() && to.items[loose_at - 1]->tab_group != 0 &&
           to.items[loose_at - 1]->tab_group == to.items[loose_at]->tab_group)
        ++loose_at;

    size_t moved = 0;
    while (!from.items.empty()) {
        const bool pinned = from.items.front()->pinned;
        const int group = from.items.front()->tab_group;
        std::unique_ptr<app::LayoutTab> tab = from.TakeTab(0, true);
        if (!tab) break;
        const size_t at = to.InsertTab(std::move(tab), pinned ? pinned_at : loose_at);
        int new_group = 0;
        for (const auto& [old_id, new_id] : ids)
            if (old_id == group) new_group = new_id;
        if (new_group != 0) to.items[at]->tab_group = new_group;
        if (pinned) ++pinned_at;
        else ++loose_at;
        ++moved;
    }
    std::erase_if(to.tab_groups, [&](const app::TabGroup& group) {
        const bool merged = std::any_of(ids.begin(), ids.end(),
                                        [&](const auto& id) { return id.second == group.id; });
        const bool used = std::any_of(to.items.begin(), to.items.end(),
                                      [&](const auto& tab) { return tab->tab_group == group.id; });
        return merged && !used;
    });
    app::NormalizeGroupRuns(to);
    if (active) Activate(dst, active);
    return moved;
}

size_t MergeWindowsInto(AppState& dst) {
    std::vector<HWND> others;
    dst.core.windows.ForEach([&](HWND hwnd, AppState&) {
        if (hwnd != dst.hwnd) others.push_back(hwnd);
    });
    std::vector<MergedWindow> batch;
    for (HWND hwnd : others) {
        AppState* src = dst.core.windows.Lookup(hwnd);
        if (!src || src == &dst || src->window_tabs.items.empty()) continue;
        MergedWindow entry;
        if (MergeOne(*src, dst, dst.window_tabs.items.size(), entry)) batch.push_back(std::move(entry));
    }

    const size_t merged = batch.size();
    if (merged == 0) return 0;
    g_merge_undo = std::move(batch);
    g_merge_owner = dst.hwnd;
    ShowMergeToast(dst, merged);
    BindCurrentLayout(dst);
    if (dst.hwnd) InvalidateRect(dst.hwnd, nullptr, FALSE);
    return merged;
}

bool MergeWindowInto(AppState& src, AppState& dst, size_t insert_at, const WINDOWPLACEMENT& before) {
    if (&src == &dst || src.window_tabs.items.empty()) return false;
    MergedWindow entry;
    if (!MergeOne(src, dst, insert_at, entry)) return false;
    // The undo reopens it where it was before the drag, not where it was dropped.
    if (before.length == sizeof(WINDOWPLACEMENT)) {
        entry.rect = app::RestoredWindowRect(before.rcNormalPosition);
        entry.maximized = before.showCmd == SW_SHOWMAXIMIZED;
    }
    g_merge_undo.clear();
    g_merge_undo.push_back(std::move(entry));
    g_merge_owner = dst.hwnd;
    ShowMergeToast(dst, 1);
    BindCurrentLayout(dst);
    SetForegroundWindow(dst.hwnd);
    if (dst.hwnd) InvalidateRect(dst.hwnd, nullptr, FALSE);
    return true;
}

size_t MovePinnedTabsInto(AppState& src, AppState& dst) {
    app::WindowTabs& from = src.window_tabs;
    app::WindowTabs& to = dst.window_tabs;
    const app::LayoutTab* const keep = to.Active();

    size_t pinned_at = 0;
    while (pinned_at < to.items.size() && to.items[pinned_at] && to.items[pinned_at]->pinned) ++pinned_at;
    size_t moved = 0;
    for (size_t i = 0; i < from.items.size();) {
        const app::LayoutTab* const tab = from.items[i].get();
        if (!tab || !tab->pinned) {
            ++i;
            continue;
        }
        const std::wstring path = TabFolder(*tab);
        const bool duplicate = !path.empty() &&
            std::any_of(to.items.begin(), to.items.begin() + static_cast<std::ptrdiff_t>(pinned_at),
                        [&](const auto& other) {
                            const std::wstring other_path = TabFolder(*other);
                            return CompareStringOrdinal(other_path.c_str(),
                                                        static_cast<int>(other_path.size()),
                                                        path.c_str(), static_cast<int>(path.size()),
                                                        TRUE) == CSTR_EQUAL;
                        });
        if (duplicate) {
            ++i;
            continue;
        }
        std::unique_ptr<app::LayoutTab> taken = from.TakeTab(i, true);
        if (!taken) {
            ++i;
            continue;
        }
        to.InsertTab(std::move(taken), pinned_at);
        ++pinned_at;
        ++moved;
    }
    if (moved) {
        app::NormalizeGroupRuns(to);
        if (keep) Activate(dst, keep);
        else BindCurrentLayout(dst);
    }
    return moved;
}

void UndoWindowMerge(AppState& dst) {
    if (g_merge_undo.empty() || g_merge_owner != dst.hwnd) return;
    std::vector<MergedWindow> batch = std::move(g_merge_undo);
    g_merge_undo.clear();
    g_merge_owner = nullptr;

    // Last merged first, so the stacking order comes back and the first
    // merged window ends up in front.
    HWND front = nullptr;
    for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
        const bool any_left = std::any_of(it->tabs.begin(), it->tabs.end(), [&](const app::LayoutTab* tab) {
            return IndexOf(dst.window_tabs, tab) >= 0;
        });
        if (!any_left) continue;

        WindowInit init;
        init.path = TabFolder(*it->tabs.front());
        if (it->rect.right > it->rect.left && it->rect.bottom > it->rect.top) init.rect = it->rect;
        const HWND hwnd = OpenLulingWindow(dst, init);
        if (!hwnd) continue;
        AppState* back = dst.core.windows.Lookup(hwnd);
        if (!back) continue;

        std::vector<std::pair<int, int>> ids;  // source group id -> its id here
        for (const app::TabGroup& group : it->tab_groups) {
            app::TabGroup copy = group;
            copy.id = back->window_tabs.next_tab_group_id++;
            back->window_tabs.tab_groups.push_back(copy);
            ids.emplace_back(group.id, copy.id);
        }
        size_t restored = 0;
        for (size_t k = 0; k < it->tabs.size(); ++k) {
            const int at = IndexOf(dst.window_tabs, it->tabs[k]);
            if (at < 0) continue;
            std::unique_ptr<app::LayoutTab> tab =
                dst.window_tabs.TakeTab(static_cast<size_t>(at), true);
            if (!tab) continue;
            const size_t pos = back->window_tabs.InsertTab(std::move(tab), back->window_tabs.items.size());
            int group = 0;
            for (const auto& [old_id, new_id] : ids)
                if (old_id == it->groups[k]) group = new_id;
            if (group != 0) back->window_tabs.items[pos]->tab_group = group;
            ++restored;
        }
        if (restored == 0) continue;
        std::erase_if(back->window_tabs.tab_groups, [&](const app::TabGroup& group) {
            return std::none_of(back->window_tabs.items.begin(), back->window_tabs.items.end(),
                                [&](const auto& tab) { return tab->tab_group == group.id; });
        });
        app::NormalizeGroupRuns(back->window_tabs);
        if (it->active < it->tabs.size()) {
            const int active = IndexOf(back->window_tabs, it->tabs[it->active]);
            if (active >= 0) back->window_tabs.SwitchTab(static_cast<size_t>(active));
        }
        ApplyWindowLayout(*back, it->layout);
        BindCurrentLayout(*back);
        if (it->maximized) ShowWindow(hwnd, SW_MAXIMIZE);
        front = hwnd;
    }
    BindCurrentLayout(dst);
    if (front) SetForegroundWindow(front);
    else if (dst.hwnd) InvalidateRect(dst.hwnd, nullptr, FALSE);
}

} // namespace luling
