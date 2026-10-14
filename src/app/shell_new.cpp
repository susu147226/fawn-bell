#include "shell_new.h"
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>
#include <unordered_set>

namespace luling::app {
namespace {

std::wstring Lower(std::wstring value) {
    for (auto& c : value) c = static_cast<wchar_t>(towlower(c));
    return value;
}

std::wstring ReadString(HKEY root, const std::wstring& key, const wchar_t* name) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return {};
    DWORD type = 0;
    DWORD size = 0;
    std::wstring out;
    if (RegQueryValueExW(h, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(wchar_t)) {
        out.resize(size / sizeof(wchar_t));
        if (RegQueryValueExW(h, name, nullptr, &type,
                             reinterpret_cast<LPBYTE>(out.data()), &size) != ERROR_SUCCESS)
            out.clear();
        while (!out.empty() && out.back() == L'\0') out.pop_back();
    }
    RegCloseKey(h);
    return out;
}

bool ValueExists(HKEY root, const std::wstring& key, const wchar_t* name) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    const bool exists =
        RegQueryValueExW(h, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(h);
    return exists;
}

bool KeyExists(HKEY root, const std::wstring& key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

bool ReadBinary(HKEY root, const std::wstring& key, std::vector<uint8_t>& out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = 0;
    bool ok = false;
    if (RegQueryValueExW(h, L"Data", nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size > 0 && size <= 1024u * 1024u) {
        out.resize(size);
        ok = RegQueryValueExW(h, L"Data", nullptr, &type, out.data(), &size) == ERROR_SUCCESS;
        if (ok) out.resize(size);
    }
    RegCloseKey(h);
    return ok;
}

// "@%SystemRoot%\system32\shell32.dll,-8970" resolves to the localized name.
std::wstring ResolveIndirect(const std::wstring& raw) {
    if (raw.empty()) return {};
    if (raw[0] != L'@') return raw;
    wchar_t buffer[512]{};
    if (SUCCEEDED(SHLoadIndirectString(raw.c_str(), buffer, ARRAYSIZE(buffer), nullptr)))
        return buffer;
    return {};
}

std::wstring Expand(const std::wstring& value) {
    if (value.empty()) return {};
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(value.c_str(), buffer, ARRAYSIZE(buffer));
    if (n == 0 || n > ARRAYSIZE(buffer)) return value;
    return buffer;
}

// The label Explorer shows: the ShellNew MUIVerb, else the associated type
// name, else the bare extension so the row is never blank.
std::wstring DisplayName(HKEY classes, const std::wstring& ext) {
    std::wstring display =
        ResolveIndirect(ReadString(classes, ext + L"\\ShellNew", L"MUIVerb"));
    if (display.empty())
        display = ResolveIndirect(ReadString(classes, ext + L"\\ShellNew", L"LocalizedName"));
    if (display.empty()) {
        const std::wstring progid = ReadString(classes, ext, nullptr);
        if (!progid.empty()) {
            display = ResolveIndirect(ReadString(classes, progid, nullptr));
            if (display.empty())
                display = ResolveIndirect(ReadString(classes, progid, L"FriendlyTypeName"));
        }
    }
    if (display.empty() || display[0] == L'{') display = ext;
    return display;
}

// HKEY_CLASSES_ROOT is a merged view whose enumeration is not reliable, so the
// two real stores are walked instead: per-user first (WPS and friends register
// their new-file types under HKCU), machine second.
void CollectFrom(HKEY root, const wchar_t* classes_key, std::vector<ShellNewItem>& out,
                 std::unordered_set<std::wstring>& seen) {
    HKEY classes = nullptr;
    if (RegOpenKeyExW(root, classes_key, 0, KEY_READ, &classes) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD length = ARRAYSIZE(name);
        const LSTATUS status =
            RegEnumKeyExW(classes, i, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        const std::wstring ext = name;
        if (ext.size() < 2 || ext[0] != L'.') continue;
        const std::wstring lower = Lower(ext);
        if (seen.count(lower)) continue;
        const std::wstring shell_new = ext + L"\\ShellNew";
        if (!KeyExists(classes, shell_new)) continue;
        // Command entries run a registered program; they are never listed here.
        if (ValueExists(classes, shell_new, L"Command")) {
            seen.insert(lower);
            continue;
        }
        ShellNewItem item;
        item.extension = ext;
        if (ValueExists(classes, shell_new, L"FileName")) {
            item.template_path = Expand(ReadString(classes, shell_new, L"FileName"));
            if (item.template_path.empty()) continue;
        } else if (ValueExists(classes, shell_new, L"Data")) {
            if (!ReadBinary(classes, shell_new, item.data)) continue;
        } else if (!ValueExists(classes, shell_new, L"NullFile")) {
            continue; // a ShellNew key with none of the shapes creates nothing
        }
        item.display = DisplayName(classes, ext);
        out.push_back(std::move(item));
        seen.insert(lower);
    }
    RegCloseKey(classes);
}

std::vector<ShellNewItem> BuildItems() {
    std::vector<ShellNewItem> out;
    std::unordered_set<std::wstring> seen;
    CollectFrom(HKEY_CURRENT_USER, L"Software\\Classes", out, seen);
    CollectFrom(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", out, seen);
    std::sort(out.begin(), out.end(), [](const ShellNewItem& a, const ShellNewItem& b) {
        return _wcsicmp(a.display.c_str(), b.display.c_str()) < 0;
    });
    return out;
}

std::vector<ShellNewItem>& Cache() {
    static std::vector<ShellNewItem> items = BuildItems();
    return items;
}

} // namespace

const std::vector<ShellNewItem>& ShellNewItems() { return Cache(); }

void InvalidateShellNewCache() { Cache() = BuildItems(); }

} // namespace luling::app
