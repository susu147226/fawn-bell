// tag_manager_dialog.h — the 「管理标签」 sheet: every tag with its color and
// item count, filtered by tab, multi-select, then merge or delete.
#pragma once

#include <d2d1.h>
#include <windows.h>

#include <string>
#include <vector>

namespace luling::ui {

struct TagManagerRow {
    std::wstring id;
    std::wstring name;
    uint32_t rgb = 0;
    int count = 0;
    bool invalid = false;   // at least one tagged path no longer exists
    bool checked = false;
};

struct TagManagerSpec {
    std::wstring title;
    std::wstring intro;
    std::wstring tabs[4];     // 全部 / 常用 / 未使用 / 有失效项
    std::wstring done_text;
    std::wstring merge_text;
    std::wstring delete_text;
    std::wstring invalid_note;
    std::vector<TagManagerRow> rows;
};

struct TagManagerResult {
    // First id is the merge target; the rest are folded into it.
    std::vector<std::wstring> merge_ids;
    std::vector<std::wstring> delete_ids;
};

// Modal. True when the user pressed Done; `result` then holds the requested
// operations (either list may be empty).
bool ShowTagManagerDialog(HWND owner, TagManagerSpec& spec, TagManagerResult& result,
                          bool dark, D2D1_COLOR_F accent);

} // namespace luling::ui
