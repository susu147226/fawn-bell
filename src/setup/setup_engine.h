#pragma once
// Install engine: the non-UI part of the Luling setup (P1). The wizard (P2)
// drives the same engine; silent and update installs call it directly.

#include "setup_common.h"
#include "setup_options.h"
#include "setup_payload.h"

#include <functional>
#include <string>

namespace luling::setup {

enum class Phase { Preparing, Extracting, Closing, Replacing, Registering, IndexService, SeedVerbs, Done };

struct EngineCallbacks {
    std::function<void(Phase, const std::wstring& status)> phase;
    std::function<bool(const ExtractProgress&)> progress;  // false cancels (before commit only)
    // Luling reported a running copy/move. Return true to ask again (Retry),
    // false to give up. Called on the engine thread; may block.
    std::function<bool()> busy;
};

struct InstallRequest {
    Options options;
    Tasks tasks;              // final selection
    std::wstring app_dir;     // resolved target
    std::wstring index_path;  // used when tasks.index_service
    bool upgrade = false;     // a registered install with luling.exe exists
    std::wstring previous_version;
};

struct InstallResult {
    int exit_code = 0;
    std::wstring error;
    std::wstring app_dir;
    bool index_running = false;  // for the summary on the finish page
    bool verbs_seeded = false;
    bool startup = false;
};

// Resolves defaults the wizard shows: target directory, remembered tasks and
// the configured index location (DefaultLulingDirectory/UsePreviousTasks/
// ConfiguredIndexPath in the iss).
InstallRequest ResolveDefaults(const Options& options);
bool FileExists(const std::wstring& path);
// Index directory from index-config.json (ConfiguredIndexPath in the iss).
std::wstring ConfiguredIndexPath();

InstallResult RunInstall(Payload& payload, const std::wstring& self_path, const InstallRequest& request,
                         const EngineCallbacks& callbacks);

}  // namespace luling::setup
