#include "location_history.h"
#include "session.h"
#include "../common/json_utils.h"
#include "../common/path_utils.h"
#include "../common/utf8_file.h"
#include <algorithm>
#include <cwctype>

namespace luling::app {
namespace {
constexpr size_t kCapacity = 50;
constexpr size_t kMaxPath = 32768;
constexpr size_t kMaxFileBytes = 8 * 1024 * 1024;

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}
} // namespace

std::wstring LocationHistory::ToJson() const {
    std::wstring out = L"{\"paths\":[";
    bool first = true;
    for (const auto& path : entries) {
        std::wstring escaped;
        luling::json::Escape(path, escaped);
        if (!first) out += L",";
        out += L"\"" + escaped + L"\"";
        first = false;
    }
    out += L"]}";
    return out;
}

bool LocationHistory::FromJson(const std::wstring& json) {
    entries = luling::json::ExtractStringArray(json, L"paths");
    // Older builds stored the internal \\?\ form; keep the list clean.
    for (auto& path : entries) path = luling::path::StripExtendedPathPrefix(path);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const std::wstring& path) {
                                     return path.empty() || path.size() > kMaxPath;
                                 }),
                  entries.end());
    if (entries.size() > kCapacity) entries.resize(kCapacity);
    return true;
}

bool LocationHistory::Load() {
    if (!persist) return false;
    const auto dir = GetLulingDataDir();
    if (dir.empty()) return false;
    std::wstring text;
    if (!ReadUtf8File(dir + L"\\location_history.json", text)) return false;
    if (text.size() > kMaxFileBytes) return false;
    return FromJson(text);
}

bool LocationHistory::Save() const {
    if (!persist) return false;
    const auto dir = GetLulingDataDir();
    return !dir.empty() && WriteUtf8FileAtomic(dir + L"\\location_history.json", ToJson());
}

bool LocationHistory::Record(const std::wstring& path) {
    if (path.empty()) return false;
    const auto found = std::find_if(entries.begin(), entries.end(),
                                    [&](const std::wstring& entry) { return SamePath(entry, path); });
    if (found != entries.end()) {
        if (found == entries.begin()) return false; // already the newest
        entries.erase(found);
    }
    entries.insert(entries.begin(), path);
    if (entries.size() > kCapacity) entries.resize(kCapacity);
    if (persist) Save();
    return true;
}

bool LocationHistory::Clear() {
    if (entries.empty()) return false;
    entries.clear();
    if (persist) Save();
    return true;
}

} // namespace luling::app
