#include "shell_new.h"
#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>

namespace luling::app {
namespace {

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
std::wstring DisplayName(const std::wstring& ext) {
    std::wstring display = ResolveIndirect(ReadString(HKEY_CLASSES_ROOT,
        ext + L"\\ShellNew", L"MUIVerb"));
    if (display.empty()) display = ResolveIndirect(ReadString(HKEY_CLASSES_ROOT,
        ext + L"\\ShellNew", L"LocalizedName"));
    if (display.empty()) {
        const std::wstring progid = ReadString(HKEY_CLASSES_ROOT, ext, nullptr);
        if (!progid.empty()) {
            display = ResolveIndirect(ReadString(HKEY_CLASSES_ROOT, progid, nullptr));
            if (display.empty())
                display = ResolveIndirect(ReadString(HKEY_CLASSES_ROOT, progid,
                                                     L"FriendlyTypeName"));
        }
    }
    if (display.empty()) display = ext;
    return display;
}

std::vector<ShellNewItem> BuildItems() {
    std::vector<ShellNewItem> out;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, L"", 0, KEY_READ, &root) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD length = ARRAYSIZE(name);
        const LSTATUS status =
            RegEnumKeyExW(root, i, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        const std::wstring ext = name;
        if (ext.size() < 2 || ext[0] != L'.') continue;
        const std::wstring shell_new = ext + L"\\ShellNew";
        HKEY probe = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, shell_new.c_str(), 0, KEY_READ, &probe) !=
            ERROR_SUCCESS) continue;
        RegCloseKey(probe);
        // Command entries run a registered program; Explorer hides them behind
        // the classic menu only, and running one from here would be a surprise.
        if (ValueExists(HKEY_CLASSES_ROOT, shell_new, L"Command")) continue;

        ShellNewItem item;
        item.extension = ext;
        if (ValueExists(HKEY_CLASSES_ROOT, shell_new, L"FileName")) {
            item.template_path = Expand(ReadString(HKEY_CLASSES_ROOT, shell_new, L"FileName"));
            if (item.template_path.empty()) continue;
        } else if (ValueExists(HKEY_CLASSES_ROOT, shell_new, L"Data")) {
            if (!ReadBinary(HKEY_CLASSES_ROOT, shell_new, item.data)) continue;
        } else if (!ValueExists(HKEY_CLASSES_ROOT, shell_new, L"NullFile")) {
            // A ShellNew key with none of the shapes above creates nothing.
            continue;
        }
        item.display = DisplayName(ext);
        out.push_back(std::move(item));
    }
    RegCloseKey(root);
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
