// shell_namespace.h — Shell namespace paths for devices that are not drive
// letters (MTP phones, cameras). A path is the absolute PIDL encoded as hex
// behind "luling:shell:"; enumeration goes through IShellFolder.
#pragma once
#include "fs_enum.h"
#include <windows.h>
#include <shtypes.h>

#include <atomic>
#include <string>
#include <vector>

namespace luling::fs {

bool IsShellPath(const std::wstring& path);
std::wstring MakeShellPath(PCIDLIST_ABSOLUTE pidl);
// Throws std::runtime_error on a malformed path or a failed bind.
void EnumerateShellDirectory(const std::wstring& path, std::vector<DirEntry>& out,
                             const std::atomic<bool>* cancelled = nullptr);
// Adds non-filesystem devices under This PC (phones and cameras on MTP).
void AppendPortableDevices(std::vector<DirEntry>& out);
// Parent path, or empty for "This PC".
std::wstring ShellParentPath(const std::wstring& path);
std::wstring ShellDisplayName(const std::wstring& path);
bool ShellOpen(const std::wstring& path);
// Device namespace items have no file path; renaming and deleting must go
// through IFileOperation.
bool ShellRename(const std::wstring& path, const std::wstring& new_name);
bool ShellDelete(const std::vector<std::wstring>& paths);
// IFileOperation helpers. All return false when the operation was aborted or
// the paths could not be resolved.
bool ShellCopyToLocal(const std::vector<std::wstring>& shell_sources,
                      const std::wstring& local_dir, bool move);
bool ShellCopyFromLocal(const std::vector<std::wstring>& local_sources,
                        const std::wstring& shell_dest, bool move);
bool ShellCopyWithin(const std::vector<std::wstring>& shell_sources,
                     const std::wstring& shell_dest, bool move);

} // namespace luling::fs
