// app_core.h — Services one process owns and every window shares.
//
// Multi-window work moves process-level state out of AppState first: a window
// keeps navigation and UI state, while the worker pool, the snapshot cache and
// the IPC clients live here. AppState holds reference aliases with the old
// member names so existing call sites are unchanged.
#pragma once
#include "app_worker.h"
#include "app_prefs.h"
#include "search_history.h"
#include "location_history.h"
#include "saved_search.h"
#include "../fs/fs_snapshot.h"
#include "../index/index_client.h"
#include "../index/network_agent_client.h"
#include "../index/content_search_client.h"

namespace luling {

struct AppCore {
    // Preferences and the user's history belong to the process: every window
    // reads the same copy and a save from one is what the others see.
    app::AppPrefs appPrefs;
    app::SearchHistory searchHistory;
    app::SearchHistoryWriter searchHistoryWriter;
    app::LocationHistory locationHistory;
    app::SavedSearchStore savedSearches;
    app::WorkerPool worker;
    fs::SnapshotStore store;
    index::IndexClient index;
    index::NetworkAgentClient networkIndex;
    index::ContentSearchClient contentSearch;
    index::ContentSearchClient duplicateSearch;
};

} // namespace luling
