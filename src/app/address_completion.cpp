#include "app_internal.h"
#include "address_completion.h"
#include "../common/localization.h"
#include <algorithm>
#include <cwctype>

namespace luling {
namespace {

constexpr int kOpenTyped = 1;      // "go to what was typed"
constexpr int kFirstChild = 100;   // + index into addressCompleteEntries
constexpr size_t kMaxEntries = 2000;
constexpr size_t kMaxRows = 200;
constexpr ULONGLONG kDebounceMs = 120;

struct CompletionTarget {
    std::wstring dir;     // folder to list; empty means "no completion here"
    std::wstring prefix;  // text after the last separator
};

std::wstring AddressEditText(const AppState& s) {
    if (!s.hwndAddressEdit || !IsWindow(s.hwndAddressEdit)) return {};
    const int length = GetWindowTextLengthW(s.hwndAddressEdit);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    text.resize(GetWindowTextW(s.hwndAddressEdit, text.data(), length + 1));
    return text;
}

// A UNC path is left alone: enumerating a share that is asleep would block the
// keystroke that asked for it. Everything local splits into "folder + prefix".
CompletionTarget SplitCompletionTarget(const std::wstring& text, const std::wstring& current) {
    CompletionTarget target;
    std::wstring input = text;
    for (auto& c : input) if (c == L'/') c = L'\\';
    while (!input.empty() && input.front() == L'"') input.erase(input.begin());
    while (!input.empty() && input.back() == L'"') input.pop_back();
    if (input.empty()) return target;
    if (input.size() >= 2 && input[0] == L'\\' && input[1] == L'\\') return target;
    const size_t slash = input.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        if (input.size() == 2 && input[1] == L':') {
            target.dir = input + L"\\";
            return target;
        }
        if (!current.empty() && !fs::IsVirtualPath(current)) {
            target.dir = current;
            target.prefix = input;
        }
        return target;
    }
    std::wstring dir = input.substr(0, slash);
    target.prefix = input.substr(slash + 1);
    if (dir.empty()) return CompletionTarget{};
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    target.dir = std::move(dir);
    return target;
}

bool IsLocalFolder(const std::wstring& dir) {
    if (dir.size() >= 2 && dir[0] == L'\\' && dir[1] == L'\\') return false;
    if (dir.size() < 2 || dir[1] != L':') return false;
    const wchar_t root[4] = { dir[0], L':', L'\\', 0 };
    const UINT type = GetDriveTypeW(root);
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK;
}

// The filter callback runs inside the edit's WM_CHAR, so it must never touch the
// disk on an ordinary keystroke. Only a change of folder (the user typed a
// separator) lists again, and only on a local drive.
void EnsureCompletionDir(AppState& s, const std::wstring& dir) {
    if (dir == s.addressCompleteDir) return;
    s.addressCompleteDir = dir;
    s.addressCompleteEntries.clear();
    if (dir.empty() || !IsLocalFolder(dir)) return;
    std::vector<fs::DirEntry> entries;
    try {
        fs::EnumerateDirectory(dir, entries);
    } catch (...) {
        return;
    }
    if (entries.size() > kMaxEntries) entries.resize(kMaxEntries);
    s.addressCompleteEntries = std::move(entries);
}

bool StartsWithNoCase(const std::wstring& text, const std::wstring& prefix) {
    if (prefix.empty()) return true;
    if (text.size() < prefix.size()) return false;
    return _wcsnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name, bool trailing_slash) {
    std::wstring full = dir;
    if (!full.empty() && full.back() != L'\\') full += L'\\';
    full += name;
    if (trailing_slash) full += L'\\';
    return full;
}

} // namespace

std::vector<ui::FluentMenuItem> AddressCompletionRows(AppState& s, const std::wstring& query) {
    std::vector<ui::FluentMenuItem> items;
    if (query.empty()) return items;
    // The first row is the commit row. Rebuilding the list highlights the first
    // row again, so Enter keeps meaning "go to the typed path".
    ui::FluentMenuItem open;
    open.command = kOpenTyped;
    open.text = std::wstring(l10n::Pick(L"转到 ", L"Open ")) + query;
    open.glyph = L"\xE8B7";
    open.shortcut = L"Enter";
    items.push_back(std::move(open));

    const auto* tab = ActiveTab(s);
    const std::wstring current = tab ? tab->current_path : std::wstring{};
    const CompletionTarget target = SplitCompletionTarget(query, current);
    if (target.dir.empty()) return items;
    EnsureCompletionDir(s, target.dir);

    std::vector<const fs::DirEntry*> matches;
    for (const auto& entry : s.addressCompleteEntries) {
        if (!entry.name.empty() && StartsWithNoCase(entry.name, target.prefix))
            matches.push_back(&entry);
    }
    std::sort(matches.begin(), matches.end(), [](const fs::DirEntry* a, const fs::DirEntry* b) {
        if (a->is_dir != b->is_dir) return a->is_dir;
        return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0;
    });
    if (matches.size() > kMaxRows) matches.resize(kMaxRows);
    for (size_t i = 0; i < matches.size(); ++i) {
        ui::FluentMenuItem item;
        // The command carries the index into the cached folder, not into the
        // filtered list: the popup rebuilds on every keystroke.
        item.command = kFirstChild +
            static_cast<int>(matches[i] - s.addressCompleteEntries.data());
        item.text = matches[i]->name;
        item.glyph = matches[i]->is_dir ? L"\xE8B7" : L"\xE7C3";
        items.push_back(std::move(item));
    }
    return items;
}

