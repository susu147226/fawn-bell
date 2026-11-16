#include "plugin_columns.h"

#include "../app/extension_manager.h"
#include "../plugin/plugin_client.h"

#include <algorithm>

namespace luling::ui {

PluginColumns& PluginColumns::Instance() {
    static PluginColumns instance;
    return instance;
}

void PluginColumns::Invalidate() { dirty_ = true; }

void PluginColumns::EnsureLoaded() const {
    const ULONGLONG now = GetTickCount64();
    if (loaded_ && !dirty_ && now - last_load_ < 2000) return;
    dirty_ = false;
    loaded_ = true;
    last_load_ = now;
    ColumnInfo next[2];
    std::vector<plugin::PluginEntry> plugins;
    int slot = 0;
    for (const app::InstalledExtension& ext : app::LoadInstalledExtensions()) {
        if (!ext.is_plugin() || !ext.enabled || !ext.granted || ext.columns.empty()) continue;
        plugins.push_back({ext.id, ext.dir});
        for (const plugin::PluginColumnSpec& spec : ext.columns) {
            if (slot >= 2) break;
            ColumnInfo& info = next[slot];
            info.available = true;
            info.plugin_id = ext.id;
            info.column_id = spec.id;
            info.title = spec.title.empty() ? ext.name : spec.title;
            info.width = static_cast<float>(spec.width > 0 ? spec.width : 120);
            ++slot;
        }
        if (slot >= 2) break;
    }
    for (int i = 0; i < 2; ++i) {
        if (columns_[i].plugin_id != next[i].plugin_id ||
            columns_[i].column_id != next[i].column_id) {
            values_[i].clear();
            by_leaf_[i].clear();
            last_request_key_[i].clear();
        }
        columns_[i] = next[i];
    }
    plugins_ = std::move(plugins);
}

bool PluginColumns::Available(int index) const {
    if (index < 0 || index > 1) return false;
    std::lock_guard lock(mutex_);
    EnsureLoaded();
    return columns_[index].available;
}

std::wstring PluginColumns::Title(int index) const {
    if (index < 0 || index > 1) return {};
    std::lock_guard lock(mutex_);
    EnsureLoaded();
    return columns_[index].title;
}

float PluginColumns::Width(int index) const {
    if (index < 0 || index > 1) return 120.0f;
    std::lock_guard lock(mutex_);
    EnsureLoaded();
    return columns_[index].width > 0.0f ? columns_[index].width : 120.0f;
}

std::wstring PluginColumns::Value(const std::wstring& path, int index) {
    if (index < 0 || index > 1) return {};
    std::lock_guard lock(mutex_);
    EnsureLoaded();
    if (!columns_[index].available) return {};
    const auto it = values_[index].find(path);
    return it == values_[index].end() ? std::wstring() : it->second;
}

std::wstring PluginColumns::ValueByLeaf(const std::wstring& leaf, int index) {
    if (index < 0 || index > 1 || leaf.empty()) return {};
    std::lock_guard lock(mutex_);
    EnsureLoaded();
    if (!columns_[index].available) return {};
    const auto it = by_leaf_[index].find(leaf);
    return it == by_leaf_[index].end() ? std::wstring() : it->second;
}

void PluginColumns::RequestMissing(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    std::lock_guard lock(mutex_);
    EnsureLoaded();
    for (int i = 0; i < 2; ++i) {
        const ColumnInfo& info = columns_[i];
        if (!info.available) continue;
        std::vector<std::wstring> missing;
        for (const std::wstring& path : paths) {
            if (path.empty()) continue;
            if (values_[i].find(path) == values_[i].end()) missing.push_back(path);
            if (missing.size() >= 300) break;
        }
        if (missing.empty()) continue;
        std::wstring key;
        for (const std::wstring& path : missing) {
            key += path;
            key.push_back(L'\x1E');
        }
        const ULONGLONG now = GetTickCount64();
        if (last_request_key_[i] == key && now - last_request_at_[i] < 5000) continue;
        last_request_key_[i] = key;
        last_request_at_[i] = now;
        app::PluginClient::Instance().Columns(i, info.plugin_id, info.column_id,
                                              std::move(missing), plugins_);
    }
}

void PluginColumns::Store(int index, const std::vector<std::wstring>& paths,
                          const std::vector<std::wstring>& values, bool ok) {
    if (index < 0 || index > 1 || !ok) return;
    std::lock_guard lock(mutex_);
    auto& map = values_[index];
    auto& leaf_map = by_leaf_[index];
    const size_t count = (std::min)(paths.size(), values.size());
    for (size_t i = 0; i < count; ++i) {
        map[paths[i]] = values[i];
        const size_t slash = paths[i].find_last_of(L"\\/");
        const std::wstring leaf =
            slash == std::wstring::npos ? paths[i] : paths[i].substr(slash + 1);
        if (!leaf.empty()) leaf_map[leaf] = values[i];
    }
    if (map.size() > 5000) {
        map.clear();
        leaf_map.clear();
    }
}

} // namespace luling::ui
