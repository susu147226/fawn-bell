// app_ops_ui.h — Delete/tray/paste/conflict/recycle operations UI.
#pragma once
#include "app_runtime.h"
#include "batch_rename.h"
#include "convert_rules.h"
#include "../ui/confirm_dialog.h"

namespace luling {
bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request, uint64_t* task_id = nullptr);
void ReleaseTrayBatch(AppState& s, size_t idx);
void PasteIntoCurrent(AppState& s);
// Device copies cannot rely on IFileOperation's conflict UI: a same-name item
// makes Shell abort the process. Detect it first and show the shared replace /
// skip / keep-both dialog; false means the operation was cancelled.
struct ShellCopyKeepBoth {
    std::wstring source;
    std::wstring name;
};
// Decisions that must run after the dialog closes. MTP transfers take seconds,
// so the caller hands this to StartShellCopy instead of blocking the UI thread.
struct ShellCopyExtras {
    std::vector<std::wstring> replace_targets;
    std::vector<ShellCopyKeepBoth> keep_both;
};
bool ResolveShellCopyConflicts(AppState& s, std::vector<std::wstring>& sources,
                               const std::wstring& dest, bool dest_shell,
                               ShellCopyExtras* extras = nullptr);
struct ShellCopyPlan {
    std::wstring dest;
    bool dest_shell = false;
    bool move = false;
    bool from_drop = false;
    std::vector<std::wstring> shell_sources;
    std::vector<std::wstring> local_sources;
    std::vector<std::wstring> replace_targets;
    std::vector<ShellCopyKeepBoth> keep_both;
    // A paste that cuts: the clipboard is marked moved only once the transfer
    // lands, so a failure leaves the source usable.
    bool track_cut = false;
    uint32_t cut_sequence = 0;
    std::vector<std::wstring> cut_paths;
};
// Posted as WM_SHELL_COPY_DONE; the window procedure owns the pointer.
struct ShellCopyResult {
    bool ok = false;
    bool from_drop = false;
    bool move = false;
    bool replace_failed = false;
    bool keep_both_failed = false;
    bool track_cut = false;
    uint32_t cut_sequence = 0;
    std::vector<std::wstring> cut_paths;
    std::wstring dest;
    std::vector<std::wstring> refresh;
};
// Runs the transfer on a worker thread and posts WM_SHELL_COPY_DONE back.
void StartShellCopy(AppState& s, ShellCopyPlan plan);
void DeleteSelected(AppState& s, bool permanent);
void DeletePaths(AppState& s, std::vector<std::wstring> paths, bool permanent);
// Confirmation shown before Delete moves items to the Recycle Bin (opt-in setting).
ui::ConfirmDialogSpec BuildRecycleDeleteConfirm(const std::vector<std::wstring>& paths);
void RestoreSelected(AppState& s);
// Restores every listed recycle-bin item (the filtered view when a filter is on).
void RestoreAllRecycle(AppState& s);
void EmptyRecycleBin(AppState& s);
void CollectToTray(AppState& s, bool move_intent);
void CollectPathsToTray(AppState& s, const std::vector<std::wstring>& paths, bool move_intent);
// Copy / cut fill the system clipboard only. Filling the staging tray is an
// explicit action (issue #126), so Ctrl+C / Ctrl+X and the toolbar buttons must
// not go through CollectToTray.
void MirrorSelectionToClipboard(AppState& s, bool move_intent);
void MirrorPathsToClipboard(AppState& s, const std::vector<std::wstring>& paths, bool move_intent);
void ShowBatchRename(AppState& s);
// The same dialog opened with a preset rule: the 按序号重命名 rows (grouped
// sequence, start 0/1) and 追加文件名后缀 use this.
void ShowBatchRename(AppState& s, const app::BatchRenameRule& initial);
// Staging tray recent drop destinations (appPrefs.tray_dests, newest first, max 3).
std::vector<std::wstring> TrayDestList(const AppState& s);
void RememberTrayDest(AppState& s, const std::wstring& dir);
// Copy (or move) every staged item that still exists into dir.
void SendTrayToDest(AppState& s, const std::wstring& dir, bool move);
// Batch rename an explicit path list (staging tray); tray entries follow.
void ShowBatchRenamePaths(AppState& s, const std::vector<std::wstring>& paths);
// 转换 (0.2.0): format conversion, resize, compress. One entry per 转换 submenu row.
void ShowConvert(AppState& s, app::ConvertMode mode);
void PinAndShowOperationWindow(AppState& s);
void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog);
} // namespace luling