bool RunAddressCompletionRow(AppState& s, int command, bool& navigated) {
    if (command == kOpenTyped) {
        HideAddressEditor(s, true);
        navigated = true;
        return false;
    }
    if (command < kFirstChild) return false;
    const size_t index = static_cast<size_t>(command - kFirstChild);
    if (index >= s.addressCompleteEntries.size() || !s.hwndAddressEdit) return false;
    const auto* tab = ActiveTab(s);
    const std::wstring current = tab ? tab->current_path : std::wstring{};
    const CompletionTarget target = SplitCompletionTarget(AddressEditText(s), current);
    if (target.dir.empty()) return false;
    const fs::DirEntry& entry = s.addressCompleteEntries[index];
    if (entry.is_dir) {
        const std::wstring next = JoinPath(target.dir, entry.name, true);
        SetWindowTextW(s.hwndAddressEdit, next.c_str());
        const auto end = static_cast<WPARAM>(next.size());
        SendMessageW(s.hwndAddressEdit, EM_SETSEL, end, static_cast<LPARAM>(end));
        EnsureCompletionDir(s, JoinPath(target.dir, entry.name, false));
        return true;
    }
    SetWindowTextW(s.hwndAddressEdit, JoinPath(target.dir, entry.name, false).c_str());
    HideAddressEditor(s, true);
    navigated = true;
    return false;
}

namespace {

// Standalone completion popup, used only when nothing else is on screen. While
// the address drop-down is open it rebuilds itself from the edit's text and this
// path never runs, which is what keeps the menu's modal loop from nesting.
void ShowAddressCompletion(AppState& s) {
    if (!s.hwndAddressEdit || !s.addressEditing || s.addressSearching || s.addressCompleting) return;
    if (!EnsureMenu(s) || s.menu->IsOpen()) return;
    const std::wstring text = AddressEditText(s);
    if (text.empty()) return;

    const auto* tab = ActiveTab(s);
    const std::wstring current = tab ? tab->current_path : std::wstring{};
    EnsureCompletionDir(s, SplitCompletionTarget(text, current).dir);

    RECT anchor{};
    GetWindowRect(s.hwndAddressEdit, &anchor);
    const bool previous_ignore = s.addressIgnoreKillFocus;
    s.addressIgnoreKillFocus = true;
    s.addressCompleting = true;
    const HWND edit = s.hwndAddressEdit;

    auto items_for = [&s](const std::wstring& query) { return AddressCompletionRows(s, query); };

    bool repeat = true;
    bool navigated = false;
    while (repeat && s.hwndAddressEdit && IsWindow(s.hwndAddressEdit) && s.addressEditing) {
        repeat = false;
        s.menu->SetTheme(s.darkMode, s.accentColor);
        s.menu->SetAnchorRect(anchor);
        s.menu->SetExternalFilterEdit(edit);
        s.menu->SetFilterMinWidth((anchor.right - anchor.left) / s.scale);
        s.menu->SetMaxVisibleRows(12);
        s.menu->SetHoverFirstOnOpen(true);
        s.menu->SetSelectAllOnOpen(false);
        const int command = s.menu->TrackPopup({anchor.left, anchor.top},
                                               AddressCompletionRows(s, AddressEditText(s)), items_for);
        if (!s.hwndAddressEdit || !IsWindow(s.hwndAddressEdit) || !s.addressEditing) break;
        if (command == 0 && s.menu->LastFilterCommitted()) {
            HideAddressEditor(s, true);
            navigated = true;
            break;
        }
        repeat = RunAddressCompletionRow(s, command, navigated);
    }

    s.addressCompleting = false;
    s.addressIgnoreKillFocus = previous_ignore;
    if (!navigated && s.hwndAddressEdit && IsWindow(s.hwndAddressEdit) && s.addressEditing)
        SetFocus(s.hwndAddressEdit);
}

} // namespace

void RequestAddressCompletion(AppState& s) {
    if (!s.addressEditing || s.addressSearching || !s.hwndAddressEdit) return;
    s.addressCompleteDue = GetTickCount64() + kDebounceMs;
    PostMessageW(s.hwnd, WM_ADDRESS_COMPLETE, 0, 0);
}

void TickAddressCompletion(AppState& s) {
    if (!s.addressEditing || s.addressSearching || !s.hwndAddressEdit) return;
    if (s.addressCompleting) return;
    // An open drop-down rebuilds itself from the edit's text on every keystroke.
    // Opening a second popup from here would nest modal loops.
    if (s.menu && s.menu->IsOpen()) return;
    const ULONGLONG now = GetTickCount64();
    if (s.addressCompleteDue > now) {
        PostMessageW(s.hwnd, WM_ADDRESS_COMPLETE, 0, 0);
        return;
    }
    s.addressCompleteDue = 0;
    ShowAddressCompletion(s);
}

void NavigateToAddressTarget(AppState& s, const std::wstring& target) {
    if (!target.empty() && !fs::IsVirtualPath(target)) {
        const DWORD attrs = GetFileAttributesW(target.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            const size_t slash = target.find_last_of(L"\\/");
            const std::wstring name = slash == std::wstring::npos ? target : target.substr(slash + 1);
            NavigateTo(s, fs::ParentPath(target));
            if (auto* tab = ActiveTab(s)) SelectNameInTab(s, *tab, name);
            return;
        }
    }
    NavigateTo(s, target);
}

} // namespace luling
