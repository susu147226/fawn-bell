#include "store_trust.h"

#include "session.h"
#include "store_signature.h"
#include "../common/json_value.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <mutex>

namespace luling::app {
namespace {

std::mutex g_mutex;

std::wstring TrustPath() { return GetLulingDataDir() + L"\\extensions\\trust.json"; }
std::wstring ExtensionsRoot() { return GetLulingDataDir() + L"\\extensions"; }

bool ReadAllBytes(const std::wstring& path, std::string& out) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
    char buffer[16384];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, n);
    fclose(file);
    return !out.empty();
}

bool WriteAtomically(const std::wstring& path, std::string_view bytes) {
    const std::wstring temporary = path + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file) return false;
    const size_t written = fwrite(bytes.data(), 1, bytes.size(), file);
    const bool flushed = fflush(file) == 0;
    fclose(file);
    if (written != bytes.size() || !flushed) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

std::vector<TrustedPublisher> Load() {
    std::vector<TrustedPublisher> out;
    std::string body;
    if (!ReadAllBytes(TrustPath(), body)) return out;
    json::Value root;
    if (!json::Parse(body, root, nullptr) || !root.is_object()) return out;
    const json::Value* list = root.find("publishers");
    if (!list || !list->is_array()) return out;
    for (const auto& item : list->items()) {
        if (!item.is_object()) continue;
        TrustedPublisher publisher;
        publisher.id = json::Wide(item.get_string("id"));
        publisher.name = json::Wide(item.get_string("name"));
        publisher.public_key = json::Wide(item.get_string("publicKey"));
        if (publisher.id.empty() || publisher.public_key.empty()) continue;
        out.push_back(std::move(publisher));
    }
    return out;
}

bool Save(const std::vector<TrustedPublisher>& publishers) {
    json::Value root = json::Value::Object();
    root.set("version", json::Value(1));
    json::Value list = json::Value::Array();
    for (const auto& publisher : publishers) {
        json::Value item = json::Value::Object();
        item.set("id", json::Value(json::Utf8(publisher.id)));
        item.set("name", json::Value(json::Utf8(publisher.name)));
        item.set("publicKey", json::Value(json::Utf8(publisher.public_key)));
        list.push(std::move(item));
    }
    root.set("publishers", std::move(list));
    CreateDirectoryW(ExtensionsRoot().c_str(), nullptr);
    return WriteAtomically(TrustPath(), json::Serialize(root));
}

} // namespace

std::vector<TrustedPublisher> TrustedPublishers() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return Load();
}

bool IsPublisherTrusted(std::wstring_view id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& publisher : Load())
        if (publisher.id == id) return true;
    return false;
}

bool FindTrustedPublisher(std::wstring_view id, TrustedPublisher& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& publisher : Load()) {
        if (publisher.id != id) continue;
        out = publisher;
        return true;
    }
    return false;
}

bool TrustPublisher(const TrustedPublisher& publisher, std::wstring& error) {
    if (publisher.id.empty() || publisher.public_key.empty()) {
        error = L"invalid publisher";
        return false;
    }
    // The id is what everything else binds to, so it must really be this key.
    std::string expected;
    if (!PublisherIdFromPublicKey(json::Utf8(publisher.public_key), expected) ||
        expected != json::Utf8(publisher.id)) {
        error = L"publisher id does not match its key";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto publishers = Load();
    for (auto& existing : publishers) {
        if (existing.id != publisher.id) continue;
        if (_wcsicmp(existing.public_key.c_str(), publisher.public_key.c_str()) != 0) {
            error = L"publisher key changed";
            return false;
        }
        if (existing.name != publisher.name) {
            existing.name = publisher.name;
            if (!Save(publishers)) {
                error = L"could not save trust";
                return false;
            }
        }
        return true;
    }
    publishers.push_back(publisher);
    if (!Save(publishers)) {
        error = L"could not save trust";
        return false;
    }
    return true;
}

bool UntrustPublisher(std::wstring_view id, std::wstring& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto publishers = Load();
    const auto before = publishers.size();
    publishers.erase(std::remove_if(publishers.begin(), publishers.end(),
                                    [&](const TrustedPublisher& publisher) {
                                        return publisher.id == id;
                                    }),
                     publishers.end());
    if (publishers.size() == before) return true;
    if (!Save(publishers)) {
        error = L"could not save trust";
        return false;
    }
    return true;
}

} // namespace luling::app
