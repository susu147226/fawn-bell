// fs_recycle.h — Enumerate the per-user Recycle Bin without Shell COM.
#pragma once
#include "fs_enum.h"
#include <cstdint>
#include <string>
#include <vector>

namespace luling::fs {

struct RecycleBinInfo {
    uint64_t bytes = 0;
    uint64_t items = 0;
    bool valid = false;
};

struct RecycleItem {
    std::wstring name;
    std::wstring original_path;
    std::wstring content_path; // $R* payload
    std::wstring index_path;   // $I* metadata
    uint64_t size = 0;
    FILETIME deleted{};
    bool is_dir = false;
};

bool QueryRecycleBinInfo(RecycleBinInfo& out);
bool ReadRecycleIndex(const std::wstring& index_path, RecycleItem& out);
std::wstring RecycleIndexPath(const std::wstring& content_path);
// Append live items belonging to the current user under one $Recycle.Bin root.
// Missing directories are empty; access/enumeration failures return false.
bool EnumerateRecycleBinAtRoot(const std::wstring& recycle_root, std::vector<DirEntry>& out);
void EnumerateRecycleBin(std::vector<DirEntry>& out, RecycleBinInfo* info = nullptr);

// Whether the Shell can move an item at `path` into a Recycle Bin. Network
// shares and anything that is not a fixed disk have nowhere to put it, so
// Explorer deletes there permanently instead of recycling — callers must do the
// same rather than refuse the delete.
bool CanRecyclePath(const std::wstring& path);
// Same verdict, but a volume lookup failure is reported through volume_error
// instead of being treated as "no Recycle Bin": a transient error must not
// silently turn a delete permanent.
bool RecycleBinAvailable(const std::wstring& path, DWORD* volume_error = nullptr);

inline bool IsRecycleViewPath(const std::wstring& path) {
    return path == L"luling:recycle";
}

} // namespace luling::fs
