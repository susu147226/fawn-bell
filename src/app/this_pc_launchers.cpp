#include "this_pc_launchers.h"

#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>

namespace luling::app {
namespace {

constexpr wchar_t kExplorer[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";
constexpr wchar_t kNonEnum[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\NonEnum";

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    return a + L"\\" + b;
}

bool ReadString(HKEY root, const std::wstring& subkey, const wchar_t* value, std::wstring& out) {
    std::wstring buffer(512, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
        const LSTATUS status = RegGetValueW(root, subkey.c_str(), value,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buffer.data(), &bytes);
        if (status == ERROR_MORE_DATA) {
            buffer.assign(bytes / sizeof(wchar_t) + 64, L'\0');
            continue;
        }
        if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return false;
        buffer.resize(bytes / sizeof(wchar_t));
        while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
        out = std::move(buffer);
        return !out.empty();
    }
    return false;
}

bool ReadDword(HKEY root, const std::wstring& subkey, const wchar_t* value, DWORD& out) {
    DWORD bytes = sizeof(out);
    return RegGetValueW(root, subkey.c_str(), value, RRF_RT_REG_DWORD, nullptr, &out, &bytes)
        == ERROR_SUCCESS;
}

bool KeyExists(HKEY root, const std::wstring& subkey) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

std::vector<std::wstring> SubkeyNames(HKEY root, const std::wstring& subkey) {
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return names;
    for (DWORD index = 0; index < 4096; ++index) {
        wchar_t name[256];
        DWORD length = ARRAYSIZE(name);
        const LSTATUS status = RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_SUCCESS) names.emplace_back(name, length);
    }
    RegCloseKey(key);
    return names;
}

// "@%SystemRoot%\\foo.dll,-101" style names resolve through the resource.
std::wstring ResolveDisplayString(const std::wstring& raw) {
    if (raw.empty() || raw.front() != L'@') return raw;
    wchar_t resolved[512]{};
    if (SUCCEEDED(SHLoadIndirectString(raw.c_str(), resolved, ARRAYSIZE(resolved), nullptr)) &&
        resolved[0])
        return resolved;
    return {};
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool ContainsNoCase(const std::wstring& haystack, const wchar_t* needle) {
    std::wstring h = haystack, n = needle;
    for (auto& c : h) c = static_cast<wchar_t>(std::towlower(c));
    for (auto& c : n) c = static_cast<wchar_t>(std::towlower(c));
    return h.find(n) != std::wstring::npos;
}

std::wstring LastComponent(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring NormalizeTarget(const std::wstring& raw) {
    if (raw.empty()) return {};
    wchar_t expanded[32768]{};
    const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), expanded, ARRAYSIZE(expanded));
    std::wstring out = (n > 0 && n <= ARRAYSIZE(expanded)) ? expanded : raw;
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

// Where a delegate-folder CLSID points: TargetFolderPath or the FolderShortcut
// "Target" spelling.
std::wstring TargetOf(HKEY root, const std::wstring& clsid_key) {
    const std::wstring bag = Join(clsid_key, L"Instance\\InitPropertyBag");
    std::wstring target;
    if (ReadString(root, bag, L"TargetFolderPath", target)) return target;
    if (ReadString(root, bag, L"Target", target)) return target;
    return {};
}

bool HiddenByPolicy(const std::wstring& clsid) {
    DWORD value = 0;
    return ReadDword(HKEY_CURRENT_USER, kNonEnum, clsid.c_str(), value) && (value & 1u);
}

// InprocServer32 of Windows' own shell folders: %SystemRoot%\system32\*.dll, or
// a bare DLL name that loads from System32.
bool SystemServer(std::wstring server) {
    if (!server.empty() && server.front() == L'"') {
        server.erase(0, 1);
        const size_t quote = server.find(L'"');
        if (quote != std::wstring::npos) server.resize(quote);
    }
    if (server.find_first_of(L"\\/") == std::wstring::npos) return true;
    wchar_t windir[MAX_PATH]{};
    const UINT n = GetWindowsDirectoryW(windir, ARRAYSIZE(windir));
    if (n == 0 || n >= ARRAYSIZE(windir)) return false;
    const std::wstring dir = std::wstring(windir) + L"\\";
    return server.size() > dir.size() && _wcsnicmp(server.c_str(), dir.c_str(), dir.size()) == 0;
}

std::uint8_t LauncherBrand(const std::wstring& clsid, const std::wstring& label) {
    if (EqualsNoCase(clsid, L"{679F137C-3162-45da-BE3C-2F9C3D093F64}") ||
        ContainsNoCase(label, L"Baidu") || ContainsNoCase(label, L"百度"))
        return 2;
    if (EqualsNoCase(clsid, L"{5FCD4425-CA3A-48F4-A57C-B8A75C32ACB1}") || ContainsNoCase(label, L"WPS"))
        return 3;
    return 1;
}

} // namespace

std::vector<ThisPcLauncher> CurrentThisPcLaunchers() {
    std::vector<ThisPcLauncher> out;
    std::vector<std::wstring> seen;
    const std::wstring pc = Join(kExplorer, L"MyComputer\\NameSpace");
    struct NamespaceKey { HKEY root; const wchar_t* path; };
    const NamespaceKey namespaces[] = {
        { HKEY_CURRENT_USER, pc.c_str() },
        { HKEY_LOCAL_MACHINE, pc.c_str() },
    };
    for (const auto& ns : namespaces) {
        for (const auto& clsid : SubkeyNames(ns.root, ns.path)) {
            if (clsid.size() != 38 || clsid.front() != L'{' || clsid.back() != L'}') continue;
            if (std::any_of(seen.begin(), seen.end(),
                    [&](const std::wstring& s) { return EqualsNoCase(s, clsid); }))
                continue;
            seen.push_back(clsid);
            if (HiddenByPolicy(clsid)) continue;
            const HKEY classes[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
            for (const HKEY classes_root : classes) {
                const std::wstring classes_path =
                    L"Software\\Classes\\CLSID\\" + clsid;
                if (!KeyExists(classes_root, classes_path)) continue;
                std::wstring target = NormalizeTarget(TargetOf(classes_root, classes_path));
                if (!target.empty()) {
                    const DWORD attrs = GetFileAttributesW(target.c_str());
                    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                        target.clear();
                }
                std::wstring server;
                const bool has_server = ReadString(classes_root,
                    Join(classes_path, L"InprocServer32"), nullptr, server);
                if (target.empty() && (!has_server || SystemServer(server))) break; // Windows' own
                std::wstring label;
                std::wstring raw;
                if (ReadString(classes_root, classes_path, L"LocalizedString", raw))
                    label = ResolveDisplayString(raw);
                if (label.empty() && ReadString(classes_root, classes_path, nullptr, raw))
                    label = ResolveDisplayString(raw);
                if (label.empty() && ReadString(ns.root, Join(ns.path, clsid), nullptr, raw))
                    label = ResolveDisplayString(raw);
                if (label.empty() && !target.empty()) label = LastComponent(target);
                if (label.empty()) break; // nothing to call it: Explorer shows no name either
                ThisPcLauncher launcher;
                launcher.clsid = clsid;
                launcher.label = std::move(label);
                launcher.target = std::move(target);
                launcher.brand = LauncherBrand(clsid, launcher.label);
                out.push_back(std::move(launcher));
                break;
            }
        }
    }
    return out;
}

} // namespace luling::app
