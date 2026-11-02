// ai_write.cpp - organize_files: plan checking, the moves, and their undo.
#include "ai_write.h"

#include "ai_tools.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace luling::app::ai {
namespace {

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/' || c == L':'; }

// A single path segment: no separators, not empty, not "." or "..".
bool SafeSegment(const std::wstring& name) {
    if (name.empty() || name.size() > 255) return false;
    if (name == L"." || name == L"..") return false;
    for (wchar_t c : name) {
        if (IsSeparator(c)) return false;
        if (c < 32) return false;
    }
    return true;
}

bool SameNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (!dir.empty() && dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

bool ParseOrganizePlan(const json::Value& args, OrganizePlan& plan, std::string& code,
                       std::string& message) {
    const std::wstring raw = json::Wide(args.get_string("folder"));
    if (!ResolveFolderPath(raw, plan.folder) || !IsDirectory(plan.folder)) {
        code = "bad_folder";
        message = "The folder does not exist or is not a folder.";
        return false;
    }
    {
        std::wstring trimmed = plan.folder;
        while (!trimmed.empty() && trimmed.back() == L'\\') trimmed.pop_back();
        if (const size_t cut = trimmed.find_last_of(L'\\'); cut != std::wstring::npos)
            plan.label = trimmed.substr(cut + 1);
        else
            plan.label = trimmed;
    }
    const json::Value* groups = args.find("groups");
    if (!groups || !groups->is_array() || groups->size() == 0) {
        code = "bad_groups";
        message = "groups must list at least one subfolder with the items that go in it.";
        return false;
    }
    if (groups->size() > 24) {
        code = "too_many_groups";
        message = "At most 24 subfolders in one plan.";
        return false;
    }
    std::vector<std::wstring> seen;   // every item, once
    for (const json::Value& g : groups->items()) {
        OrganizeGroup group;
        group.name = json::Wide(g.get_string("name"));
        if (!SafeSegment(group.name)) {
            code = "bad_group_name";
            message = "A subfolder name must be one short name, without \\ / or :.";
            return false;
        }
        const json::Value* items = g.find("items");
        if (!items || !items->is_array() || items->size() == 0) {
            code = "empty_group";
            message = "Every subfolder needs at least one item.";
            return false;
        }
        for (const json::Value& it : items->items()) {
            const std::wstring name = json::Wide(it.as_string());
            if (!SafeSegment(name)) {
                code = "bad_item";
                message = "Items are names directly inside the folder, not paths.";
                return false;
            }
            const bool dup = std::any_of(seen.begin(), seen.end(),
                                         [&](const std::wstring& s) { return SameNoCase(s, name); });
            if (dup) continue;
            if (!Exists(Join(plan.folder, name))) {
                code = "missing_item";
                message = "An item in the plan is not in that folder any more.";
                return false;
            }
            seen.push_back(name);
            group.items.push_back(name);
        }
        if (group.items.empty()) {
            code = "empty_group";
            message = "Every subfolder needs at least one item.";
            return false;
        }
        group.exists = IsDirectory(Join(plan.folder, group.name));
        if (!group.exists) ++plan.created;
        plan.total += group.items.size();
        plan.groups.push_back(std::move(group));
    }
    if (plan.groups.empty() || plan.total == 0) {
        code = "empty_plan";
        message = "The plan has nothing to move.";
        return false;
    }
    if (plan.total > 500) {
        code = "too_many_items";
        message = "At most 500 items in one plan; split the request.";
        return false;
    }
    return true;
}

} // namespace luling::app::ai
