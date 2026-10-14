// location_history.h — Folders the user has visited, newest first, for the
// address bar's history dropdown. The search history is a separate list.
#pragma once
#include <string>
#include <vector>

namespace luling::app {

struct LocationHistory {
    bool persist = true;
    std::vector<std::wstring> entries;

    bool Load();
    bool Save() const;
    // True when the list changed: re-visiting the newest folder is a no-op.
    bool Record(const std::wstring& path);
    bool Clear();
    std::wstring ToJson() const;
    bool FromJson(const std::wstring& json);
};

} // namespace luling::app
