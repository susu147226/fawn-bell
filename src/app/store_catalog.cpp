#include "store_catalog.h"

#include "app_messages.h"
#include "session.h"
#include "store_signature.h"
#include "update_transport.h"
#include "../common/json_value.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace luling::app {
namespace {

// The official catalog publisher. Both values are pinned in this build: the
// catalog and every publisher entry are checked against them, so a catalog
// served by something else, or one that names a different "official"
// publisher, is rejected before any extension is shown.
constexpr std::string_view kCatalogPublicKeyHex =
    "525341310008000003000000000100000000000000000000010001f1b2e5f1cf1b5ef6c0ac8d26b528c129cf"
    "28029d66b0ae791a9ae073601c5f6ae0cd3b9195acf07a603892547b579f0c7aa667f8163a6e4e6e680752"
    "410e051cfac0710ec82bfdf55fae5132faab9773b15b8558ed7b5308e095a79178523deabe794cd5381f10"
    "983bc3d9e3d88b85fe33d65672bb70f5326e8152ce7eabfb902008770fa0633d63ba1a447fe0963a1c19f"
    "5b8143169fdba5a4bebbedc529b747c3f72278839f22a41f34e5e4fe4a40b3ca6b4c944a70ecdff47981"
    "c13d10b0bcb676bf6a9f8a3fdbddcee0e2ca358254d9624ad7ee416dfc34dbd182ab43bbb52a98855c01"
    "aa9412f8c64fd7339e23acec59a77d1ddec13d23e14c579911ac1";
constexpr std::string_view kCatalogPublisherId =
    "d4176e45f924b603912e48869f54d7c03d3acfd8237fd7f5c395cdffc37b7aa4";

// The catalog lives in the Luling repository; a fork or mirror can change these
// two lines. Raw GitHub is used because the index is small and updated
// independently of releases.
constexpr wchar_t kCatalogIndexUrl[] =
    L"https://raw.githubusercontent.com/susu147226/fawn-bell/main/extensions/index.json";
constexpr wchar_t kCatalogSignatureUrl[] =
    L"https://raw.githubusercontent.com/susu147226/fawn-bell/main/extensions/index.sig.json";

constexpr uint64_t kMaxCatalogBytes = 4u << 20;      // 4 MiB
constexpr uint64_t kMaxSignatureBytes = 16u << 10;   // 16 KiB
constexpr uint64_t kMaxPackageBytes = 512u << 20;    // 512 MiB
constexpr size_t kMaxPublishers = 64;
constexpr size_t kMaxExtensions = 512;

std::mutex g_mutex;
StoreCatalogSnapshot g_snapshot;
std::wstring g_install_error;

bool ReadAllBytes(const std::wstring& path, std::string& out) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
    char buffer[16384];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, n);
    fclose(file);
    return !out.empty();
}

void WriteAllBytes(const std::wstring& path, std::string_view bytes) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return;
    fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
}

std::wstring CatalogCachePath() { return GetLulingDataDir() + L"\\extensions_index.json"; }
std::wstring SignatureCachePath() { return GetLulingDataDir() + L"\\extensions_index.sig.json"; }
std::wstring ExtensionsRoot() { return GetLulingDataDir() + L"\\extensions"; }
std::wstring InstalledPath() { return ExtensionsRoot() + L"\\installed.json"; }

bool ValidId(std::wstring_view id) {
    if (id.empty() || id.size() > 64) return false;
    for (wchar_t c : id) {
        if (std::iswalnum(c) || c == L'-' || c == L'_' || c == L'.') continue;
        return false;
    }
    return true;
}

bool ValidHex64(std::wstring_view text) {
    if (text.size() != 64) return false;
    for (wchar_t c : text) {
        if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))
            continue;
        return false;
    }
    return true;
}

bool ValidHttpsUrl(std::wstring_view url) {
    return url.size() > 8 && url.size() <= 2048 && url.substr(0, 8) == L"https://";
}

// Package assets may only come from GitHub releases: the catalog is signed,
// but a typo or a compromised entry should still not point at an arbitrary
// host.
bool ValidPackageUrl(std::wstring_view url) {
    return ValidHttpsUrl(url) && url.substr(0, 19) == L"https://github.com/";
}

std::wstring Field(const json::Value& object, const char* key) {
    return json::Wide(object.get_string(key));
}

struct SignatureDocument {
    std::wstring publisher;
    std::wstring signature;
};

