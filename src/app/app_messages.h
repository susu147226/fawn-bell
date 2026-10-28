// app_messages.h — private window messages (WM_APP range).
//
// Light on purpose: routing tables and tests use the ids without pulling in
// AppState. Values are fixed: an installed build keeps its own numbering.
#pragma once
#include <windows.h>

namespace luling {

constexpr UINT WM_WORKER_RESULT = WM_APP + 1;
constexpr UINT WM_OPS_NOTIFY = WM_APP + 2;
constexpr UINT WM_INDEX_NOTIFY = WM_APP + 40;
constexpr UINT WM_NET_PROBE = WM_APP + 41;
constexpr UINT WM_INDEX_SEARCH = WM_APP + 42;
constexpr UINT WM_TAG_ADS_WARNING = WM_APP + 43;
constexpr UINT WM_TAG_ADS_DISCOVERED = WM_APP + 44;
constexpr UINT WM_SHELLCTX_ITEMS = WM_APP + 45;  // ShellCtxItemsPayload*
constexpr UINT WM_SHELL_VERBS = WM_APP + 46;     // ShellVerbsResult*
constexpr UINT WM_DETAILS_META = WM_APP + 47;    // DetailsMetaResult*
constexpr UINT WM_SETTINGS_TASK_RESULT = WM_APP + 48;
constexpr UINT WM_SHELL_CACHE_INVALIDATE = WM_APP + 49;
constexpr UINT WM_NETWORK_INDEX_NOTIFY = WM_APP + 51;
constexpr UINT WM_NETWORK_INDEX_SEARCH = WM_APP + 52;
constexpr UINT WM_CONTENT_SEARCH = WM_APP + 53;
constexpr UINT WM_QUICK_PREVIEW_NAVIGATE = WM_APP + 54;
constexpr UINT WM_QUICK_PREVIEW_OPEN = WM_APP + 55;
constexpr UINT WM_UPDATE_RESULT = WM_APP + 56;
constexpr UINT WM_RECYCLE_INFO = WM_APP + 57;
constexpr UINT WM_DUPLICATE_SCAN = WM_APP + 58;
constexpr UINT WM_DUP_VOLUMES = WM_APP + 59;
constexpr UINT WM_UPDATE_DOWNLOADED = WM_APP + 60;
constexpr UINT WM_UPDATE_INSTALL = WM_APP + 61;
constexpr UINT WM_SEARCH_HISTORY = WM_APP + 62;
constexpr UINT WM_CHANGE_TRACKING = WM_APP + 63;
constexpr UINT WM_CONTENT_SELECTION = WM_APP + 64;
constexpr UINT WM_QUICK_PREVIEW_COMMAND = WM_APP + 65;  // lParam: immutable command token
constexpr UINT WM_SHOW_RELEASE_NOTES = WM_APP + 66;     // "updated" toast clicked
constexpr UINT WM_FRAME_PUMP = WM_APP + 67;             // one display frame of motion
constexpr UINT WM_SHELL_SELECT = WM_APP + 68;           // app::ShellSelectRequest*
constexpr UINT WM_EXPLORER_TAKEOVER = WM_APP + 69;      // app::ExplorerTakeoverRequest*
constexpr UINT WM_SHELL_VERB_SEED = WM_APP + 70;        // ShellVerbSeed*
constexpr UINT WM_NETWORK_LIVE_SEARCH = WM_APP + 71;    // shared_ptr<LiveNetworkSearch>*
constexpr UINT WM_EXIT_LULING = WM_APP + 72;            // palette "Exit Luling" (#57)
constexpr UINT WM_NETWORK_LOCATIONS = WM_APP + 73;
constexpr UINT WM_ASSOC_CHANGED = WM_APP + 74;  // SHCNE_ASSOCCHANGED: default programs changed
// A UNC folder the worker could not list: lParam is an owned std::wstring of
// the path. Posted rather than shown inline so the credential dialog runs
// outside the worker-result handler (B34).
constexpr UINT WM_SHARE_CREDENTIALS = WM_APP + 75;
// The background probe found that the share needs a sign-in: the dialog runs
// here, on the UI thread. lParam is an owned std::wstring of the path.
constexpr UINT WM_SHARE_CREDENTIALS_PROMPT = WM_APP + 76;
// A device transfer finished on its worker thread. lParam owns a
// ShellCopyResult (app_ops_ui.h).
constexpr UINT WM_SHELL_COPY_DONE = WM_APP + 77;
// Drive verbs finished on a worker: lParam owns app::DriveVerbResult.
constexpr UINT WM_DRIVE_VERB = WM_APP + 78;
// The BitLocker prompt finished: lParam owns app::BitLockerUnlockResult.
constexpr UINT WM_BITLOCKER_UNLOCK = WM_APP + 79;

// Multi-window (M2/M4/M6): values above the single-window range, since the
// upstream 76-79 collide with the ids this build already ships.
constexpr UINT WM_RELEASE_WINDOW_STATES = WM_APP + 80;  // the host frees retired window states
constexpr UINT WM_NEW_WINDOW = WM_APP + 81;             // open another window at this window's folder
constexpr UINT WM_OPEN_SESSION_WINDOWS = WM_APP + 82;   // first window shown: open the session's other windows
constexpr UINT WM_BEGIN_TAB_DETACH = WM_APP + 83;       // a tab dragged off the strip: open it in a new window
constexpr UINT WM_OPEN_EXTERNAL = WM_APP + 84;          // host: lParam std::wstring*, a forwarded launch
constexpr UINT WM_UNDO_WINDOW_MERGE = WM_APP + 85;      // merge toast clicked: reopen the merged windows
constexpr UINT WM_MERGE_WINDOW = WM_APP + 86;           // wParam target HWND, lParam tab index: a window was dropped on a strip

} // namespace luling
