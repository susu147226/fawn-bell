#include "cloud_status.h"

#include "../common/localization.h"
#include "../common/windows_compat.h"

#include <mutex>
#include <unordered_map>

#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000
#endif

namespace luling::app {
namespace {
struct CacheEntry {
    ULONGLONG at = 0;
    std::wstring text;
};

std::mutex g_cache_mutex;
std::unordered_map<std::wstring, CacheEntry> g_cache;
constexpr ULONGLONG kStatusTtlMs = 5000;
constexpr int kSampleLimit = 64;

std::wstring SampleStatus(const std::wstring& root) {
    WIN32_FILE_ATTRIBUTE_DATA root_data{};
    if (!GetFileAttributesExW(root.c_str(), GetFileExInfoStandard, &root_data) ||
        (root_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return l10n::Get(l10n::StringId::CloudUnavailable);
    }
    WIN32_FIND_DATAW find_data{};
    const std::wstring pattern = root.back() == L'\\' ? root + L"*" : root + L"\\*";
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &find_data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return {};
    bool sampled = false;
    bool online_only = false;
    for (int i = 0; i < kSampleLimit; ++i) {
        const std::wstring name = find_data.cFileName;
        if (name != L"." && name != L"..") {
            sampled = true;
            if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0)
                online_only = true;
        }
        if (!FindNextFileW(find, &find_data)) break;
    }
    FindClose(find);
    if (!sampled) return {};
    return l10n::Get(online_only ? l10n::StringId::CloudOnlineOnly
                                 : l10n::StringId::CloudSynced);
}
}

std::wstring CloudFolderSyncStatus(const std::wstring& root) {
    if (root.empty()) return {};
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        const auto it = g_cache.find(root);
        if (it != g_cache.end() && now - it->second.at < kStatusTtlMs)
            return it->second.text;
    }
    std::wstring text = SampleStatus(root);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        g_cache[root] = CacheEntry{now, text};
    }
    return text;
}
}
