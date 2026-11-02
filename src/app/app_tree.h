// app_tree.h — Inline file tree for the details list: expanded folders show
// their children as indented rows, with the same selection, operations and
// columns as the folder itself.
#pragma once
#include "app_model.h"

#include <string>
#include <vector>

namespace luling::app {

// Process-wide switch (Settings > 外观 > 文件树). Tab::TreeActive consults it.
void SetTreeViewEnabled(bool on) noexcept;
bool TreeViewEnabled() noexcept;

// Lowercase full path used as the expansion / children cache key.
std::wstring TreePathKey(const std::wstring& path);

// Flatten the snapshot plus every expanded folder's loaded children into the
// row list the details view draws. Clears tree_rows when the tree is not
// applicable (other view modes, search/tag/recycle, filters, grouping).
void RebuildTreeRows(Tab& tab);

// Enumerate one folder's children on a worker thread and post WM_TREE_CHILDREN
// (token in wParam) to `notify`. Registration lives process-wide, so a stuck
// enumeration can outlive no window but never writes to it.
UINT_PTR BeginTreeChildrenLoad(HWND notify, const std::wstring& path, ui::SortColumn column,
                               ui::SortDirection direction, bool show_hidden,
                               bool show_protected);
// Consume a finished load. Null when the token is unknown (stale). `stamp` is
// the folder's write time observed with the listing, for the change poll.
std::shared_ptr<const std::vector<fs::DirEntry>> TakeTreeChildrenLoad(UINT_PTR token,
                                                                     std::wstring& path, bool& ok,
                                                                     uint64_t& stamp);

// Directory write times, read on a worker thread and posted as WM_TREE_STAMPS.
// Each entry is (path, last known stamp); the result lists the paths whose
// stamp changed, so the tree can re-enumerate just those folders.
UINT_PTR BeginTreeStampCheck(HWND notify,
                             std::vector<std::pair<std::wstring, uint64_t>> folders);
std::shared_ptr<const std::vector<std::wstring>> TakeTreeStampCheck(UINT_PTR token);

} // namespace luling::app
