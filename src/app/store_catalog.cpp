#include "store_catalog.h"

#include "app_messages.h"
#include "session.h"
#include "update_transport.h"
#include "../common/json_value.h"

#include <atomic>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <thread>

namespace luling::app {
namespace {

// Catalog lives in the Luling repository; a fork or mirror can change this one
// line. Raw GitHub is used because the file is small and updated independently
// of releases.
constexpr wchar_t kCatalogUrl[] =
    L"https://raw.githubusercontent.com/susu147226/fawn-bell/main/extensions/catalog.json";
constexpr uint64_t kMaxCatalogBytes = 1u << 20;   // 1 MiB
constexpr uint64_t kMaxPackageBytes = 512u << 20; // 512 MiB

std::mutex g_mutex;
StoreCatalogSnapshot g_snapshot;
std::wstring g_install_error;

bool ValidId(const std::wstring& id) {
    if (id.empty() || id.size() > 64) return false;
    for (wchar_t c : id) {
        if (std::iswalnum(c) || c == L'-' || c == L'_' || c == L'.') continue;
        return false;
    }
    return true;
}

bool ParseCatalog(const std::string& body, std::vector<StoreExtension>& out) {
    json::Value root;
    if (!json::Parse(body, root, nullptr) || !root.is_object()) return false;
    const json::Value* list = root.find("extensions");
    if (!list || !list->is_array()) return false;
    for (const auto& item : list->items()) {
        if (!item.is_object()) continue;
        StoreExtension ext;
        ext.id = json::Wide(item.get_string("id"));
        ext.name = json::Wide(item.get_string("name"));
        ext.desc = json::Wide(item.get_string("desc"));
        ext.version = json::Wide(item.get_string("version"));
        ext.url = json::Wide(item.get_string("url"));
        ext.sha256 = json::Wide(item.get_string("sha256"));
        const double size = item.find("size") ? item.find("size")->as_number(0.0) : 0.0;
        if (size > 0.0) ext.size = static_cast<uint64_t>(size);
        if (!ValidId(ext.id) || ext.name.empty() || ext.url.empty()) continue;
        out.push_back(std::move(ext));
    }
    return true;
}

std::wstring CatalogCachePath() { return GetLulingDataDir() + L"\\extensions_catalog.json"; }
std::wstring ExtensionsRoot() { return GetLulingDataDir() + L"\\extensions"; }
std::wstring InstalledPath() { return ExtensionsRoot() + L"\\installed.json"; }

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

struct InstalledEntry {
    std::wstring id;
    std::wstring version;
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
        entry.id = json::Wide(item.get_string("id"));
        entry.version = json::Wide(item.get_string("version"));
        if (!entry.id.empty()) out.push_back(std::move(entry));
    }
    return out;
}

void RecordInstalled(const StoreExtension& ext) {
    auto entries = ReadInstalled();
    bool updated = false;
    for (auto& entry : entries) {
        if (entry.id != ext.id) continue;
        entry.version = ext.version;
        updated = true;
        break;
    }
    if (!updated) entries.push_back({ext.id, ext.version});
    json::Value root = json::Value::Object();
    root.set("version", json::Value(1));
    json::Value list = json::Value::Array();
    for (const auto& entry : entries) {
        json::Value item = json::Value::Object();
        item.set("id", json::Value(json::Utf8(entry.id)));
        item.set("version", json::Value(json::Utf8(entry.version)));
        list.push(std::move(item));
    }
    root.set("installed", std::move(list));
    const std::string text = json::Serialize(root);
    CreateDirectoryW(ExtensionsRoot().c_str(), nullptr);
    WriteAllBytes(InstalledPath(), text);
}

bool ExtractZip(const std::wstring& zip, const std::wstring& dest) {
    // Windows ships bsdtar, which reads zip; it is the one extractor that needs
    // no extra DLL and no Shell UI.
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, ARRAYSIZE(system))) return false;
    const std::wstring tar = std::wstring(system) + L"\\tar.exe";
    if (GetFileAttributesW(tar.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::wstring command = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dest + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, dest.c_str(), &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exit_code == 0;
}

} // namespace

StoreCatalogSnapshot StoreCatalogSnapshotCopy() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snapshot;
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

void StartStoreCatalogFetch(HWND notify) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_snapshot.state == StoreCatalogState::Loading) return;
        g_snapshot.state = StoreCatalogState::Loading;
    }
    if (notify) PostMessageW(notify, WM_STORE_CATALOG, 0, 0);
    std::thread([notify] {
        std::string body;
        std::atomic<bool> cancelled{false};
        UpdateError error = UpdateError::None;
        DWORD code = 0;
        const bool fetched = ReadUpdateResponse(kCatalogUrl, kMaxCatalogBytes, cancelled,
            [&](const void* data, DWORD size) {
                body.append(static_cast<const char*>(data), size);
                return true;
            }, error, code);
        StoreCatalogSnapshot snapshot;
        if (fetched && !body.empty() && ParseCatalog(body, snapshot.extensions)) {
            snapshot.state = StoreCatalogState::Ready;
            WriteAllBytes(CatalogCachePath(), body);
        } else {
            // No network (or no catalog yet): keep the last good copy.
            std::string cached;
            if (ReadAllBytes(CatalogCachePath(), cached) &&
                ParseCatalog(cached, snapshot.extensions)) {
                snapshot.state = StoreCatalogState::Ready;
                snapshot.from_cache = true;
            } else {
                snapshot.state = StoreCatalogState::Offline;
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_snapshot = std::move(snapshot);
        }
        if (notify) PostMessageW(notify, WM_STORE_CATALOG, 0, 0);
    }).detach();
}

void StartStoreExtensionInstall(HWND notify, StoreExtension extension) {
    if (!ValidId(extension.id) || extension.url.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_install_error.clear();
    }
    std::thread([notify, extension = std::move(extension)] {
        const std::wstring dir = ExtensionsRoot() + L"\\" + extension.id;
        CreateDirectoryW(ExtensionsRoot().c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring zip = dir + L"\\package.zip";
        bool ok = false;
        FILE* file = nullptr;
        if (_wfopen_s(&file, zip.c_str(), L"wb") == 0 && file) {
            std::atomic<bool> cancelled{false};
            UpdateError error = UpdateError::None;
            DWORD code = 0;
            ok = ReadUpdateResponse(extension.url, kMaxPackageBytes, cancelled,
                [&](const void* data, DWORD size) {
                    return fwrite(data, 1, size, file) == size;
                }, error, code);
            fclose(file);
        }
        if (ok) ok = ExtractZip(zip, dir);
        DeleteFileW(zip.c_str());
        if (ok) {
            RecordInstalled(extension);
        } else {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_install_error = L"download or extract failed";
        }
        if (notify) PostMessageW(notify, WM_STORE_INSTALLED, ok ? 1 : 0, 0);
    }).detach();
}

} // namespace luling::app
