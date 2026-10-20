// shell_namespace.h — Shell namespace paths for devices that are not drive
// letters (MTP phones, cameras). A path is the absolute PIDL encoded as hex
// behind "luling:shell:"; enumeration goes through IShellFolder.
#pragma once
#include "fs_enum.h"
#include <windows.h>
#include <objidl.h>
#include <shobjidl.h>
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
// Copy one item under a caller-chosen name. Used for "keep both": staging the
// item and uploading it as a fresh name keeps the device out of its conflict
// path, which is where MTP copies used to end the process.
bool ShellCopyToLocalAs(const std::wstring& shell_source, const std::wstring& local_dir,
                        const std::wstring& name, bool move);
bool ShellCopyFromLocalAs(const std::wstring& local_source, const std::wstring& shell_dir,
                          const std::wstring& name, bool move);
bool ShellCopyWithinAs(const std::wstring& shell_source, const std::wstring& shell_dir,
                       const std::wstring& name, bool move);

// True when `name` already exists directly under the device folder `directory`.
bool ShellChildExists(const std::wstring& directory, const std::wstring& name);
// Shell path of that child, or empty when it does not exist.
std::wstring ShellChildPath(const std::wstring& directory, const std::wstring& name);

// Drag and drop: a Shell data object for device items, and conversion from a
// Shell item (including parsing names) to Luling's path form.
IDataObject* ShellCreateDragDataObject(const std::vector<std::wstring>& shell_paths);
std::wstring ShellPathFromItem(IShellItem* item);
std::wstring ShellPathFromParsingName(const std::wstring& parsing_name);

} // namespace luling::fs
