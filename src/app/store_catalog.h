#pragma once
// store_catalog.h — 扩展商店's GitHub catalog and extension installs.
//
// The catalog is one JSON file in the Luling repository:
//   { "version": 1, "extensions": [ { "id", "name", "desc", "version",
//                                     "size", "url", "sha256" } ] }
// Fetching is read-only; installing downloads the package zip and extracts it
// under %LOCALAPPDATA%\Luling\extensions\<id>. Running extensions is the
// extension host's job (not part of this store yet).

#include "../common/windows_compat.h"
#include "store_install_util.h"

#include <cstdint>
#include <string>
#include <vector>

namespace luling::app {

struct StoreExtension {
    std::wstring id;
    std::wstring name;
    std::wstring desc;
    std::wstring version;
    std::wstring url;
    std::wstring sha256;  // required: the installer refuses a package without it
    uint64_t size = 0;
    // Catalog v2 fields; 0 / empty for a v1 entry.
    int schema = 0;
    int api_version = 0;
    std::vector<std::wstring> capabilities;
};

enum class StoreCatalogState : uint32_t { Idle, Loading, Ready, Offline };

struct StoreCatalogSnapshot {
    StoreCatalogState state = StoreCatalogState::Idle;
    std::vector<StoreExtension> extensions;
    bool from_cache = false;
};

// Last fetch result (any thread may copy it).
StoreCatalogSnapshot StoreCatalogSnapshotCopy();

// Starts a background fetch; posts WM_STORE_CATALOG to `notify` when done.
void StartStoreCatalogFetch(HWND notify);

// Installed extensions (id -> version) from installed.json.
std::wstring InstalledStoreExtensionVersion(const std::wstring& id);

// Downloads and extracts one extension; posts WM_STORE_INSTALLED with the
// result (1 ok, 0 failed) to `notify`.
void StartStoreExtensionInstall(HWND notify, StoreExtension extension);

// Latest install error, for the page's status line ("" after success).
std::wstring StoreInstallError();

// Removes an extension in the real data folder.
void UninstallExtension(const std::wstring& id);

} // namespace luling::app
