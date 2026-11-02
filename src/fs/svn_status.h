#pragma once
// svn_status.h - per-name SVN working-copy state for one directory listing.
// A cached probe drives `svn status` off the UI thread; a listing read never
// waits for the client.
#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace luling::fs {

enum class SvnItem : uint8_t {
    None = 0,
    Modified,     // M
    Added,        // A
    Deleted,      // D
    Unversioned,  // ?
    Conflicted,   // C
    Ignored,      // I
    Replaced,     // R
    Missing,      // !
};

struct SvnStatusMap {
    bool in_working_copy = false;
    std::unordered_map<std::wstring, SvnItem> items; // lowercase file name -> item
};

// True when an svn client (LULING_SVN_PATH or svn.exe on PATH) is available.
// The answer is looked up once and cached.
bool SvnClientAvailable();

// Returns the last cached answer for directory immediately. When the cache
// is missing or older than a few seconds, a background refresh is queued and
// WM_SVN_STATUS is posted to `notify` when it lands.
SvnStatusMap QuerySvnStatus(const std::wstring& directory, HWND notify);

} // namespace luling::fs
