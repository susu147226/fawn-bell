#pragma once
// store_catalog.h — the signed extension catalog and extension installs.
//
// The catalog is a signed index in the Luling repository:
//   extensions/index.json      the index (publishers + extensions + releases)
//   extensions/index.sig.json  RSA-PKCS1-SHA256 over the index bytes
// The official publisher key is pinned in this build. A catalog that does not
// verify is never shown; while offline the last verified copy is reused.
//
// Each release names its package and the publisher's signature over the
// package's manifest.json. Installing verifies, in order: catalog signature,
// package size and SHA-256, manifest SHA-256, and the publisher signature.
// Running extensions is the extension host's job (not part of this store).

#include "../common/windows_compat.h"
#include "store_install_util.h"
#include "store_trust.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace luling::app {

struct StoreAsset {
    std::wstring url;
    uint64_t size = 0;
    std::wstring sha256;
};

struct StoreRelease {
    std::wstring version;
    std::wstring released;
    std::wstring min_luling;       // optional minimum app version
    std::wstring manifest_sha256;  // SHA-256 of manifest.json inside the zip
    StoreAsset package;
    StoreAsset signature;
    std::wstring notes;
};

struct StorePublisher {
    std::wstring id;  // SHA-256 of public_key, lower-case hex
    std::wstring name;
    std::wstring public_key;  // lower-case hex CNG RSAPUBLICBLOB
    std::wstring url;
    bool official = false;
};

struct StoreExtension {
    std::wstring id;
    std::wstring publisher;
    std::wstring name;
    std::wstring summary;
    std::wstring description;
    std::wstring category;
    std::wstring license;
    std::wstring homepage;
    std::wstring source;
    std::vector<std::wstring> tags;
    std::vector<StoreRelease> versions;  // newest first, as the catalog orders them

    const StoreRelease* Newest() const noexcept {
        return versions.empty() ? nullptr : &versions.front();
    }
};

enum class StoreCatalogState : uint32_t { Idle, Loading, Ready, Offline };

struct StoreCatalogSnapshot {
    StoreCatalogState state = StoreCatalogState::Idle;
    bool verified = false;    // the shown catalog passed the pinned signature
    bool from_cache = false;  // network failed; this is the last verified copy
    std::wstring error;       // diagnostics for the store page, not localized
    std::vector<StorePublisher> publishers;
    std::vector<StoreExtension> extensions;
};

// Last fetch result (any thread may copy it).
StoreCatalogSnapshot StoreCatalogSnapshotCopy();

// Starts a background fetch; posts WM_STORE_CATALOG to `notify` when done.
void StartStoreCatalogFetch(HWND notify);

const StorePublisher* FindStorePublisher(const StoreCatalogSnapshot& snapshot,
                                         std::wstring_view id);

// The publisher an extension belongs to, or null when the catalog is
// inconsistent (the installer refuses such an entry).
const StorePublisher* FindExtensionPublisher(const StoreCatalogSnapshot& snapshot,
                                             const StoreExtension& extension);

// Installed extensions (id -> version) from installed.json.
std::wstring InstalledStoreExtensionVersion(const std::wstring& id);

// Downloads, verifies and installs one extension; posts WM_STORE_INSTALLED with
// the result (1 ok, 0 failed) to `notify`.
void StartStoreExtensionInstall(HWND notify, StoreExtension extension,
                                StorePublisher publisher);

// Latest install error, for the page's status line ("" after success).
std::wstring StoreInstallError();

// Verifies a catalog index against the pinned official key. Exposed for tests.
bool VerifyCatalogDocument(std::string_view index, std::string_view signature_document,
                           std::wstring& error);

// Removes an extension in the real data folder.
void UninstallExtension(const std::wstring& id);

} // namespace luling::app
