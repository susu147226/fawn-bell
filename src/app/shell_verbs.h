// shell_verbs.h — Registry-backed static context-menu verbs (优化.md §7.2.1).
//
// HKCR\<progid>\shell verbs (打印/编辑/…) and the OpenWithList MRU are plain
// registry data, so they go into the context menu's first frame without
// waiting for luling_shell's COM session. Invocation stays on the ops worker
// (ShellExecuteEx verb / explicit application launch).
//
// EnumerateStaticVerbs does registry reads plus version-info lookups for
// friendly app names: call it off the UI thread and cache per extension.
#pragma once
#include "../common/menu_icon.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace luling::app {

struct StaticVerb {
    std::wstring verb;      // registry verb key ("print", "edit", "openas")
    std::wstring display;   // menu row text
    std::wstring app_path;  // non-empty => open-with entry: launch this exe
    std::wstring command;   // raw HKCR command template; CommandStore children need this
    // The verb key's own Icon value ("file,index"), read where the verb was
    // enumerated (FileExts / ProgID / SystemFileAssociations / CommandStore).
    std::wstring icon;
    std::vector<StaticVerb> children; // cascade flyout (SubCommands / ExtendedSubCommandsKey)
};

// The icon to draw for a static verb row: its Icon value, else the program the
// row launches. Empty when neither exists.
bool StaticVerbIcon(const StaticVerb& verb, std::wstring& file, int& index);

// ext is ".dwg" (file), or a location key ":folder" / ":drive" / ":bg".
// File keys return progid/OpenWith verbs plus "打开方式…". Location keys
// return Directory/Folder/Drive/Background shell verbs, including cascades.
std::vector<StaticVerb> EnumerateStaticVerbs(const std::wstring& ext);

// Pure helper (self-tested): dedupes by display text, drops entries matching
// any built-in row text, caps the list.
std::vector<StaticVerb> DedupeStaticVerbs(std::vector<StaticVerb> verbs,
                                          const std::vector<std::wstring>& builtin_texts,
                                          size_t cap);

common::MenuIconRef FileTypeMenuIcon(const std::wstring& ext);

// A program's display name: Applications\<exe>\FriendlyAppName, else the
// version resource's FileDescription, else the file name. Reads the file.
std::wstring ProgramFriendlyName(const std::wstring& exe_path);
// Icon pixels for "file[,index]" at the menu's 16 DIP, through the same
// process-wide cache as the open-with rows. Touches the file: worker threads.
common::MenuIconRef ProgramMenuIcon(const std::wstring& reference);

std::wstring MachineStaticVerbCachePath();
bool LoadMachineStaticVerbCache(std::unordered_map<std::wstring, std::vector<StaticVerb>>& out);
bool SaveMachineStaticVerbCache(
    const std::unordered_map<std::wstring, std::vector<StaticVerb>>& cache);
// Walks HKCR file extensions and writes ProgramData\Luling\shell-verbs.bin.
// Safe to run from the installer (elevated) or Luling.exe --seed-shell-verbs.
bool SeedMachineStaticVerbCache();

} // namespace luling::app
