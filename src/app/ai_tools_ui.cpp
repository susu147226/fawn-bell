// ai_tools_ui.cpp - The tools that read AppState, so they run on the UI
// thread (ai_tools.h RunToolOnUi).
#include "ai_tools.h"

#include "app_internal.h"
#include "app_runtime.h"

#include <windows.h>

#include <string>

namespace luling::app::ai {

json::Value StateSnapshot(AppState& s) {
    json::Value out = json::Value::Object();
    app::Tab* tab = ActiveTab(s);
    const std::wstring folder = tab ? tab->current_path : std::wstring();
    out.set("folder", json::Utf8(folder));
    out.set("folder_name", json::Utf8(folder.empty() ? std::wstring() : app::TabTitle(folder)));
    const size_t count = tab ? tab->EntryCount() : 0;
    out.set("items", static_cast<int64_t>(count));
    json::Value selection = json::Value::Array();
    if (tab && tab->snapshot) {
        auto add = [&](int index) {
            if (index < 0 || index >= static_cast<int>(tab->snapshot->size())) return;
            selection.push(json::Utf8((*tab->snapshot)[static_cast<size_t>(index)].name));
        };
        if (tab->all_selected) {
            for (size_t i = 0; i < tab->snapshot->size() && i < 50; ++i)
                add(static_cast<int>(i));
        } else {
            for (const int index : tab->selected) add(index);
            if (tab->selected.empty()) add(tab->selected_index);
        }
    }
    out.set("selected_count", static_cast<int64_t>(selection.size()));
    out.set("selection", std::move(selection));
    const char* view = "details";
    if (tab) {
        switch (tab->view_mode) {
        case ui::ViewMode::List: view = "list"; break;
        case ui::ViewMode::Tiles: view = "tiles"; break;
        case ui::ViewMode::Content: view = "content"; break;
        case ui::ViewMode::ExtraLargeIcons: view = "extra_large_icons"; break;
        case ui::ViewMode::LargeIcons: view = "large_icons"; break;
        case ui::ViewMode::MediumIcons: view = "medium_icons"; break;
        case ui::ViewMode::SmallIcons: view = "small_icons"; break;
        default: view = "details"; break;
        }
    }
    out.set("view", view);
    if (tab) {
        const char* sort = "name";
        switch (tab->sort_column) {
        case ui::SortColumn::Mtime: sort = "modified"; break;
        case ui::SortColumn::Type: sort = "type"; break;
        case ui::SortColumn::Size: sort = "size"; break;
        case ui::SortColumn::Path: sort = "path"; break;
        case ui::SortColumn::Created: sort = "created"; break;
        case ui::SortColumn::Accessed: sort = "accessed"; break;
        default: sort = "name"; break;
        }
        out.set("sort", sort);
        out.set("sort_desc", tab->sort_direction == ui::SortDirection::Desc);
    }
    return out;
}

ToolOutcome RunToolOnUi(AppState& s, const std::string& name, const json::Value&) {
    ToolOutcome out;
    if (name == "get_state") {
        out.data = StateSnapshot(s);
        out.ok = true;
        return out;
    }
    out.code = "unknown_tool";
    out.message = "This version does not have that tool.";
    return out;
}

} // namespace luling::app::ai