bool ParseSignatureDocument(std::string_view body, SignatureDocument& out,
                            std::wstring& error) {
    json::Value root;
    if (!json::Parse(body, root, nullptr) || !root.is_object()) {
        error = L"signature: not a JSON object";
        return false;
    }
    int64_t format = 0;
    const json::Value* format_value = root.find("formatVersion");
    if (!format_value || !format_value->get_int(format, 1, 1)) {
        error = L"signature: unsupported format";
        return false;
    }
    if (root.get_string("algorithm") != kStoreSignatureAlgorithm) {
        error = L"signature: unsupported algorithm";
        return false;
    }
    out.publisher = Field(root, "publisher");
    out.signature = Field(root, "signature");
    if (!ValidHex64(out.publisher) || out.signature.size() < 512 ||
        out.signature.size() > 1024) {
        error = L"signature: malformed";
        return false;
    }
    return true;
}

bool AssetField(const json::Value& parent, const char* key, bool package,
                StoreAsset& out) {
    const json::Value* value = parent.find(key);
    if (!value || !value->is_object()) return false;
    out.url = Field(*value, "url");
    int64_t size = 0;
    const json::Value* size_value = value->find("size");
    const int64_t maximum = static_cast<int64_t>(kMaxPackageBytes);
    if (!size_value || !size_value->get_int(size, 1, maximum)) return false;
    out.size = static_cast<uint64_t>(size);
    out.sha256 = Field(*value, "sha256");
    if (package ? !ValidPackageUrl(out.url) : !ValidHttpsUrl(out.url)) return false;
    return ValidHex64(out.sha256);
}

bool ParsePublishers(const json::Value& root, StoreCatalogSnapshot& out,
                     std::wstring& error) {
    const json::Value* list = root.find("publishers");
    if (!list || !list->is_array() || list->size() > kMaxPublishers) {
        error = L"catalog: invalid publishers";
        return false;
    }
    for (const auto& item : list->items()) {
        if (!item.is_object()) {
            error = L"catalog: invalid publisher entry";
            return false;
        }
        StorePublisher publisher;
        publisher.id = Field(item, "id");
        publisher.name = Field(item, "name");
        publisher.public_key = Field(item, "publicKey");
        publisher.url = Field(item, "url");
        publisher.official = item.get_bool("official", false);
        if (!ValidHex64(publisher.id) || publisher.name.empty() ||
            publisher.public_key.empty() || publisher.public_key.size() > 8192) {
            error = L"catalog: malformed publisher";
            return false;
        }
        // The id is what the trust list and every extension reference, so it
        // must really be the hash of this key.
        std::string derived;
        if (!PublisherIdFromPublicKey(json::Utf8(publisher.public_key), derived) ||
            derived != json::Utf8(publisher.id)) {
            error = L"catalog: publisher id does not match its key";
            return false;
        }
        if (publisher.official &&
            (json::Utf8(publisher.id) != kCatalogPublisherId ||
             json::Utf8(publisher.public_key) != kCatalogPublicKeyHex)) {
            error = L"catalog: unknown official publisher";
            return false;
        }
        if (FindStorePublisher(out, publisher.id)) {
            error = L"catalog: duplicate publisher";
            return false;
        }
        out.publishers.push_back(std::move(publisher));
    }
    return true;
}

bool ParseVersions(const json::Value& item, StoreExtension& extension,
                   std::wstring& error) {
    const json::Value* versions = item.find("versions");
    if (!versions || !versions->is_array() || versions->size() == 0 ||
        versions->size() > 32) {
        error = L"catalog: extension has no versions";
        return false;
    }
    for (const auto& version_item : versions->items()) {
        if (!version_item.is_object()) {
            error = L"catalog: invalid release";
            return false;
        }
        StoreRelease release;
        release.version = Field(version_item, "version");
        release.released = Field(version_item, "released");
        release.min_luling = Field(version_item, "minLuling");
        release.manifest_sha256 = Field(version_item, "manifestSha256");
        release.notes = Field(version_item, "notes");
        if (release.version.empty() || !ValidHex64(release.manifest_sha256) ||
            !AssetField(version_item, "package", true, release.package) ||
            !AssetField(version_item, "signature", false, release.signature)) {
            error = L"catalog: malformed release";
            return false;
        }
        extension.versions.push_back(std::move(release));
    }
    return true;
}

