// ai_delete.cpp - delete_files: the checks before anything is removed.
#include "ai_delete.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace luling::app::ai {
namespace {

std::wstring BaseName(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

bool SameNoCase(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

bool StartsWithNoCase(const std::wstring& text, const std::wstring& prefix) {
    if (prefix.empty() || text.size() < prefix.size()) return false;
    return SameNoCase(std::wstring_view(text).substr(0, prefix.size()), prefix);
}

// Trimmed, lower-case, no trailing separator: C:\Windows\ -> c:\windows.
std::wstring Normalize(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    for (wchar_t& c : path) c = static_cast<wchar_t>(std::towlower(c));
    return path;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n && n < std::size(buf) ? Normalize(std::wstring(buf, n)) : std::wstring();
}

bool IsDriveRoot(const std::wstring& path) {
    std::wstring p = path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p.size() == 2 && p[1] == L':';
}

// A UNC path, or a drive letter mapped to a share: no Recycle Bin there.
bool OnNetwork(const std::wstring& path) {
    if (path.rfind(L"\\\\", 0) == 0) return true;
    if (path.size() >= 2 && path[1] == L':') {
        wchar_t root[4] = {path[0], L':', L'\\', 0};
        return GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    return false;
}

bool SystemArea(const std::wstring& path) {
    const std::wstring p = Normalize(path);
    for (const wchar_t* name : {L"windir", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData"}) {
        const std::wstring root = Env(name);
        if (!root.empty() && (SameNoCase(p, root) || StartsWithNoCase(p, root + L"\\"))) return true;
    }
    return false;
}

} // namespace

bool ParseDeletePlan(const json::Value& args, const std::vector<std::wstring>& selection,
                     DeletePlan& plan, std::string& code, std::string& message) {
    std::vector<std::wstring> paths;
    if (const json::Value* given = args.find("paths"); given && given->is_array()) {
        for (const json::Value& v : given->items()) {
            const std::wstring path = json::Wide(v.as_string());
            if (!path.empty()) paths.push_back(path);
        }
    }
    if (paths.empty()) paths = selection;
    if (paths.empty()) {
        code = "no_files";
        message = "No items were given and nothing is selected in the window.";
        return false;
    }
    if (paths.size() > 200) {
        code = "too_many_items";
        message = "At most 200 items at once.";
        return false;
    }
    plan.permanent = args.get_bool("permanent", false);
    for (const std::wstring& path : paths) {
        if (IsDriveRoot(path)) {
            code = "drive_root";
            message = "A whole drive cannot be deleted this way.";
            return false;
        }
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            code = "missing_item";
            message = "An item is not there any more.";
            return false;
        }
        if (!plan.permanent && OnNetwork(path)) {
            code = "recycle_unavailable";
            message = "Network locations have no Recycle Bin, so a delete there cannot be undone. Call "
                      "delete_files again with permanent true (the user still confirms, with two clicks).";
            return false;
        }
        if (plan.permanent && SystemArea(path)) {
            code = "system_area";
            message = "Permanent delete never touches Windows folders, Program Files or ProgramData. "
                      "Do it by hand in the window if the user really means it.";
            return false;
        }
        plan.paths.push_back(path);
        plan.names.push_back(BaseName(path));
    }
    plan.folder = ParentOf(plan.paths.front());
    plan.label = BaseName(plan.folder);
    return true;
}

} // namespace luling::app::ai
