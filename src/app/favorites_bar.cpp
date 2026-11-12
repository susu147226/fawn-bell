#include "favorites_bar.h"
#include "app_commands.h"
#include "../common/localization.h"
#include "../ui/fluent_menu.h"

#include <algorithm>

namespace luling {

void ToggleFavoritesBar(AppState& s) {
    s.appPrefs.favorites_bar = !s.appPrefs.favorites_bar;
    s.renderer.SetFavoritesBar(s.appPrefs.favorites_bar);
    s.favoritesFocus = -1;
    if (!s.isolatedTest) s.appPrefs.Save();
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowFavoritesBarMenu(AppState& s, int index, POINT screen_pt) {
    if (index < 0 || index >= static_cast<int>(s.places.starred_items.size())) return;
    if (!EnsureMenu(s)) return;
    const app::StarredItem item = s.places.starred_items[static_cast<size_t>(index)];

    std::vector<ui::FluentMenuItem> items;
    ui::FluentMenuItem remove;
    remove.command = 1;
    remove.text = l10n::Pick(L"移出收藏夹", L"Remove from favorites");
    items.push_back(std::move(remove));
    ui::FluentMenuItem group;
    group.command = 2;
    group.text = l10n::Pick(L"移到分组…", L"Move to group…");
    items.push_back(std::move(group));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == 1) {
        ToggleStarred(s, item.path, item.kind);
        return;
    }
    if (command != 2) return;

    // Group names live on the starred items themselves; the menu doubles as
    // the name field, so no separate input dialog is needed.
    std::vector<std::wstring> groups;
    for (const auto& entry : s.places.starred_items) {
        if (entry.group.empty()) continue;
        if (std::find(groups.begin(), groups.end(), entry.group) == groups.end())
            groups.push_back(entry.group);
    }
    std::wstring current = item.group;
    s.menu->SetFilterPlaceholder(l10n::Pick(L"分组名称", L"Group name"));
    s.menu->SetInitialFilterText(item.group);
    s.menu->SetFilterMinWidth(220.0f);
    auto build = [&](const std::wstring& query) {
        current = query;
        std::vector<ui::FluentMenuItem> list;
        ui::FluentMenuItem apply;
        apply.command = 10;
        apply.text = query.empty()
            ? l10n::Pick(L"不分组", L"No group")
            : std::wstring(l10n::Pick(L"使用「", L"Use \"")) + query + L"」";
        apply.separator_after = !groups.empty();
        list.push_back(std::move(apply));
        for (size_t i = 0; i < groups.size(); ++i) {
            ui::FluentMenuItem entry;
            entry.command = 20 + static_cast<int>(i);
            entry.text = groups[i];
            entry.checked = groups[i] == item.group;
            list.push_back(std::move(entry));
        }
        return list;
    };
    const int pick = s.menu->TrackPopup(screen_pt, build(item.group),
        [&](const std::wstring& query) { return build(query); });
    std::wstring next;
    if (pick == 10) next = current;
    else if (pick >= 20 && pick < 20 + static_cast<int>(groups.size()))
        next = groups[static_cast<size_t>(pick - 20)];
    else return;
    if (next == item.group) return;
    if (s.places.SetStarredGroup(item.path, next)) {
        RefreshStarredViews(s);
        InvalidateRect(s.hwnd, nullptr, FALSE);
    }
}

} // namespace luling