bool ParseCatalog(std::string_view body, StoreCatalogSnapshot& out,
                  std::wstring& error) {
    json::Value root;
    if (!json::Parse(body, root, nullptr) || !root.is_object()) {
        error = L"catalog: not a JSON object";
        return false;
    }
    int64_t format = 0;
    const json::Value* format_value = root.find("formatVersion");
    if (!format_value || !format_value->get_int(format, 1, 1)) {
        error = L"catalog: unsupported format";
        return false;
    }
    if (!ParsePublishers(root, out, error)) return false;

    const json::Value* list = root.find("extensions");
    if (!list || !list->is_array() || list->size() > kMaxExtensions) {
        error = L"catalog: invalid extensions";
        return false;
    }
    for (const auto& item : list->items()) {
        if (!item.is_object()) {
            error = L"catalog: invalid extension entry";
            return false;
        }
        StoreExtension extension;
        extension.id = Field(item, "id");
        extension.publisher = Field(item, "publisher");
        extension.name = Field(item, "name");
        extension.summary = Field(item, "summary");
        extension.description = Field(item, "description");
        extension.category = Field(item, "category");
        extension.license = Field(item, "license");
        extension.homepage = Field(item, "homepage");
        extension.source = Field(item, "source");
        if (const json::Value* tags = item.find("tags"); tags && tags->is_array()) {
            for (const auto& tag : tags->items())
                if (tag.is_string()) extension.tags.push_back(json::Wide(tag.as_string()));
        }
        if (!ValidId(extension.id) || extension.name.empty() ||
            !FindStorePublisher(out, extension.publisher)) {
            error = L"catalog: malformed extension";
            return false;
        }
        for (const auto& existing : out.extensions) {
            if (existing.id == extension.id) {
                error = L"catalog: duplicate extension";
                return false;
            }
        }
        if (!ParseVersions(item, extension, error)) return false;
        out.extensions.push_back(std::move(extension));
    }
    return true;
}

bool FetchText(std::wstring_view url, uint64_t maximum, std::string& out) {
    std::atomic<bool> cancelled{false};
    UpdateError category = UpdateError::None;
    DWORD code = 0;
    return ReadUpdateWithFallback(
        url, maximum, cancelled,
        [&]() {
            out.clear();
            return true;
        },
        [&](const void* data, DWORD size) {
            out.append(static_cast<const char*>(data), size);
            return true;
        },
        category, code);
}

bool DownloadToFile(std::wstring_view url, const std::wstring& path, uint64_t maximum,
                    std::wstring& error) {
    std::atomic<bool> cancelled{false};
    UpdateError category = UpdateError::None;
    DWORD code = 0;
    FILE* file = nullptr;
    const auto reset = [&]() {
        if (file) {
            fclose(file);
            file = nullptr;
        }
        return _wfopen_s(&file, path.c_str(), L"wb") == 0 && file != nullptr;
    };
    if (!reset()) {
        error = L"could not create the download file";
        return false;
    }
    const bool ok = ReadUpdateWithFallback(
        url, maximum, cancelled, reset,
        [&](const void* data, DWORD size) {
            return file && fwrite(data, 1, size, file) == size;
        },
        category, code);
    if (file) {
        if (fflush(file) != 0) {
            fclose(file);
            file = nullptr;
            error = L"could not flush the download";
            return false;
        }
        fclose(file);
    }
    if (!ok) error = L"download failed";
    return ok;
}

bool ExtractZip(const std::wstring& zip, const std::wstring& destination,
                std::wstring& error) {
    // Windows ships bsdtar, which reads zip; it is the one extractor that needs
    // no extra DLL and no Shell UI.
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, ARRAYSIZE(system))) {
        error = L"no system directory";
        return false;
    }
    const std::wstring tar = std::wstring(system) + L"\\tar.exe";
    if (GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"tar.exe is unavailable";
        return false;
    }
    std::wstring command =
        L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + destination + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, destination.c_str(), &si, &pi)) {
        error = L"could not start tar.exe";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code != 0) {
        error = L"the package could not be extracted";
        return false;
    }
    return true;
}

bool FileSize(const std::wstring& path, uint64_t& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    out = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
}

bool VerifyFileAsset(const std::wstring& path, const StoreAsset& asset,
                     std::wstring& error) {
    uint64_t size = 0;
    if (!FileSize(path, size) || size != asset.size) {
        error = L"downloaded size does not match the catalog";
        return false;
    }
    std::wstring digest;
    if (!Sha256FileHex(path, digest) || _wcsicmp(digest.c_str(), asset.sha256.c_str()) != 0) {
        error = L"downloaded checksum does not match the catalog";
        return false;
    }
    return true;
}

