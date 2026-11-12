// app_tree.cpp — Inline file tree row construction (app_tree.h).
#include "app_tree.h"
#include "app_messages.h"
#include "entry_sort.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace luling::app {

namespace {
std::atomic_bool g_tree_enabled{false};

struct TreeLoadResult {
    std::wstring path;
    std::vector<fs::DirEntry> entries;
    bool ok = false;
    uint64_t stamp = 0;
};

struct TreeStampResult {
    std::vector<std::wstring> changed;
};

std::mutex g_tree_load_mutex;
std::unordered_map<UINT_PTR, std::shared_ptr<TreeLoadResult>> g_tree_loads;
std::unordered_map<UINT_PTR, std::shared_ptr<TreeStampResult>> g_tree_stamps;
std::atomic<UINT_PTR> g_tree_token{1};

// Directory write time; 0 when it cannot be read.
uint64_t DirectoryStamp(const std::wstring& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
           data.ftLastWriteTime.dwLowDateTime;
}
}  // namespace

void SetTreeViewEnabled(bool on) noexcept {
    g_tree_enabled.store(on, std::memory_order_relaxed);
}

bool TreeViewEnabled() noexcept {
    return g_tree_enabled.load(std::memory_order_relaxed);
}

UINT_PTR BeginTreeChildrenLoad(HWND notify, const std::wstring& path, ui::SortColumn column,
                               ui::SortDirection direction, bool show_hidden,
                               bool show_protected) {
    const UINT_PTR token = g_tree_token.fetch_add(1, std::memory_order_relaxed);
    auto result = std::make_shared<TreeLoadResult>();
    result->path = path;
    {
        std::lock_guard lock(g_tree_load_mutex);
        g_tree_loads[token] = result;
    }
    std::thread([notify, token, result, column, direction, show_hidden, show_protected] {
        std::vector<fs::DirEntry> entries;
        try {
            fs::EnumerateDirectory(result->path, entries);
            const auto allows = [show_hidden, show_protected](DWORD attrs) {
                if ((attrs & FILE_ATTRIBUTE_HIDDEN) == 0) return true;
                if (!show_hidden) return false;
                return (attrs & FILE_ATTRIBUTE_SYSTEM) == 0 || show_protected;
            };
            std::erase_if(entries, [&](const fs::DirEntry& entry) { return !allows(entry.attrs); });
            SortEntries(entries, column, direction);
            result->entries = std::move(entries);
            result->ok = true;
        } catch (...) {
            result->ok = false;
        }
        result->stamp = DirectoryStamp(result->path);
        if (notify) PostMessageW(notify, WM_TREE_CHILDREN, token, 0);
    }).detach();
    return token;
}

std::shared_ptr<const std::vector<fs::DirEntry>> TakeTreeChildrenLoad(UINT_PTR token,
                                                                     std::wstring& path,
                                                                     bool& ok,
                                                                     uint64_t& stamp) {
    std::shared_ptr<TreeLoadResult> result;
    {
        std::lock_guard lock(g_tree_load_mutex);
        const auto it = g_tree_loads.find(token);
        if (it == g_tree_loads.end()) return nullptr;
        result = std::move(it->second);
        g_tree_loads.erase(it);
    }
    path = result->path;
    ok = result->ok;
    stamp = result->stamp;
    return std::make_shared<const std::vector<fs::DirEntry>>(std::move(result->entries));
}

std::wstring TreePathKey(const std::wstring& path) {
    std::wstring key = path;
    for (wchar_t& c : key) c = static_cast<wchar_t>(std::towlower(c));
    return key;
}

void RebuildTreeRows(Tab& tab) {
    // This PC keeps flat rows: grouping (磁盘 / 设备 / 网盘程序入口) owns the
    // order, so expanding a drive or phone inline would fight it.
    if (!TreeViewEnabled() || !tab.snapshot || tab.view_mode != ui::ViewMode::Details ||
        tab.content_results || !tab.filter_text.empty() || tab.group_by != 0 ||
        tab.current_path.empty() || fs::IsVirtualPath(tab.current_path)) {
        tab.tree_rows.reset();
        return;
    }
    auto rows = std::make_shared<std::vector<fs::DirEntry>>();
    rows->reserve(tab.snapshot->size());
    std::function<void(const std::wstring&, const std::vector<fs::DirEntry>&, int)> append =
        [&](const std::wstring& parent, const std::vector<fs::DirEntry>& entries, int depth) {
            for (const fs::DirEntry& source : entries) {
                fs::DirEntry entry = source;
                entry.tree_depth = depth;
                if (entry.full_path.empty()) {
                    std::wstring base = parent;
                    if (!base.empty() && !base.ends_with(L'\\')) base += L'\\';
                    entry.full_path = base + entry.name;
                }
                const std::wstring key = TreePathKey(entry.full_path);
                entry.tree_has_children = entry.is_dir && !entry.is_reparse;
                entry.tree_expanded = entry.tree_has_children && tab.tree_expanded.contains(key);
                const std::wstring full = entry.full_path;
                const bool expand = entry.tree_expanded;
                rows->push_back(std::move(entry));
                if (!expand) continue;
                const auto children = tab.tree_children.find(key);
                if (children != tab.tree_children.end() && !children->second.empty())
                    append(full, children->second, depth + 1);
            }
        };
    append(tab.current_path, *tab.snapshot, 0);
    tab.tree_rows = std::move(rows);
}

UINT_PTR BeginTreeStampCheck(HWND notify,
                             std::vector<std::pair<std::wstring, uint64_t>> folders) {
    const UINT_PTR token = g_tree_token.fetch_add(1, std::memory_order_relaxed);
    auto result = std::make_shared<TreeStampResult>();
    {
        std::lock_guard lock(g_tree_load_mutex);
        g_tree_stamps[token] = result;
    }
    std::thread([notify, token, result, folders = std::move(folders)] {
        std::vector<std::wstring> changed;
        for (const auto& [path, stamp] : folders) {
            const uint64_t now = DirectoryStamp(path);
            // 0: the folder disappeared or is unreachable — let the refresh
            // pass re-enumerate and show what the listing really says.
            if (now == 0 || now != stamp) changed.push_back(path);
        }
        result->changed = std::move(changed);
        if (notify) PostMessageW(notify, WM_TREE_STAMPS, token, 0);
    }).detach();
    return token;
}

std::shared_ptr<const std::vector<std::wstring>> TakeTreeStampCheck(UINT_PTR token) {
    std::shared_ptr<TreeStampResult> result;
    {
        std::lock_guard lock(g_tree_load_mutex);
        const auto it = g_tree_stamps.find(token);
        if (it == g_tree_stamps.end()) return nullptr;
        result = std::move(it->second);
        g_tree_stamps.erase(it);
    }
    return std::make_shared<const std::vector<std::wstring>>(std::move(result->changed));
}

} // namespace luling::app
