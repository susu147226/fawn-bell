// app_ops_ui.cpp — extracted from app_main.cpp.
#include "app_internal.h"
#include "../fs/shell_namespace.h"
#include "../ui/lumatext_renderer.h"
#include "../ui/fluent_menu.h"
#include "../ui/drag_drop.h"
#include "../ui/file_operation_dialog.h"
#include "../ui/batch_rename_dialog.h"
#include "../ui/convert_dialog.h"
#include "../ui/quick_preview_window.h"
#include "../ui/typography.h"
#include "../ui/color_picker.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../common/path_utils.h"
#include "../common/diagnostics_exporter.h"
#include "../common/debug_log.h"
#include "snapshot_patch.h"
#include "session.h"
#include "context_menu.h"
#include "batch_rename.h"
#include "link_resolve.h"
#include "locked_item_prompt.h"
#include "resource.h"
#include "../ops/clipboard.h"
#include "../ipc/ctx_menu_util.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>
#include <unordered_set>

using namespace luling;

namespace luling {
namespace {
// Ops and Shell host errors are Simplified protocol text; zh-TW shows the
// converted form, so classification accepts both.
// Errors come from OpsManager (localized with Pick) or luling_shell (Simplified),
// so accept the Simplified, Traditional and English forms.
bool ErrorIs(const std::wstring& error, const wchar_t* simplified, const wchar_t* english) {
    return error == simplified || error == l10n::Cn(simplified) || error == english;
}
bool ErrorHas(const std::wstring& error, const wchar_t* simplified) {
    return error.find(simplified) != std::wstring::npos ||
           error.find(l10n::Cn(simplified)) != std::wstring::npos;
}
} // namespace

bool SubmitWithConflictResolution(AppState& s, ops::OpRequest request, uint64_t* task_id) {
    // Copy/move conflict discovery is part of the transfer worker's recursive
    // scan. The UI only consumes immutable conflict snapshots.
    const auto id = s.ops.Submit(std::move(request));
    if (task_id) *task_id = id;
    return id != 0;
}

// Tell the user that staged items which vanished were left out.
static void NotifyTraySkipped(AppState& s, size_t n) {
    if (n == 0) return;
    std::wstring msg = l10n::Get(l10n::StringId::TraySkippedFormat);
    const size_t at = msg.find(L"{n}");
    if (at != std::wstring::npos) msg.replace(at, 3, std::to_wstring(n));
    s.notification_toast.Show(s.hwnd, l10n::Get(l10n::StringId::StagingTray), std::move(msg), false);
}

// Release one tray batch into the current folder through the ops layer.
// Every paste that moves a cut finishes it the same way: once all of the cut's
// items have moved (app_main's completion handler), the system clipboard is
// emptied - but only while it still holds exactly that cut (sequence number
// and paths), so a newer copy is never touched.
static void TrackCutClipboard(AppState& s, const ops::ClipboardData& cut) {
    s.cutPaths.clear();
    s.pendingCutClipboardSequence = cut.sequence;
    s.pendingCutClipboardPaths = cut.paths;
    s.completedCutClipboardPaths.clear();
}

// A tray batch taken as a move mirrors onto the system clipboard; when the tray
// releases those items instead, the mirrored cut is finished as well, as long as
// every clipboard path is among the items being moved.
static void TrackTrayCutClipboard(AppState& s, const std::vector<std::wstring>& moved) {
    ops::ClipboardData cut;
    if (moved.empty() || !ops::ReadClipboard(cut) || !cut.cut) return;
    for (const auto& path : cut.paths) {
        const std::wstring normalized = fs::NormalizePath(path);
        const bool staged = std::any_of(moved.begin(), moved.end(), [&](const std::wstring& item) {
            return _wcsicmp(fs::NormalizePath(item).c_str(), normalized.c_str()) == 0;
        });
        if (!staged) return;
    }
    TrackCutClipboard(s, cut);
}

void ReleaseTrayBatch(AppState& s, size_t idx) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || idx >= s.tray.batches().size()) return;
    if (fs::IsVirtualPath(tab->current_path) || tab->net_readonly) return;
    const app::TrayBatch& b = s.tray.batches()[idx];
    ops::OpRequest req;
    req.type = b.move_intent ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = tab->current_path;
    std::vector<uint64_t> item_ids;
    size_t skipped = 0;
    for (const auto& it : b.items) {
        if (s.tray.IsInFlight(it.path)) continue;
        if (it.exists) {
            if (std::none_of(req.sources.begin(), req.sources.end(), [&](const auto& source) {
                return _wcsicmp(source.c_str(), it.path.c_str()) == 0;
            })) req.sources.push_back(it.path);
            item_ids.push_back(it.id);
        }
        else ++skipped;
    }
    NotifyTraySkipped(s, skipped);
    if (req.sources.empty()) return;
    const bool move = b.move_intent;
    const auto moved = move ? req.sources : std::vector<std::wstring>{};
    const auto task = s.ops.Submit(std::move(req));
    if (!task) return;
    if (move) TrackTrayCutClipboard(s, moved);
    s.tray.MarkInFlight(task, item_ids, move);
    RememberTrayDest(s, tab->current_path);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