struct InstalledEntry {
    std::wstring id;
    std::wstring version;
    std::wstring publisher;
};

std::vector<InstalledEntry> ReadInstalled() {
    std::vector<InstalledEntry> out;
    std::string body;
    if (!ReadAllBytes(InstalledPath(), body)) return out;
    json::Value root;
    if (!json::Parse(body, root, nullptr) || !root.is_object()) return out;
    const json::Value* list = root.find("installed");
    if (!list || !list->is_array()) return out;
    for (const auto& item : list->items()) {
        if (!item.is_object()) continue;
        InstalledEntry entry;
        entry.id = Field(item, "id");
        entry.version = Field(item, "version");
        entry.publisher = Field(item, "publisher");
        if (!entry.id.empty()) out.push_back(std::move(entry));
    }
    return out;
}

void RecordInstalled(const StoreExtension& extension, const StoreRelease& release) {
    auto entries = ReadInstalled();
    bool updated = false;
    for (auto& entry : entries) {
        if (entry.id != extension.id) continue;
        entry.version = release.version;
        entry.publisher = extension.publisher;
        updated = true;
        break;
    }
    if (!updated)
        entries.push_back({extension.id, release.version, extension.publisher});
    json::Value root = json::Value::Object();
    root.set("version", json::Value(1));
    json::Value list = json::Value::Array();
    for (const auto& entry : entries) {
        json::Value item = json::Value::Object();
        item.set("id", json::Value(json::Utf8(entry.id)));
        item.set("version", json::Value(json::Utf8(entry.version)));
        item.set("publisher", json::Value(json::Utf8(entry.publisher)));
        list.push(std::move(item));
    }
    root.set("installed", std::move(list));
    CreateDirectoryW(ExtensionsRoot().c_str(), nullptr);
    WriteAllBytes(InstalledPath(), json::Serialize(root));
}

bool InstallOne(const StoreExtension& extension, const StorePublisher& publisher,
                std::wstring& error) {
    const StoreRelease* release = extension.Newest();
    if (!release) {
        error = L"catalog entry has no release";
        return false;
    }
    if (!IsPublisherTrusted(extension.publisher)) {
        error = L"publisher is not trusted";
        return false;
    }
    // The publisher record and the extension must agree, and the pinned key
    // must be the official one.
    std::string derived;
    if (!PublisherIdFromPublicKey(json::Utf8(publisher.public_key), derived) ||
        derived != json::Utf8(publisher.id) || publisher.id != extension.publisher) {
        error = L"publisher key does not match the catalog";
        return false;
    }

    const std::wstring root = ExtensionsRoot();
    const std::wstring staging = root + L"\\.staging";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(staging.c_str(), nullptr);
    const std::wstring work = staging + L"\\" + extension.id + L"." + release->version +
        L"." + std::to_wstring(GetTickCount64());
    std::error_code ec;
    std::filesystem::create_directories(work, ec);
    if (ec) {
        error = L"could not create the staging directory";
        return false;
    }
    const std::wstring package_path = work + L"\\package.zip";
    const std::wstring signature_path = work + L"\\package.sig.json";
    const std::wstring unpacked = work + L"\\unpacked";
    std::filesystem::create_directories(unpacked, ec);

    bool ok = DownloadToFile(release->package.url, package_path, kMaxPackageBytes, error) &&
        VerifyFileAsset(package_path, release->package, error);
    if (ok)
        ok = DownloadToFile(release->signature.url, signature_path, kMaxSignatureBytes, error) &&
            VerifyFileAsset(signature_path, release->signature, error);
    if (ok) ok = ExtractZip(package_path, unpacked, error);

    // The publisher signs manifest.json itself; the catalog only certifies the
    // package hash. Both therefore describe the same bytes.
    std::string manifest;
    if (ok && !ReadAllBytes(unpacked + L"\\manifest.json", manifest)) {
        error = L"the package has no manifest.json";
        ok = false;
    }
    if (ok) {
        std::string digest;
        if (!Sha256BytesHex(manifest.data(), manifest.size(), digest) ||
            _stricmp(digest.c_str(), json::Utf8(release->manifest_sha256).c_str()) != 0) {
            error = L"the manifest does not match the catalog";
            ok = false;
        }
    }
    if (ok) {
        std::string signature_body;
        SignatureDocument document;
        if (!ReadAllBytes(signature_path, signature_body) ||
            !ParseSignatureDocument(signature_body, document, error)) {
            if (error.empty()) error = L"the signature could not be read";
            ok = false;
        } else if (document.publisher != extension.publisher) {
            error = L"the signing publisher does not match the catalog";
            ok = false;
        } else if (!VerifyStoreSignature(json::Utf8(publisher.public_key),
                                         kManifestSignatureDomain, manifest,
                                         json::Utf8(document.signature))) {
            error = L"the publisher signature is invalid";
            ok = false;
        }
    }
    if (ok) {
        json::Value manifest_root;
        if (!json::Parse(manifest, manifest_root, nullptr) || !manifest_root.is_object()) {
            error = L"the manifest is not JSON";
            ok = false;
        } else {
            const std::wstring id = Field(manifest_root, "id");
            const std::wstring version = Field(manifest_root, "version");
            if (id != extension.id) {
                error = L"the manifest id does not match the catalog";
                ok = false;
            } else if (!version.empty() && version != release->version) {
                error = L"the manifest version does not match the catalog";
                ok = false;
            }
        }
    }

    if (ok) {
        const std::wstring target = root + L"\\" + extension.id;
        const std::wstring backup = root + L"\\." + extension.id + L".old";
        std::filesystem::remove_all(backup, ec);
        const bool had_old = std::filesystem::exists(target, ec);
        if (had_old && !MoveFileExW(target.c_str(), backup.c_str(), 0)) {
            error = L"could not replace the installed extension";
            ok = false;
        }
        if (ok && !MoveFileExW(unpacked.c_str(), target.c_str(), 0)) {
            if (had_old) MoveFileExW(backup.c_str(), target.c_str(), 0);
            error = L"could not move the extension into place";
            ok = false;
        }
        if (ok) std::filesystem::remove_all(backup, ec);
    }
    if (ok) RecordInstalled(extension, *release);
    std::filesystem::remove_all(work, ec);
    return ok;
}

