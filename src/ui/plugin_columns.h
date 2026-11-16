#pragma once
// plugin_columns.h — values for the two optional plugin columns.
//
// The list asks Value() while building rows and calls RequestMissing() once
// per build; the host answers in batches and the next frame shows the text.
// Values are cached per path until the extension list changes.

#include <windows.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../plugin/plugin_protocol.h"

namespace luling::ui {

class PluginColumns {
public:
    static PluginColumns& Instance();

    // Re-reads the installed extension list on the next query.
    void Invalidate();

    bool Available(int index) const;
    std::wstring Title(int index) const;
    float Width(int index) const;

    // Cached text for one path (empty while unknown).
    std::wstring Value(const std::wstring& path, int index);
    // Leaf-name lookup for the sorter: DirEntry.full_path is empty for ordinary
    // folders, so the list worker only has the entry's name.
    std::wstring ValueByLeaf(const std::wstring& leaf, int index);

    // Asks the host for every path without a cached value. Repeated calls with
    // the same working set are throttled.
    void RequestMissing(const std::vector<std::wstring>& paths);

    // From WM_PLUGIN_COLUMNS_DONE. A failed batch is not cached, so the next
    // request retries it instead of freezing the column at empty text.
    void Store(int index, const std::vector<std::wstring>& paths,
               const std::vector<std::wstring>& values, bool ok);

private:
    struct ColumnInfo {
        bool available = false;
        std::wstring plugin_id;
        std::wstring column_id;
        std::wstring title;
        float width = 120.0f;
    };

    void EnsureLoaded() const;

    // The worker thread reads Value() while sorting, so every entry point locks.
    mutable std::mutex mutex_;
    mutable ColumnInfo columns_[2];
    mutable std::vector<plugin::PluginEntry> plugins_;
    mutable ULONGLONG last_load_ = 0;
    mutable bool loaded_ = false;
    mutable bool dirty_ = true;
    mutable std::unordered_map<std::wstring, std::wstring> values_[2];
    // Same values keyed by leaf name, for the sorter.
    mutable std::unordered_map<std::wstring, std::wstring> by_leaf_[2];
    mutable std::wstring last_request_key_[2];
    ULONGLONG last_request_at_[2] = {0, 0};
};

} // namespace luling::ui