std::vector<std::wstring> TrayDestList(const AppState& s) {
    std::vector<std::wstring> out;
    const std::wstring& all = s.appPrefs.tray_dests;
    size_t start = 0;
    while (start < all.size() && out.size() < 3) {
        size_t bar = all.find(L'|', start);
        if (bar == std::wstring::npos) bar = all.size();
        if (bar > start) out.push_back(all.substr(start, bar - start));
        start = bar + 1;
    }
    return out;
}

void RememberTrayDest(AppState& s, const std::wstring& dir) {
    if (dir.empty() || fs::IsVirtualPath(dir)) return;
    // Stored for display: no \\?\ long-path prefix.
    const std::wstring norm = ClipboardPath(fs::NormalizePath(dir));
    std::vector<std::wstring> list{ norm };
    for (const auto& old : TrayDestList(s)) {
        if (_wcsicmp(old.c_str(), norm.c_str()) != 0 && list.size() < 3) list.push_back(old);
    }
    std::wstring joined;
    for (const auto& d : list) {
        if (!joined.empty()) joined += L'|';
        joined += d;
    }
    if (joined == s.appPrefs.tray_dests) return;
    s.appPrefs.tray_dests = std::move(joined);
    s.appPrefs.Save();
}

void SendTrayToDest(AppState& s, const std::wstring& dir, bool move) {
    if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    ops::OpRequest req;
    req.type = move ? ops::OpType::Move : ops::OpType::Copy;
    req.dest_dir = fs::NormalizePath(dir);
    std::vector<uint64_t> item_ids;
    size_t skipped = 0;
    for (const auto& batch : s.tray.batches())
        for (const auto& item : batch.items) {
            if (s.tray.IsInFlight(item.path)) continue;
            if (GetFileAttributesW(item.path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (std::none_of(req.sources.begin(), req.sources.end(), [&](const auto& source) {
                    return _wcsicmp(source.c_str(), item.path.c_str()) == 0;
                })) req.sources.push_back(item.path);
                item_ids.push_back(item.id);
            } else
                ++skipped;
        }
    NotifyTraySkipped(s, skipped);
    if (req.sources.empty()) return;
    const auto moved = move ? req.sources : std::vector<std::wstring>{};
    const auto task = s.ops.Submit(std::move(req));
    if (!task) return;
    if (move) TrackTrayCutClipboard(s, moved);
    s.tray.MarkInFlight(task, item_ids, move);
    RememberTrayDest(s, dir);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// Ctrl+V: release newest tray batch, else paste from the system clipboard.
bool ResolveShellCopyConflicts(AppState& s, std::vector<std::wstring>& sources,
                               const std::wstring& dest, bool dest_shell,
                               ShellCopyExtras* extras) {
    try {
    if (sources.empty() || dest.empty()) return false;
    {
        diagnostics::DebugLog("copy", L"resolve sources=" + std::to_wstring(sources.size()) +
            L" dest_shell=" + std::to_wstring(dest_shell ? 1 : 0) +
            L" dest=" + diagnostics::LogText(dest));
    }
    const app::Tab* destination_tab = nullptr;
    if (dest_shell) {
        // Snapshot lookup is fastest when the folder is open; otherwise ask the
        // Shell for the exact name (no full enumeration on the UI thread).
        for (const auto& pane : Panes(s)) {
            if (!pane) continue;
            const app::Tab* tab = pane->ActiveTab();
            if (tab && tab->current_path == dest && tab->snapshot) {
                destination_tab = tab;
                break;
            }
        }
    }
    std::vector<std::wstring> conflicting;
    std::vector<std::wstring> targets;
    for (const auto& source : sources) {
        std::wstring name;
        if (fs::IsShellPath(source)) {
            name = fs::ShellDisplayName(source);
        } else {
            const size_t slash = source.find_last_of(L"\\/");
            name = slash == std::wstring::npos ? source : source.substr(slash + 1);
        }
        if (name.empty()) continue;
        std::wstring target;
        if (dest_shell) {
            if (destination_tab) {
                for (size_t i = 0; i < destination_tab->EntryCount(); ++i) {
                    const fs::DirEntry& entry = destination_tab->EntryAt(i);
                    if (_wcsicmp(entry.name.c_str(), name.c_str()) == 0) {
                        target = entry.full_path;
                        break;
                    }
                }
            } else {
                target = fs::ShellChildPath(dest, name);
            }
        } else {
            std::wstring candidate = dest;
            if (!candidate.ends_with(L"\\")) candidate += L'\\';
            candidate += name;
            if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
                target = candidate;
        }
        if (!target.empty()) {
            conflicting.push_back(source);
            targets.push_back(target);
        }
    }
    if (conflicting.empty()) return true;
    auto file_size = [](const WIN32_FILE_ATTRIBUTE_DATA& data) {
        return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    };
    auto info_for = [&](size_t index, size_t remaining) {
        ops::ConflictItemInfo info;
        const std::wstring& source = conflicting[index];
        const std::wstring& target = targets[index];
        info.source = fs::IsShellPath(source) ? fs::ShellDisplayName(source) : source;
        info.destination = fs::IsShellPath(target) ? fs::ShellDisplayName(target) : target;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!fs::IsShellPath(source) &&
            GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &data)) {
            info.source_size = file_size(data);
            info.source_modified = data.ftLastWriteTime;
            info.source_is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
        if (!fs::IsShellPath(target) &&
            GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &data)) {
            info.destination_size = file_size(data);
            info.destination_modified = data.ftLastWriteTime;
            info.destination_is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
        info.remaining = remaining;
        return info;
    };
    auto remove_target = [&](size_t index) {
        if (extras) extras->replace_targets.push_back(targets[index]);
        return true;
    };
    auto source_name = [&](size_t index) {
        if (fs::IsShellPath(conflicting[index])) {
            const auto name = fs::ShellDisplayName(conflicting[index]);
            if (!name.empty()) return name;
        }
        const auto slash = conflicting[index].find_last_of(L"\\/");
        return slash == std::wstring::npos ? conflicting[index]
                                          : conflicting[index].substr(slash + 1);
    };
    auto local_child = [&](const std::wstring& name) {
        if (dest.empty()) return name;
        const wchar_t last = dest.back();
        return dest + (last == L'\\' || last == L'/' ? L"" : L"\\") + name;
    };
    auto unique_name = [&](const std::wstring& leaf) -> std::wstring {
        auto exists = [&](const std::wstring& candidate) {
            if (candidate.empty()) return true;
            if (dest_shell) return fs::ShellChildExists(dest, candidate);
            return GetFileAttributesW(local_child(candidate).c_str()) != INVALID_FILE_ATTRIBUTES;
        };
        if (!exists(leaf)) return leaf;
        const size_t dot = leaf.find_last_of(L'.');
        const std::wstring stem = dot == std::wstring::npos || dot == 0 ? leaf : leaf.substr(0, dot);
        const std::wstring extension = dot == std::wstring::npos || dot == 0 ? L"" : leaf.substr(dot);
        for (int n = 2; n < 1000; ++n) {
            const std::wstring candidate = stem + L" (" + std::to_wstring(n) + L")" + extension;
            if (!exists(candidate)) return candidate;
        }
        return {};
    };
    bool repeated = false;
    ops::ConflictChoice repeated_choice = ops::ConflictChoice::Cancel;
    for (size_t i = 0; i < conflicting.size(); ++i) {
        if (std::find(sources.begin(), sources.end(), conflicting[i]) == sources.end()) continue;
        ops::ConflictChoice choice = repeated_choice;
        if (!repeated) {
            const ui::ConflictDialogResult result = ui::ShowFileConflictDialog(
                s.hwnd, info_for(i, conflicting.size() - i - 1), s.darkMode, s.accentColor);
            choice = result.choice;
            if (result.apply_to_all) {
                repeated = true;
                repeated_choice = choice;
            }
        }
        {
            diagnostics::DebugLog("copy", L"choice target=" + diagnostics::LogText(targets[i]) +
                L" choice=" + std::to_wstring(static_cast<int>(choice)) +
                L" remaining=" + std::to_wstring(conflicting.size() - i - 1));
        }
        switch (choice) {
        case ops::ConflictChoice::Cancel:
            sources.clear();
            return false;
        case ops::ConflictChoice::Skip:
            std::erase(sources, conflicting[i]);
            break;
        case ops::ConflictChoice::KeepBoth: {
            // Never ask the Shell/device to resolve the name: copy under a
            // fresh name, which MTP handles without entering the conflict path
            // that ended the process. The copy itself runs on the worker.
            const std::wstring leaf = source_name(i);
            const std::wstring unique = unique_name(leaf);
            if (unique.empty()) {
                diagnostics::DebugLog("copy", L"keep both no unique name source=" +
                    diagnostics::LogText(leaf));
                s.notification_toast.Show(s.hwnd,
                    l10n::Pick(L"无法保留两者", L"Could not keep both"),
                    l10n::Pick(L"无法生成新的名称，已跳过该项目。",
                               L"Could not build a new name; the item was skipped."));
            } else if (extras) {
                extras->keep_both.push_back({conflicting[i], unique});
            }
            std::erase(sources, conflicting[i]);
            break;
        }
        case ops::ConflictChoice::Replace:
            remove_target(i);
            break;
        }
    }
    return true;
    } catch (...) {
        diagnostics::DebugLog("copy", L"exception in conflict resolution");
        sources.clear();
        return false;
    }
}

namespace {

bool RemoveShellCopyTarget(const std::wstring& target, bool dest_shell,
                           const std::wstring& dest) {
    if (dest_shell) {
        if (!fs::ShellDelete({target})) return false;
        // MTP deletion is asynchronous. Copying while the old item still
        // exists drives Shell into its conflict path, which ended the process.
        const std::wstring name = fs::ShellDisplayName(target);
        for (int attempt = 0; attempt < 25; ++attempt) {
            if (!name.empty() && !fs::ShellChildExists(dest, name)) return true;
            Sleep(100);
        }
        return false;
    }
    const DWORD attrs = GetFileAttributesW(target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(target.c_str()) != 0
                                              : DeleteFileW(target.c_str()) != 0;
}

bool RunShellCopyPlan(const ShellCopyPlan& plan, ShellCopyResult& result) {
    for (const auto& target : plan.replace_targets) {
        if (!RemoveShellCopyTarget(target, plan.dest_shell, plan.dest)) {
            diagnostics::DebugLog("copy", L"replace failed target=" +
                diagnostics::LogText(target));
            result.replace_failed = true;
            return false;
        }
    }
    bool ok = true;
    if (!plan.shell_sources.empty()) {
        ok = plan.dest_shell
            ? fs::ShellCopyWithin(plan.shell_sources, plan.dest, plan.move)
            : fs::ShellCopyToLocal(plan.shell_sources, plan.dest, plan.move);
    }
    if (ok && !plan.local_sources.empty()) {
        ok = plan.dest_shell
            ? fs::ShellCopyFromLocal(plan.local_sources, plan.dest, plan.move)
            : false;
    }
    for (const auto& item : plan.keep_both) {
        if (!ok) break;
        const bool copied = plan.dest_shell
            ? (fs::IsShellPath(item.source)
                ? fs::ShellCopyWithinAs(item.source, plan.dest, item.name, plan.move)
                : fs::ShellCopyFromLocalAs(item.source, plan.dest, item.name, plan.move))
            : fs::ShellCopyToLocalAs(item.source, plan.dest, item.name, plan.move);
        diagnostics::DebugLog("copy", L"keep both name=" + diagnostics::LogText(item.name) +
            L" ok=" + std::to_wstring(copied ? 1 : 0));
        if (!copied) result.keep_both_failed = true;
    }
    return ok;
}

void CollectShellCopyRefresh(const ShellCopyPlan& plan, ShellCopyResult& result) {
    result.refresh.push_back(plan.dest);
    if (!plan.move) return;
    for (const auto& source : plan.shell_sources) {
        const std::wstring parent = fs::ShellParentPath(source);
        if (!parent.empty()) result.refresh.push_back(parent);
    }
    for (const auto& source : plan.local_sources) {
        const std::wstring parent = fs::ParentPath(source);
        if (!parent.empty()) result.refresh.push_back(parent);
    }
    for (const auto& item : plan.keep_both) {
        const std::wstring parent = fs::IsShellPath(item.source)
            ? fs::ShellParentPath(item.source) : fs::ParentPath(item.source);
        if (!parent.empty()) result.refresh.push_back(parent);
    }
}

} // namespace

void StartShellCopy(AppState& s, ShellCopyPlan plan) {
    const bool has_work = !plan.shell_sources.empty() || !plan.local_sources.empty() ||
        !plan.keep_both.empty() || !plan.replace_targets.empty();
    if (!has_work) return;
    if (s.shell_copy_busy.exchange(true)) {
        s.notification_toast.Show(s.hwnd,
            l10n::Pick(L"正在传输", L"Transfer in progress"),
            l10n::Pick(L"请等待当前设备操作完成后再试。",
                       L"Wait for the current device transfer to finish."));
        return;
    }
    const HWND hwnd = s.hwnd;
    std::thread([hwnd, plan = std::move(plan)]() mutable {
        // A worker has no message pump, so it cannot serve an STA proxy. MTA
        // lets the Shell and the MTP provider marshal without one.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* result = new ShellCopyResult;
        result->from_drop = plan.from_drop;
        result->move = plan.move;
        result->dest = plan.dest;
        result->track_cut = plan.track_cut;
        result->cut_sequence = plan.cut_sequence;
        result->cut_paths = plan.cut_paths;
        try {
            result->ok = RunShellCopyPlan(plan, *result);
        } catch (...) {
            diagnostics::DebugLog("copy", L"exception in device transfer");
            result->ok = false;
        }
        if (result->ok) CollectShellCopyRefresh(plan, *result);
        diagnostics::DebugLog("copy", L"transfer done ok=" +
            std::to_wstring(result->ok ? 1 : 0) +
            L" replace_failed=" + std::to_wstring(result->replace_failed ? 1 : 0) +
            L" keep_both_failed=" + std::to_wstring(result->keep_both_failed ? 1 : 0));
        if (!PostMessageW(hwnd, WM_SHELL_COPY_DONE, 0,
                          reinterpret_cast<LPARAM>(result))) {
            delete result;
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
}

void PasteIntoCurrent(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty() || tab->net_readonly) return;
    const bool shell_target = fs::IsShellPath(tab->current_path);
    // Items copied from a device live only in this process: CF_HDROP cannot
    // describe them. Paste them before considering the system clipboard.
    if (!s.shellClipboardPaths.empty()) {
        auto sources = s.shellClipboardPaths;
        const bool move = s.shellClipboardCut;
        s.shellClipboardPaths.clear();
        s.shellClipboardCut = false;
        s.cutPaths.clear();
        ShellCopyExtras extras;
        if (!ResolveShellCopyConflicts(s, sources, tab->current_path, shell_target, &extras)) {
            InvalidateRect(s.hwnd, nullptr, FALSE);
            return;
        }
        ShellCopyPlan plan;
        plan.dest = tab->current_path;
        plan.dest_shell = shell_target;
        plan.move = move;
        plan.shell_sources = std::move(sources);
        plan.replace_targets = std::move(extras.replace_targets);
        plan.keep_both = std::move(extras.keep_both);
        {
            diagnostics::DebugLog("copy", L"paste queued dest_shell=" +
                std::to_wstring(shell_target ? 1 : 0) +
                L" count=" + std::to_wstring(plan.shell_sources.size()) +
                L" keep_both=" + std::to_wstring(plan.keep_both.size()) +
                L" move=" + std::to_wstring(move ? 1 : 0) +
                L" dest=" + diagnostics::LogText(tab->current_path));
        }
        StartShellCopy(s, std::move(plan));
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (shell_target) {
        // Local files dropped onto a phone or camera via the clipboard.
        ops::ClipboardData cb;
        if (ops::ReadClipboard(cb) && !cb.paths.empty()) {
            std::vector<std::wstring> sources;
            sources.reserve(cb.paths.size());
            for (auto& path : cb.paths) sources.push_back(fs::NormalizePath(path));
            ShellCopyExtras extras;
            if (!ResolveShellCopyConflicts(s, sources, tab->current_path, true, &extras)) {
                InvalidateRect(s.hwnd, nullptr, FALSE);
                return;
            }
            ShellCopyPlan plan;
            plan.dest = tab->current_path;
            plan.dest_shell = true;
            plan.move = cb.cut;
            plan.local_sources = std::move(sources);
            plan.replace_targets = std::move(extras.replace_targets);
            plan.keep_both = std::move(extras.keep_both);
            plan.track_cut = cb.cut;
            plan.cut_sequence = cb.sequence;
            plan.cut_paths = cb.paths;
            {
                diagnostics::DebugLog("copy", L"paste local-to-device queued count=" +
                    std::to_wstring(plan.local_sources.size()) +
                    L" cut=" + std::to_wstring(cb.cut ? 1 : 0) +
                    L" dest=" + diagnostics::LogText(tab->current_path));
            }
            StartShellCopy(s, std::move(plan));
        }
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (fs::IsVirtualPath(tab->current_path)) return;
    if (!s.tray.batches().empty()) {
        size_t idx = s.tray.batches().size() - 1;
        if (s.tray.batches()[idx].move_intent) s.cutPaths.clear();
        ReleaseTrayBatch(s, idx);
        return;
    }
    ops::ClipboardData cb;
    if (ops::ReadClipboard(cb)) {
        ops::OpRequest req;
        req.type = cb.cut ? ops::OpType::Move : ops::OpType::Copy;
        req.dest_dir = tab->current_path;
        for (auto& p : cb.paths) req.sources.push_back(fs::NormalizePath(p));
        if (SubmitWithConflictResolution(s, std::move(req)) && cb.cut) TrackCutClipboard(s, cb);
    }
}

ui::ConfirmDialogSpec BuildRecycleDeleteConfirm(const std::vector<std::wstring>& paths) {
    ui::ConfirmDialogSpec confirm;
    confirm.title = l10n::Get(l10n::StringId::Delete);
    if (paths.size() == 1) {
        confirm.message = l10n::Get(l10n::StringId::RecycleConfirmOne);
    } else {
        wchar_t buf[256]{};
        swprintf_s(buf, l10n::Get(l10n::StringId::RecycleConfirmManyFormat).c_str(), paths.size());
        confirm.message = buf;
    }
    // The dialog shows the first few and collapses the rest.
    for (const std::wstring& path : paths) confirm.items.push_back(ClipboardPath(path));
    confirm.confirm_text = l10n::Get(l10n::StringId::Delete);
    return confirm;
}

void DeleteSelected(AppState& s, bool permanent) {
    if(DeferContentSelection(s,[=](AppState& v){DeleteSelected(v,permanent);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab) return;
    if (IsRecycleTab(tab)) {
        std::vector<std::wstring> paths;
        size_t item_count = 0;
        if (tab->snapshot) {
            for (int index : tab->SelectedIndices()) {
                if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
                const fs::DirEntry& entry = tab->EntryAt(static_cast<size_t>(index));
                if (entry.recycle_path.empty()) continue;
                ++item_count;
                paths.push_back(entry.recycle_path);
                const std::wstring index_path = fs::RecycleIndexPath(entry.recycle_path);
                if (!index_path.empty()) paths.push_back(index_path);
            }
        }
        if (paths.empty()) return;
        ui::ConfirmDialogSpec confirm;
        confirm.title = l10n::Get(l10n::StringId::PermanentDelete);
        wchar_t message[256]{};
        swprintf_s(message, l10n::Get(l10n::StringId::RecycleDeleteConfirmFormat).c_str(),
                   item_count);
        confirm.message = message;
        confirm.confirm_text = l10n::Get(l10n::StringId::PermanentDelete);
        confirm.danger = true;
        if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
        ops::OpRequest req;
        req.type = ops::OpType::RealDelete;
        req.sources = std::move(paths);
        s.ops.Submit(std::move(req));
        return;
    }
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    DeletePaths(s, std::move(paths), permanent);
}

void DeletePaths(AppState& s, std::vector<std::wstring> paths, bool permanent) {
    if (paths.empty()) return;
    const bool has_shell = std::any_of(paths.begin(), paths.end(),
        [](const std::wstring& path) { return fs::IsShellPath(path); });
    if (has_shell) {
        // A device has no Recycle Bin and no filesystem path; delete through
        // the Shell after the same destructive confirmation.
        ui::ConfirmDialogSpec confirm;
        confirm.title = l10n::Get(l10n::StringId::PermanentDelete);
        if (paths.size() == 1) {
            confirm.message = l10n::Get(l10n::StringId::PermanentDeleteOne);
        } else {
            wchar_t buf[256]{};
            swprintf_s(buf, l10n::Get(l10n::StringId::PermanentDeleteManyFormat).c_str(),
                       paths.size());
            confirm.message = buf;
        }
        for (const std::wstring& path : paths) {
            const std::wstring name = fs::ShellDisplayName(path);
            confirm.items.push_back(name.empty() ? ClipboardPath(path) : name);
        }
        confirm.confirm_text = l10n::Get(l10n::StringId::PermanentDelete);
        confirm.danger = true;
        if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
        if (fs::ShellDelete(paths)) {
            if (auto* tab = ActiveTab(s))
                RefreshActiveTab(s, RefreshReason::OperationCompleted);
        } else {
            s.notification_toast.Show(s.hwnd,
                l10n::Get(l10n::StringId::PermanentDelete),
                l10n::Pick(L"设备无法删除这些项目，可能没有权限。",
                           L"The device could not delete these items."));
        }
        return;
    }
    // A share or removable drive has no Recycle Bin to restore from, and the
    // Shell refuses to recycle there. Explorer deletes those permanently
    // instead; the confirmation below is what keeps that step visible (§8.5.5).
    if (!permanent) {
        std::vector<std::wstring> direct;
        std::erase_if(paths, [&](const std::wstring& path) {
            DWORD volume_error = 0;
            if (fs::RecycleBinAvailable(path, &volume_error) || volume_error) return false;
            direct.push_back(path);
            return true;
        });
        if (!direct.empty()) {
            ui::ConfirmDialogSpec confirm;
            confirm.title = l10n::Get(l10n::StringId::PermanentDelete);
            confirm.message = l10n::Pick(L"网络位置和移动设备上没有回收站。",
                                         L"Network locations and removable drives have no Recycle Bin. ");
            if (direct.size() == 1) {
                confirm.message += l10n::Get(l10n::StringId::PermanentDeleteOne);
            } else {
                wchar_t buf[256]{};
                swprintf_s(buf, l10n::Get(l10n::StringId::PermanentDeleteManyFormat).c_str(),
                           direct.size());
                confirm.message += buf;
            }
            for (const std::wstring& path : direct) confirm.items.push_back(ClipboardPath(path));
            confirm.confirm_text = l10n::Get(l10n::StringId::PermanentDelete);
            confirm.danger = true;
            if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
            ops::OpRequest req;
            req.type = ops::OpType::RealDelete;
            req.sources = std::move(direct);
            s.ops.Submit(std::move(req));
            if (paths.empty()) return;
        }
    }
    if (permanent) {
        ui::ConfirmDialogSpec confirm;
        confirm.title = l10n::Get(l10n::StringId::PermanentDelete);
        if (paths.size() == 1) {
            confirm.message = l10n::Get(l10n::StringId::PermanentDeleteOne);
        } else {
            wchar_t buf[256]{};
            swprintf_s(buf, l10n::Get(l10n::StringId::PermanentDeleteManyFormat).c_str(),
                       paths.size());
            confirm.message = buf;
        }
        // The dialog shows the first few and collapses the rest.
        for (const std::wstring& path : paths) confirm.items.push_back(ClipboardPath(path));
        confirm.confirm_text = l10n::Get(l10n::StringId::PermanentDelete);
        confirm.danger = true;
        if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
    } else if (s.appPrefs.confirm_recycle_delete) {
        const ui::ConfirmDialogSpec confirm = BuildRecycleDeleteConfirm(paths);
        if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
    }
    ops::OpRequest req;
    req.type = permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

// ---------------------------------------------------------------------------
// Stage 1B-2: built-in Fluent context menu + new-item dropdown.
void RestoreSelected(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || !IsRecycleTab(tab)) return;
    std::vector<std::wstring> paths;
    for (int index : tab->SelectedIndices()) {
        if (index < 0 || index >= static_cast<int>(tab->EntryCount())) continue;
        const auto& path = tab->EntryAt(static_cast<size_t>(index)).recycle_path;
        if (!path.empty()) paths.push_back(path);
    }
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RestoreRecycle;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

void RestoreAllRecycle(AppState& s) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || !IsRecycleTab(tab)) return;
    std::vector<std::wstring> paths;
    const size_t count = tab->EntryCount();
    paths.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        std::wstring full = tab->EntryAt(i).recycle_path;
        if (!full.empty()) paths.push_back(std::move(full));
    }
    if (paths.empty()) return;
    ops::OpRequest req;
    req.type = ops::OpType::RestoreRecycle;
    req.sources = std::move(paths);
    s.ops.Submit(std::move(req));
}

void EmptyRecycleBin(AppState& s) {
    ui::ConfirmDialogSpec confirm;
    confirm.title = l10n::Get(l10n::StringId::EmptyRecycleConfirmTitle);
    confirm.message = l10n::Get(l10n::StringId::EmptyRecycleConfirm);
    confirm.confirm_text = l10n::Get(l10n::StringId::EmptyRecycleBin);
    confirm.cancel_text = l10n::Get(l10n::StringId::Cancel);
    confirm.danger = true;
    if (!ui::ShowConfirmDialog(s.hwnd, confirm, s.darkMode, s.accentColor)) return;
    ops::OpRequest req;
    req.type = ops::OpType::EmptyRecycle;
    s.ops.Submit(std::move(req));
}
void CollectToTray(AppState& s, bool move_intent) {
    if(DeferContentSelection(s,[=](AppState& v){CollectToTray(v,move_intent);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || IsRecycleTab(tab)) return;
    std::vector<std::wstring> paths = SelectedFullPaths(*tab);
    CollectPathsToTray(s, paths, move_intent);
}

// Filling the tray is an explicit action only: the status-bar hint, or dropping
// onto the tray. Copy / cut go through MirrorSelectionToClipboard instead, so
// collecting here must never touch the clipboard (issue #126).
void CollectPathsToTray(AppState& s, const std::vector<std::wstring>& paths, bool move_intent) {
    if (paths.empty()) return;
    s.tray.Collect(paths, move_intent);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void MirrorSelectionToClipboard(AppState& s, bool move_intent) {
    if(DeferContentSelection(s,[=](AppState& v){MirrorSelectionToClipboard(v,move_intent);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot || IsRecycleTab(tab)) return;
    MirrorPathsToClipboard(s, SelectedFullPaths(*tab), move_intent);
}

void MirrorPathsToClipboard(AppState& s, const std::vector<std::wstring>& paths, bool move_intent) {
    if (paths.empty()) return;
    const bool has_shell = std::any_of(paths.begin(), paths.end(),
        [](const std::wstring& path) { return fs::IsShellPath(path); });
    s.shellClipboardPaths.clear();
    s.shellClipboardCut = false;
    if (has_shell) {
        // A portable device item has no CF_HDROP representation. Keep it in
        // process so Ctrl+V can run an IFileOperation against it.
        s.shellClipboardPaths = paths;
        s.shellClipboardCut = move_intent;
        s.cutPaths = move_intent ? paths : std::vector<std::wstring>{};
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    // The cut state still shows on the list rows; only the clipboard is written.
    s.cutPaths = move_intent ? paths : std::vector<std::wstring>{};
    std::vector<std::wstring> cbPaths;
    cbPaths.reserve(paths.size());
    for (const auto& p : paths) cbPaths.push_back(ClipboardPath(p));
    ops::WriteClipboard(cbPaths, move_intent);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}
void PinAndShowOperationWindow(AppState& s) {
    if (!s.operationWindow) return;
    const ops::OpStatus status = s.ops.Status();
    if (!status.active && status.summary.empty() && status.last_error.empty()) return;
    s.operationDismissedTaskId = 0;
    s.operationPinnedByUser = true;
    s.operationAutoShown = true;
    s.operationWindow->Update(status);
    s.operationWindow->Show(true);
}

// Both batch-rename entries (the selection and the tray) submit through here.
// Nothing else changes up front: places and the tray follow the renames that
// actually succeeded once the operation completes (app_main's completion
// handler), so a failed or cancelled item never leaves a stale new name behind.
static void SubmitBatchRename(AppState& s, const std::vector<std::wstring>& paths, app::Tab* select_in,
                              const app::BatchRenameRule* initial = nullptr) {
    if (paths.size() < 2) return;
    const auto result = ui::ShowBatchRenameDialog(s.hwnd, paths, s.darkMode, s.accentColor, initial);
    if (!result.accepted) return;
    ops::OpRequest req;
    req.type = ops::OpType::BatchRename;
    for (const auto& item : result.items) {
        if (item.status != app::BatchRenameStatus::Ok) continue;
        req.sources.push_back(item.source_path);
        req.new_names.push_back(item.new_name);
    }
    if (req.sources.empty()) return;
    if (select_in) {
        select_in->pending_selected_names = req.new_names;
        select_in->pending_selected_name = req.new_names.front();
    }
    s.ops.Submit(std::move(req));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowBatchRename(AppState& s) {
    if(DeferContentSelection(s,[=](AppState& v){ShowBatchRename(v);})) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->net_readonly) return;
    SubmitBatchRename(s, SelectedFullPaths(*tab), tab);
}
void ShowBatchRename(AppState& s, const app::BatchRenameRule& initial) {
    if (DeferContentSelection(s, [=](AppState& v) { ShowBatchRename(v, initial); })) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->net_readonly) return;
    SubmitBatchRename(s, SelectedFullPaths(*tab), tab, &initial);
}
void ShowBatchRenamePaths(AppState& s, const std::vector<std::wstring>& paths) {
    SubmitBatchRename(s, paths, nullptr);
}

// 转换 (0.2.0). The dialog owns the preview; here we only turn the approved
// items into jobs. Originals are never touched when the user kept them, and the
// ops layer takes no undo entry either way.
static void SubmitConvert(AppState& s, const std::vector<std::wstring>& paths, app::ConvertMode mode) {
    if (paths.empty()) return;
    const auto result = ui::ShowConvertDialog(s.hwnd, paths, s.darkMode, s.accentColor, mode);
    if (!result.accepted) return;
    ops::OpRequest req;
    req.type = ops::OpType::Convert;
    for (const auto& item : result.items) {
        if (item.status != app::ConvertStatus::Ok) continue;
        ops::ConvertJob job;
        job.source = item.source;
        job.target = item.target;
        job.args = app::BuildConvertArgs(item, result.options);
        job.overwrite = result.options.target == app::ConvertTarget::Overwrite;
        // Stills finish in seconds; a 4K transcode legitimately takes many minutes.
        job.timeout_ms = item.kind == app::ConvertKind::Image ? 60000u : 1800000u;
        req.convert_jobs.push_back(std::move(job));
        req.sources.push_back(item.source);
    }
    if (req.convert_jobs.empty()) return;
    s.ops.Submit(std::move(req));
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ShowConvert(AppState& s, app::ConvertMode mode) {
    if (DeferContentSelection(s, [=](AppState& v) { ShowConvert(v, mode); })) return;
    app::Tab* tab = ActiveTab(s);
    if (!tab || IsRecycleTab(tab) || tab->net_readonly) return;
    SubmitConvert(s, SelectedFullPaths(*tab), mode);
}

void UpdateOperationWindow(AppState& s, bool allow_conflict_dialog) {
    if (!s.operationWindow) return;
    const auto now = std::chrono::steady_clock::now();
    const ops::OpStatus status = s.ops.Status();
    s.operationWindow->Update(status);

    if (status.active && status.task_id != s.operationUiTaskId) {
        s.operationUiTaskId = status.task_id;
        s.operationAutoShown = false;
        s.operationPinnedByUser = false;
        s.operationStartedAt = now;
        s.operationFinishedAt = {};
    }

    if (allow_conflict_dialog) {
        if (const auto conflict = s.ops.PendingConflict();
            conflict && conflict->token != s.conflictUiToken) {
            s.conflictUiToken = conflict->token;
            const ui::ConflictDialogResult result = ui::ShowFileConflictDialog(
                s.hwnd, *conflict, s.darkMode, s.accentColor);
            s.ops.ResolveConflict(conflict->token, result.choice, result.apply_to_all);
        }
    }

    // Failed on an item other processes hold: name them and offer the retry
    // instead of the plain failure window (the token guards modal re-entry).
    const bool locked_failure = !status.active && status.phase == ops::OpPhase::Failed
        && !status.lock_owners.empty() && status.task_id != 0;
    if (locked_failure && allow_conflict_dialog && status.task_id != s.lockPromptTaskId) {
        s.lockPromptTaskId = status.task_id;
        s.operationDismissedTaskId = status.task_id;
        s.operationPinnedByUser = false;
        if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
        PromptLockedItem(s, status);
        return;
    }

    if (status.task_id != 0 && status.task_id == s.operationDismissedTaskId &&
        !s.operationPinnedByUser) {
        if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
        return;
    }

    if (status.active && status.authorization != ops::AuthorizationState::None) {
        // Keep the existing operation window visible without stealing focus from UAC.
        if (!s.operationWindow->IsVisible()) s.operationWindow->Show(false);
        return;
    }

    if (status.active) {
        // Progress lives in the status-bar pill; the details window opens only
        // when the pill is clicked (conflicts and failures still surface).
        return;
    }

    if (s.operationPinnedByUser) return;

    if (status.phase == ops::OpPhase::Completed) {
        if (s.operationWindow->IsVisible()) {
            if (s.operationFinishedAt.time_since_epoch().count() == 0)
                s.operationFinishedAt = now;
            if (now - s.operationFinishedAt >= std::chrono::milliseconds(600))
                s.operationWindow->Hide();
        }
    } else if (status.phase == ops::OpPhase::Failed) {
        // The ops notification shows the locked-item prompt; a timer tick
        // that sees the failure first must not open the failure window.
        if (locked_failure) return;
        if (ErrorIs(status.last_error, L"已取消", L"Canceled")) {
            s.operationWindow->Hide();
            return;
        }
        const bool simple = status.type == ops::OpType::CreateFolder
                         || status.type == ops::OpType::CreateTextFile
                         || status.type == ops::OpType::Rename
                         || status.type == ops::OpType::BatchRename;
        if (simple) {
            if (s.operationWindow->IsVisible()) s.operationWindow->Hide();
            if (status.task_id == 0 || status.task_id == s.operationDismissedTaskId)
                return;
            s.operationDismissedTaskId = status.task_id;
            const bool folder = status.type == ops::OpType::CreateFolder;
            const bool file = status.type == ops::OpType::CreateTextFile;
            const std::wstring title = l10n::Get(folder ? l10n::StringId::CannotCreateFolder
                : file ? l10n::StringId::CannotCreateTextFile : l10n::StringId::CannotRename);
            std::wstring message;
            const std::wstring& err = status.last_error;
            const bool no_access = ErrorHas(err, L"没有权限")
                || ErrorHas(err, L"拒绝访问")
                || err.find(L"存取被拒") != std::wstring::npos  // zh-TW system message
                || err.find(L"Access is denied") != std::wstring::npos
                || err == L"create failed";
            if (no_access) {
                message = l10n::Get(folder || file
                    ? l10n::StringId::FolderNoWritePermission
                    : l10n::StringId::RenameNoPermission);
            } else if (ErrorIs(err, L"目标名称已存在", L"The target name already exists")) {
                message = l10n::Get(l10n::StringId::RenameTargetExists);
            } else if (ErrorIs(err, L"名称无效", L"Invalid name")) {
                message = l10n::Get(l10n::StringId::InvalidName);
            } else if (!err.empty()) {
                // luling_shell reports Simplified text; known messages are localized,
                // file names and unknown messages are shown as reported.
                message = l10n::ServiceErrorText(err);
            } else {
                message = l10n::Get(l10n::StringId::OperationFailedMessage);
            }
            s.notification_toast.ShowError(s.hwnd, title, std::move(message));
            return;
        }
        if (!s.operationWindow->IsVisible()) s.operationWindow->Show(true);
    }
}

} // namespace luling