bool InstallFromFile(const std::wstring& zip, std::wstring& error) {
    const std::wstring root = ExtensionsRoot();
    const std::wstring staging = root + L"\\.staging";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(staging.c_str(), nullptr);
    const std::wstring work = staging + L"\\file." + std::to_wstring(GetTickCount64());
    std::error_code ec;
    std::filesystem::create_directories(work, ec);
    if (ec) {
        error = L"could not create the staging directory";
        return false;
    }
    const std::wstring unpacked = work + L"\\unpacked";
    std::filesystem::create_directories(unpacked, ec);
    if (!ExtractZip(zip, unpacked, error)) {
        std::filesystem::remove_all(work, ec);
        return false;
    }
    std::string manifest;
    if (!ReadAllBytes(unpacked + L"\\manifest.json", manifest)) {
        error = L"the package has no manifest.json";
        std::filesystem::remove_all(work, ec);
        return false;
    }
    json::Value document;
    if (!json::Parse(manifest, document, nullptr) || !document.is_object()) {
        error = L"the manifest is not JSON";
        std::filesystem::remove_all(work, ec);
        return false;
    }
    const std::wstring id = Field(document, "id");
    const std::wstring version = Field(document, "version");
    if (!ValidId(id)) {
        error = L"the manifest has no valid id";
        std::filesystem::remove_all(work, ec);
        return false;
    }
    const std::wstring target = root + L"\\" + id;
    const std::wstring backup = root + L"\\." + id + L".old";
    std::filesystem::remove_all(backup, ec);
    const bool had_old = std::filesystem::exists(target, ec);
    if (had_old && !MoveFileExW(target.c_str(), backup.c_str(), 0)) {
        error = L"could not replace the installed extension";
        std::filesystem::remove_all(work, ec);
        return false;
    }
    if (!MoveFileExW(unpacked.c_str(), target.c_str(), 0)) {
        if (had_old) MoveFileExW(backup.c_str(), target.c_str(), 0);
        error = L"could not move the extension into place";
        std::filesystem::remove_all(work, ec);
        return false;
    }
    std::filesystem::remove_all(backup, ec);
    StoreExtension extension;
    extension.id = id;
    StoreRelease release;
    release.version = version;
    RecordInstalled(extension, release);
    std::filesystem::remove_all(work, ec);
    return true;
}

} // namespace

StoreCatalogSnapshot StoreCatalogSnapshotCopy() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snapshot;
}

const StorePublisher* FindStorePublisher(const StoreCatalogSnapshot& snapshot,
                                         std::wstring_view id) {
    for (const auto& publisher : snapshot.publishers)
        if (publisher.id == id) return &publisher;
    return nullptr;
}

