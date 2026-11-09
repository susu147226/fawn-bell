#include "app_internal.h"
#include "context_menu.h"
#include "../fs/fs_enum.h"
#include "../ops/clipboard.h"
#include "../common/localization.h"
#include <algorithm>
#include <cwctype>

namespace luling {

namespace {

constexpr int kHideCloud = 32400;

// Runs a BuildBreadcrumbMenu command on `path` (a breadcrumb or a sidebar row).
void RunPathCommand(AppState& s, const std::wstring& path, bool filesystem, int command) {
    const std::wstring shell_path = ClipboardPath(path);
    switch (command) {
    case app::CmdOpenInNewTab:
        OpenFolderTab(s, path);
        break;
    case app::CmdOpen:
        NavigateTo(s, path);
        break;
    case app::CmdCopyPath:
        ops::WriteClipboardText(shell_path);
        break;
    case app::CmdCopy:
        if (filesystem) ops::WriteClipboard({shell_path}, false);
        break;
    case app::CmdOpenTerminal:
        if (filesystem) s.ops.OpenTerminal(shell_path);
        break;
    case app::CmdProperties:
        if (filesystem) s.ops.ShowProperties(shell_path);
        break;
    default:
        break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace

void ShowBreadcrumbMenu(AppState& s, std::wstring path, POINT screen_pt) {
    if (path.empty() || !EnsureMenu(s)) return;
    s.context_menu.Close();
    const bool filesystem = !fs::IsVirtualPath(path);
    // Keep the clicked path across TrackPopup's nested message loop. Selection
    // commands target file rows and must not be used for breadcrumb ancestors.
    const int command = s.menu->TrackPopup(screen_pt, app::BuildBreadcrumbMenu(filesystem));
    RunPathCommand(s, path, filesystem, command);
}

// The separator between two breadcrumb segments lists the folder that ends at
// it, so a sibling folder is one click away instead of "up, then down".
void ShowBreadcrumbChildrenMenu(AppState& s, const std::wstring& path, POINT screen_pt) {
    if (path.empty() || !EnsureMenu(s)) return;
    if (fs::IsVirtualPath(path)) {
        NavigateTo(s, path);
        return;
    }
    std::vector<fs::DirEntry> entries;
    try {
        fs::EnumerateDirectory(path, entries);
    } catch (...) {
        return;
    }
    std::vector<const fs::DirEntry*> folders;
    for (const auto& entry : entries) if (entry.is_dir) folders.push_back(&entry);
    if (folders.empty()) return;
    std::sort(folders.begin(), folders.end(), [](const fs::DirEntry* a, const fs::DirEntry* b) {
        return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0;
    });
    constexpr int kFirstChild = 500;
    std::vector<ui::FluentMenuItem> items;
    items.reserve(folders.size());
    for (size_t i = 0; i < folders.size(); ++i) {
        ui::FluentMenuItem item;
        item.command = kFirstChild + static_cast<int>(i);
        item.text = folders[i]->name;
        item.glyph = L"\xE8B7";
        items.push_back(std::move(item));
    }
    s.menu->SetMaxVisibleRows(16);
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command < kFirstChild || static_cast<size_t>(command - kFirstChild) >= folders.size()) return;
    std::wstring target = path;
    if (target.back() != L'\\') target += L'\\';
    target += folders[static_cast<size_t>(command - kFirstChild)]->name;
    NavigateTo(s, target);
}

// #80: OneDrive rows form a header-less section, so the row carries what a
// header menu would: the usual folder commands, then hiding OneDrive. The toast
// says where to bring it back.
void ShowCloudPlaceMenu(AppState& s, const std::wstring& path, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    s.context_menu.Close();
    const bool filesystem = !path.empty() && !fs::IsVirtualPath(path);
    std::vector<ui::FluentMenuItem> items;
    if (!path.empty()) items = app::BuildBreadcrumbMenu(filesystem);
    if (!items.empty()) items.back().separator_after = true;
    ui::FluentMenuItem hide;
    hide.command = kHideCloud;
    hide.text = l10n::Get(l10n::StringId::SidebarHideCloud);
    items.push_back(std::move(hide));
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == kHideCloud) {
        s.sidebarHiddenMask |= 1u << static_cast<int>(app::SidebarSectionId::Cloud);
        s.notification_toast.Show(s.hwnd, L"OneDrive",
            l10n::Get(l10n::StringId::SidebarCloudHiddenHint), false);
        InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }
    if (!path.empty()) RunPathCommand(s, path, filesystem, command);
}

} // namespace luling
