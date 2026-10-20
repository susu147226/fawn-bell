// context_menu.h — Built-in Fluent context menu verbs (stage 1B-2).
//
// Built-in verbs only; third-party IContextMenu stays in luling_shell and is
// out of scope for 1B-2 (plan.md §11 pit 6). Item construction is windowless
// so the console self-test can assert the verb list, hit-test and dispatch
// without a GUI.
#pragma once
#include "../ui/fluent_menu.h"
#include "../ui/view_layout.h"
#include "../ui/ui_renderer.h"
#include "context_menu_prefs.h"
#include "../index/index_engine.h"
#include <string>

namespace luling::app {

// Command ids returned by FluentMenu::TrackPopup for the built-in verbs.
enum MenuCmd : int {
    CmdNone = 0,
    CmdOpen = 1,
    CmdCut,
    CmdCopy,
    CmdPaste,
    CmdDelete,
    CmdRename,
    CmdProperties,
    CmdOpenTerminal,
    CmdCopyPath,        // Ctrl+Shift+C
    CmdUndo,            // Ctrl+Z
    CmdNewFolder,
    CmdNewTextFile,
    CmdCopyAsPath,      // unused alias guard — keep ids stable
    CmdOpenPath,        // search results: reveal in containing folder
    CmdOpenInNewTab,    // folder: open selection in a new tab
    CmdSelectAll,
    CmdInvertSelection,
    CmdSelectWildcard,
    CmdRefresh,
    CmdFolderProperties,
    CmdSortName,
    CmdSortModified,
    CmdSortType,
    CmdSortSize,
    CmdSortPath,
    CmdSortAscending,
    CmdSortDescending,
    CmdFolderSortTop,     // 文件夹优先：始终置顶
    CmdFolderSortFollow,  // 文件夹优先：跟随排序方向
    CmdFolderSortMixed,   // 文件夹优先：与文件混排
    CmdSortCreated = 31,  // 排序方式：创建日期
    CmdSortAccessed = 32, // 排序方式：访问日期
    CmdLayoutSingle = 50,
    CmdLayoutTwoVertical,
    CmdLayoutTwoHorizontal,
    CmdLayoutThree,
    CmdLayoutFourGrid,
    CmdCopyToTarget,
    CmdMoveToTarget,
    CmdPinWorkspace,
    CmdPinNetwork,
    CmdSearchAll,           // palette: open luling:search: as a folder
    CmdInstallFullIndex,    // palette: UAC-install LulingIndex service
    CmdSettings,            // palette / title-bar gear → settings tab
    CmdSettingsContextMenu, // 管理右键项… → 右键菜单 page
    CmdTags,
    CmdTagBase = 70,        // +0..6  Ctrl+Shift+1..7
    CmdViewBase = 80,       // + ViewModeIndex (8 stable view choices)
    CmdTabColorBase = 90,   // +0..7  tab-group color swatches
    CmdTabGroupNewTab = 98,
    CmdTabGroupUngroup,
    CmdTabGroupClose,
    CmdDetailsPanel,        // view menu: right details panel toggle
    CmdDetailsComputeSize,  // details "更多": compute folder size in place
    CmdDetailsShellMenu,    // details "更多": open the Explorer context menu
    CmdTabNewRight,         // tab menu: new tab to the right
    CmdTabDuplicate,        // tab menu: duplicate this tab
    CmdTabPin,              // tab menu: pin/unpin toggle
    CmdTabClose,            // tab menu: close this tab
    CmdTabAddToNewGroup,    // tab menu: create a group with this tab
    CmdTabRemoveFromGroup,  // tab menu: leave the group (group survives)
    CmdTabCloseOthers,
    CmdTabCloseRight,
    CmdTabRename,
    CmdTabNameSave,
    CmdTabNameReset,
    CmdTabColorNone,
    CmdTabJoinGroupBase = 130, // + index into WindowTabs::tab_groups (clear of 98-129)
    CmdEditStarBadge = 170,
    CmdRemoveStarred,
    CmdRemoveRecent,
    CmdBatchRename,
    CmdAdvancedSearch,
    CmdRestoreRecycle,
    CmdEmptyRecycle,
    CmdOpenRecycle,
    CmdPinQuickAccess,
    CmdUnpinQuickAccess,
    CmdViewRecentChanges = 180,
    CmdColumnLayout = 181,
    CmdShortcutHelp = 182,
    CmdCompareSideBySide = 183, // two folders selected: split + compare
    CmdCompareToggle = 184,     // split menu: compare the two panes
    CmdCompareDiffOnly = 185,   // split menu: differences only
    CmdGroupNone = 186,         // "group by" submenu: none / name / date / type / size
    CmdGroupName = 187,
    CmdGroupDate = 188,
    CmdGroupType = 189,
    CmdGroupSize = 190,
    CmdGroupTag = 191,          // = CmdGroupNone + GroupBy::Tag
    CmdGroupLocation = 192,     // = CmdGroupNone + GroupBy::Location
    CmdRestoreAllRecycle = 193, // recycle background: restore every listed item
    CmdApplyViewToAllFolders = 194, // view menu: this view + sort become every folder's default
    CmdApplyGroupToAllFolders = 195, // group menu: this grouping becomes every folder's default (#75)
    CmdExitLuling = 196,         // palette: quit even when closing keeps Luling running (#57)
    // Rows that the 右键菜单 settings page can hide/reorder (197-199 are the only
    // free ids below CmdRecentBase).
    CmdRenameSequenceZero = 197,
    CmdRenameSequenceOne = 198,
    CmdAppendNameSuffix = 199,
    CmdRecentBase = 200,
    CmdIndexBase = 1000,
    // Explorer integration (优化.md §7): registry static verbs bound to the
    // current selection (+index into the StaticVerb list) and luling_shell
    // IContextMenu session items (+host menu id, up to 0x7FFF).
    CmdShellStaticBase = 6000,
    CmdShellComBase = 8000,
    // 新建 submenu rows backed by HKCR\<ext>\ShellNew (+ index into
    // app::ShellNewItems()). Deliberately outside every other range: 200-999 is
    // the recent list, 1000-5999 index hits, 6000+ Explorer verbs.
    CmdNewShellBase = 100000,
    kMaxNewShellItems = 64,
    // 安装到系统 / 卸载（由程序自身完成，不经过安装器）.
    CmdDeployToSystem = 100103,
    CmdRemoveFromSystem = 100104,
    // Palette: bring the window back when its monitor is gone (B26, §9.3).
    CmdResetWindowPosition = 100105,
    // Palette: drop the thumbnail cache and decode everything again (B31).
    CmdRefreshThumbnails = 100106,
    // 转换 submenu (0.2.0): convert format, resize, compress.
    CmdConvertFormat = 100107,
    CmdConvertResize = 100108,
    CmdConvertCompress = 100109,
    // Built-in "send to compressed folder" row (Explorer's zipped folder).
    CmdCompressZip = 100110,
};

// These sit above the session-local COM id window, so ExecuteShellCommand's
// range check must not claim them (it checks both bounds now). Failing this
// means a 转换 row would silently do nothing when clicked.
static_assert(CmdConvertFormat > CmdShellComBase + 0x7FFF);
static_assert(CmdConvertCompress > CmdShellComBase + 0x7FFF);

// One merged Explorer row (registry static verb or luling_shell COM item),
// already mapped to its final command id. Software-owned submenus keep one
// level: command 0 + non-empty children => flyout header row.
struct ShellMenuEntry {
    int command = 0;
    std::wstring text;
    bool enabled = true;
    std::vector<ShellMenuEntry> children;
    std::wstring verb;      // canonical verb when known (registry / GetCommandString)
    bool from_com = false;  // luling_shell IContextMenu row (vs registry static)
    std::wstring clsid;     // handler CLSID when known ("{guid}")
    std::wstring handler;   // handler display name for the settings catalog
    // Icon the verb registered for itself, "path,index" already split apart. Empty
    // when the verb has no Icon value; the row then draws no icon rather than a guess.
    std::wstring icon_path;
    int icon_index = 0;
};

// Context menu for a selected entry: 打开 + icon strip (cut/copy/delete/
// rename) + built-in verbs + undo. Explorer rows are appended afterwards via
// AppendShellSection (they grow the menu downward so open rows never move).
std::vector<ui::FluentMenuItem> BuildItemMenu(bool can_undo, const std::wstring& undo_label,
                                              bool folder = false);

// Which settings switch owns a built-in row (BuiltinMenuItem::Count: none,
// the row always stays).
BuiltinMenuItem BuiltinItemForCommand(int command);
// Drops the built-in rows the user hid on the 右键菜单 page from an item or
// background menu, before the Explorer section is appended. A dropped row's
// separator moves to the row above it, so groups stay apart without doubling.
// With a surface, the movable rows first take the user's order for it: they
// trade places among the slots they occupy and each slot keeps its separator.
void ApplyBuiltinMenuPrefs(std::vector<ui::FluentMenuItem>& items,
                           const ContextMenuPrefs& prefs,
                           BuiltinMenuSurface surface = BuiltinMenuSurface::Count);

struct Tab;
// Uses snapshot metadata only; never probes the filesystem on the UI thread.
std::wstring RecentChangesMenuPath(const Tab& tab, bool background);
void AppendRecentChangesCommand(std::vector<ui::FluentMenuItem>& items,
                                const std::wstring& path);

// Appends the merged Explorer section at the very bottom: separator, then one
// row per entry. Software submenus become one-level flyout headers (children
// on FluentMenuItem); other rows stay flat. Drops entries whose text
// duplicates an existing row (case-insensitive). Hard safety cap is 48; the
// user-facing default (32) is applied earlier by ApplyExplorerPrefs.
void AppendShellSection(std::vector<ui::FluentMenuItem>& items,
                        const std::vector<ShellMenuEntry>& entries);

// Factory denylist + per-item overrides + compress-into-flyout + order + cap.
// Host still returns the full list; call this in the UI process.
std::vector<ShellMenuEntry> ApplyExplorerPrefs(const ContextMenuPrefs& prefs,
                                               const std::vector<ShellMenuEntry>& entries);

// Context menu for empty list space (creation / paste / terminal / undo).
std::vector<ui::FluentMenuItem> BuildBackgroundMenu(bool can_paste, bool can_undo,
                                                    const std::wstring& undo_label);
struct BackgroundViewOptions {
    ui::ViewMode view_mode = ui::ViewMode::Details;
    ui::SortColumn sort_column = ui::SortColumn::Name;
    ui::SortDirection sort_direction = ui::SortDirection::Asc;
    int folder_sort = 0; // 0 folders first, 1 follow direction, 2 mixed
    bool details_panel = false;
    bool can_sort = true;
    bool indexed_search = false;
    bool show_path = false;
    bool filesystem = true;
    int group_by = 0;        // app::GroupBy value
    bool can_group = false;  // real folders, Recent, search results, tag views
    bool group_virtual = false; // multi-folder view (search, tag, recycle): Location instead of Tag
    bool can_apply_group_all = false; // real folder: offer "apply grouping to all folders"
};
void AppendBackgroundViewCommands(std::vector<ui::FluentMenuItem>& items,
                                  const BackgroundViewOptions& options);
std::vector<ui::FluentMenuItem> BuildSortMenu(const BackgroundViewOptions& options);
// "Group by" submenu item (children: name, date, type, size, none).
ui::FluentMenuItem BuildGroupMenu(const BackgroundViewOptions& options);
ui::FluentMenuItem BuildShortcutHints();
std::vector<ui::FluentMenuItem> BuildRecycleItemMenu(bool can_undo,
                                                     const std::wstring& undo_label);
std::vector<ui::FluentMenuItem> BuildRecycleBackgroundMenu(bool can_undo,
                                                           const std::wstring& undo_label,
                                                           bool can_empty);
std::vector<ui::FluentMenuItem> BuildRecyclePlaceMenu(bool can_empty);

// Toolbar "新建▾" dropdown (reuses the same FluentMenu component).
std::vector<ui::FluentMenuItem> BuildNewMenu();
std::vector<ui::FluentMenuItem> BuildConvertMenu();

std::vector<ui::FluentMenuItem> BuildBreadcrumbMenu(bool filesystem);

// Split-button layout presets.
std::vector<ui::FluentMenuItem> BuildSplitMenu(int current_preset);
// can_apply_all: real folder, offer "apply view and sort to all folders".
std::vector<ui::FluentMenuItem> BuildViewMenu(ui::ViewMode current_mode,
                                              bool details_panel = false,
                                              bool can_apply_all = false);

// Ctrl+K / address omnibar: verbs + index hits + recent folders at the end.
struct OmnibarQuery {
    enum class Kind { Mixed, Command, Search, Project };
    Kind kind = Kind::Mixed;
    std::wstring needle;
    wchar_t prefix = 0;
};

OmnibarQuery ParseOmnibarQuery(const std::wstring& query, bool project_only = false);
bool LooksLikeFilesystemPath(const std::wstring& text);

std::vector<ui::FluentMenuItem> BuildCommandPalette(const std::vector<std::wstring>& recent_paths);
std::vector<ui::FluentMenuItem> BuildCommandPalette(const std::wstring& query,
                                                    const std::vector<std::wstring>& recent_paths,
                                                    const std::vector<index::Hit>& hits,
                                                    bool project_only,
                                                    size_t total = 0,
                                                    const std::wstring& current_path = {});

// "base.ext", "base (2).ext", ... — first name not colliding under dir.
std::wstring UniqueChildName(const std::wstring& dir, const std::wstring& base,
                             const std::wstring& ext);

} // namespace luling::app