const StorePublisher* FindExtensionPublisher(const StoreCatalogSnapshot& snapshot,
                                             const StoreExtension& extension) {
    return FindStorePublisher(snapshot, extension.publisher);
}

std::wstring InstalledStoreExtensionVersion(const std::wstring& id) {
    for (const auto& entry : ReadInstalled())
        if (entry.id == id) return entry.version;
    return {};
}

std::wstring StoreInstallError() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_install_error;
}

bool VerifyCatalogDocument(std::string_view index, std::string_view signature_document,
                           std::wstring& error) {
    SignatureDocument document;
    if (!ParseSignatureDocument(signature_document, document, error)) return false;
    if (json::Utf8(document.publisher) != kCatalogPublisherId) {
        error = L"catalog: unknown signer";
        return false;
    }
    if (!VerifyStoreSignature(kCatalogPublicKeyHex, kCatalogSignatureDomain, index,
                              json::Utf8(document.signature))) {
        error = L"catalog: signature invalid";
        return false;
    }
    return true;
}

void UninstallExtension(const std::wstring& id) {
    std::wstring error;
    UninstallExtensionFrom(ExtensionsRoot(), id, error);
}

void StartStoreCatalogFetch(HWND notify) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_snapshot.state == StoreCatalogState::Loading) return;
        g_snapshot.state = StoreCatalogState::Loading;
    }
    if (notify) PostMessageW(notify, WM_STORE_CATALOG, 0, 0);
    std::thread([notify] {
        std::string index;
        std::string signature;
        std::wstring error;
        const bool fetched = FetchText(kCatalogIndexUrl, kMaxCatalogBytes, index) &&
            FetchText(kCatalogSignatureUrl, kMaxSignatureBytes, signature);

        StoreCatalogSnapshot snapshot;
        bool loaded = false;
        if (fetched) {
            loaded = VerifyCatalogDocument(index, signature, error) &&
                ParseCatalog(index, snapshot, error);
            if (loaded) {
                snapshot.verified = true;
                WriteAllBytes(CatalogCachePath(), index);
                WriteAllBytes(SignatureCachePath(), signature);
            }
        }
        if (!loaded) {
            // Network or verification failure: the last verified copy is the
            // only thing shown, and it is re-verified before use.
            std::string cached_index;
            std::string cached_signature;
            std::wstring cache_error;
            StoreCatalogSnapshot cached;
            if (ReadAllBytes(CatalogCachePath(), cached_index) &&
                ReadAllBytes(SignatureCachePath(), cached_signature) &&
                VerifyCatalogDocument(cached_index, cached_signature, cache_error) &&
                ParseCatalog(cached_index, cached, cache_error)) {
                cached.verified = true;
                cached.from_cache = true;
                snapshot = std::move(cached);
                loaded = true;
            } else if (error.empty()) {
                error = L"catalog: unavailable";
            }
        }
        if (loaded) {
            snapshot.state = StoreCatalogState::Ready;
            snapshot.error.clear();
        } else {
            snapshot.state = StoreCatalogState::Offline;
            snapshot.error = error;
            snapshot.publishers.clear();
            snapshot.extensions.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_snapshot = std::move(snapshot);
        }
        if (notify) PostMessageW(notify, WM_STORE_CATALOG, 0, 0);
    }).detach();
}

void StartStoreExtensionInstall(HWND notify, StoreExtension extension,
                                StorePublisher publisher) {
    if (!ValidId(extension.id) || !extension.Newest()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_install_error.clear();
    }
    std::thread([notify, extension = std::move(extension), publisher = std::move(publisher)] {
        std::wstring error;
        const bool ok = InstallOne(extension, publisher, error);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_install_error = ok ? std::wstring() : error;
        }
        if (notify) PostMessageW(notify, WM_STORE_INSTALLED, ok ? 1 : 0, 0);
    }).detach();
}

void StartStoreExtensionInstallFromFile(HWND notify, std::wstring zip_path) {
    if (zip_path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_install_error.clear();
    }
    std::thread([notify, zip_path = std::move(zip_path)] {
        std::wstring error;
        const bool ok = InstallFromFile(zip_path, error);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_install_error = ok ? std::wstring() : error;
        }
        if (notify) PostMessageW(notify, WM_STORE_INSTALLED, ok ? 1 : 0, 0);
    }).detach();
}

} // namespace luling::app
