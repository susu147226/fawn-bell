// app_core.h — Services one process owns and every window shares.
//
// Multi-window work moves process-level state out of AppState first: a window
// keeps navigation and UI state, while the worker pool, the snapshot cache and
// the IPC clients live here. AppState holds reference aliases with the old
// member names so existing call sites are unchanged.
#pragma once
#include "app_worker.h"
#include "app_model.h"
#include "../ops/ops_manager.h"
#include "app_change_tracking.h"
#include "folder_sizes.h"
#include "live_network_search.h"
#include "host_window.h"
#include "window_registry.h"
#include "places.h"
#include "shell_window_registry.h"
#include "explorer_window_takeover.h"
#include "app_prefs.h"
#include "search_history.h"
#include "location_history.h"
#include "saved_search.h"
#include "context_menu_prefs.h"
#include "context_menu_controller.h"
#include "settings_controller.h"
#include "update_checker.h"
#include "update_installer.h"
#include "single_instance_coordinator.h"
#include "tray_controller.h"
#include "global_search_hotkey.h"
#include "global_search_window.h"
#include "../fs/fs_snapshot.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"
#include "../index/content_search_client.h"
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace luling {

struct AppState;

struct AppCore {
    // Where process-level notifications go. The first window to start owns it;
    // extra windows keep using that one instead of stealing the route.
    HWND service_window = nullptr;
    // Start/Stop and the tray Attach/Detach belong to that window too; other
    // windows reuse these instances without starting or stopping them.
    app::WorkerPool worker;
    // The ops layer is process-level like the Shell host it drives: one queue,
    // one undo stack, one luling_shell client for every window. A window
    // closing must not stop it (that left the others without folder verbs).
    ops::OpsManager ops;
    // IPC clients come first so they outlive the services holding pointers to
    // them: a consumer's destructor may still be running when the core unwinds.
    index::IndexClient index;
    index::NetworkAgentClient networkIndex;
    index::ContentSearchClient contentSearch;
    index::ContentSearchClient duplicateSearch;
    fs::SnapshotStore store;
    // Results of shared navigation/scan work. Every window drains the same
    // queue; a result applies only to the tabs whose path and generation it
    // carries, so a background window's refresh still lands.
    std::mutex resultMutex;
    std::queue<app::WorkResult> results;
    app::FolderSizes folderSizes;
    ChangeTrackingUi changes;
    uint64_t tagAdsResultRevision = 0;
    std::unordered_set<std::wstring> tagAdsDiscoveryQueued;
    std::unordered_set<std::wstring> tagAdsDiscoveryChecked;
    std::vector<index::VolumeInfo> dup_volume_cache;
    ULONGLONG dup_volume_cache_tick = 0;
    bool dup_volume_cache_pending = false;
    struct PendingIndexSearch {
        index::Query query;
        index::SearchResult local;
        index::SearchResult network;
        bool local_ready = false;
        bool network_ready = false;
        bool network_snapshot = false; // Captured for this request, not global root configuration.
        std::wstring live_network_root;  // #74: scope walked live instead of the network index
    };
    std::unordered_map<uint32_t, PendingIndexSearch> pendingIndexSearches;
    LiveNetworkSearchSessions liveNetworkSearches;
    uint32_t nextIndexReq = 1;
    // Preferences and the user's history belong to the process: every window
    // reads the same copy and a save from one is what the others see.
    app::AppPrefs appPrefs;
    app::SearchHistory searchHistory;
    app::SearchHistoryWriter searchHistoryWriter;
    app::LocationHistory locationHistory;
    app::SavedSearchStore savedSearches;
    // Shell context-menu preferences and the COM session every window reuses.
    app::ContextMenuPrefs ctxMenuPrefs;
    app::ContextMenuController context_menu;
    app::SettingsController settings;
    // Update check/download state: one check per process, shared by windows.
    app::UpdateChecker update_checker;
    app::UpdateResult update_result;
    bool update_result_ready = false;
    app::UpdateInstaller update_installer;
    DWORD update_install_error = ERROR_SUCCESS;
    // One tray icon, one single-instance endpoint, one global hotkey.
    app::TrayController tray_controller;
    GlobalSearchHotkey globalSearchHotkey;
    GlobalSearchWindow globalSearchWindow;
    app::SingleInstanceCoordinator single_instance;
    // The staging tray and the favorites/recent catalog are user data, not
    // window state: windows show the same tray and the same starred folders.
    app::StagingTray tray;
    app::PlacesCatalog places;
    // Shell windows published for "open file location" and the Explorer
    // takeover belong to the process, not to one window.
    std::unique_ptr<app::ShellWindowRegistry> shell_windows;
    std::unique_ptr<app::ExplorerWindowTakeover> explorer_takeover;
    // Multi-window: every window registers here; the hidden host owns the
    // process endpoint and forwards routed notifications to the most recent
    // window. Declared last so it is torn down first.
    app::BasicWindowRegistry<AppState> windows;
    bool started = false;  // the first window ran the process-level setup
    bool exiting = false;  // closing every window: not a user-close of one
    app::HostWindow host;
};

} // namespace luling
