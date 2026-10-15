#include "system_deploy.h"
#include "shell_integration_registry.h"
#include "shell_tag_registry.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>

namespace luling {
namespace {

constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Luling";
constexpr wchar_t kAppName[] = L"Luling";
constexpr wchar_t kDisplayName[] = L"鹿铃";

std::wstring ModulePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size()) {
            buffer.resize(written);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring FolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

// Recursive delete through the shell, which handles the nested and read-only
// cases a hand-rolled walk misses.
void RemoveTree(const std::wstring& dir) {
    if (dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::wstring from = dir;
    from.push_back(L'\0');  // pFrom is double-terminated
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) || !raw) return {};
    std::wstring path = raw;
    CoTaskMemFree(raw);
    return path;
}

std::wstring VersionText() {
    const std::wstring exe = ModulePath();
    const std::wstring dir = FolderOf(exe);
    // version.txt sits next to the executable in a portable/exe layout.
    std::wstring buffer(64, L'\0');
    const std::wstring file = dir.empty() ? std::wstring{} : dir + L"\\version.txt";
    HANDLE h = file.empty() ? INVALID_HANDLE_VALUE
                            : CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD read = 0;
    ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(wchar_t)), &read, nullptr);
    CloseHandle(h);
    buffer.resize(read / sizeof(wchar_t));
    while (!buffer.empty() && (buffer.back() == L'\r' || buffer.back() == L'\n' ||
                               buffer.back() == L' '))
        buffer.pop_back();
    return buffer;
}

bool WriteShortcut(const std::wstring& lnk, const std::wstring& target,
                   const std::wstring& workdir, std::wstring& error) {
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&link));
    if (FAILED(hr) || !link) {
        error = L"无法创建快捷方式对象";
        return false;
    }
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workdir.c_str());
    link->SetIconLocation(target.c_str(), 0);
    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr) && file) {
        hr = file->Save(lnk.c_str(), TRUE);
        file->Release();
    }
    link->Release();
    if (FAILED(hr)) {
        error = L"无法写入快捷方式：" + lnk;
        return false;
    }
    return true;
}

bool WriteRegistry(const std::wstring& exe, const std::wstring& dir, std::wstring& error) {
    HKEY key = nullptr;
    const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0,
                                            KEY_SET_VALUE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        error = L"无法写入卸载信息（错误 " + std::to_wstring(created) + L"）";
        return false;
    }
    auto text = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    const std::wstring version = VersionText();
    text(L"DisplayName", kDisplayName);
    if (!version.empty()) text(L"DisplayVersion", version);
    text(L"Publisher", L"云舒眠眠");
    text(L"InstallLocation", dir);
    text(L"DisplayIcon", exe);
    text(L"UninstallString", L"\"" + exe + L"\" --uninstall");
    text(L"QuietUninstallString", L"\"" + exe + L"\" --uninstall");
    DWORD one = 1;
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(key);
    return true;
}

} // namespace

bool DeployToSystem(std::wstring& error) {
    const std::wstring exe = ModulePath();
    const std::wstring dir = FolderOf(exe);
    if (exe.empty() || dir.empty()) {
        error = L"无法确定程序位置";
        return false;
    }
    if (!WriteRegistry(exe, dir, error)) return false;
    const std::wstring desktop = KnownFolderPath(FOLDERID_Desktop);
    if (!desktop.empty() && !WriteShortcut(desktop + L"\\" + kAppName + L".lnk", exe, dir, error))
        return false;
    const std::wstring programs = KnownFolderPath(FOLDERID_Programs);
    if (!programs.empty()) {
        const std::wstring group = programs + L"\\" + kDisplayName;
        CreateDirectoryW(group.c_str(), nullptr);
        if (!WriteShortcut(group + L"\\" + kDisplayName + L".lnk", exe, dir, error)) return false;
    }
    return true;
}

bool RemoveFromSystem(std::wstring& error) {
    const std::wstring exe = ModulePath();
    const std::wstring dir = FolderOf(exe);
    // Hand the file manager back first. The takeover keys point at this exe and
    // the snapshot that makes the step reversible is dropped a few lines below,
    // so an uninstall that skips this leaves Explorer unable to open a folder
    // at all (#228, #230).
    for (const auto kind : {app::ShellIntegrationKind::Folders, app::ShellIntegrationKind::WinE,
                            app::ShellIntegrationKind::ThisPc}) {
        app::ApplyShellIntegration(kind, exe, false);
    }
    // The tag submenu lives in the registry and points at icons kept inside the
    // data directory this function is about to erase.
    if (const std::wstring local = KnownFolderPath(FOLDERID_LocalAppData); !local.empty())
        app::RemoveTagMenu(local + L"\\" + kAppName + L"\\tagicons");
    const std::wstring desktop = KnownFolderPath(FOLDERID_Desktop);
    if (!desktop.empty()) DeleteFileW((desktop + L"\\" + kAppName + L".lnk").c_str());
    const std::wstring programs = KnownFolderPath(FOLDERID_Programs);
    if (!programs.empty()) {
        const std::wstring group = programs + L"\\" + kDisplayName;
        DeleteFileW((group + L"\\" + kDisplayName + L".lnk").c_str());
        RemoveDirectoryW(group.c_str());
    }
    const LSTATUS removed =
        RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) {
        error = L"无法删除卸载信息（错误 " + std::to_wstring(removed) + L"）";
        return false;
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Luling");

    // Everything the app keeps outside the install folder. Leaving these behind
    // is what made an "uninstalled" Luling still show up in %LOCALAPPDATA%.
    // The data directory also holds the index service's files, so it may need a
    // moment after the service stops.
    for (int attempt = 0; attempt < 3; ++attempt) {
        for (const auto& id : {FOLDERID_LocalAppData, FOLDERID_RoamingAppData}) {
            const std::wstring base = KnownFolderPath(id);
            if (base.empty()) continue;
            RemoveTree(base + L"\\" + kAppName);
            RemoveTree(base + L"\\" + kDisplayName);
        }
        const std::wstring common = KnownFolderPath(FOLDERID_ProgramData);
        if (!common.empty()) {
            RemoveTree(common + L"\\" + kAppName);
            RemoveTree(common + L"\\" + kDisplayName);
        }
        if (GetFileAttributesW((KnownFolderPath(FOLDERID_LocalAppData) + L"\\" + kAppName).c_str()) ==
            INVALID_FILE_ATTRIBUTES)
            break;
        Sleep(300);
    }

    // The running executable cannot delete its own folder: hand that to a
    // detached command that waits for this process to exit.
    if (!dir.empty()) {
        const std::wstring command = L"/c timeout /t 2 /nobreak >nul & rmdir /s /q \"" + dir + L"\"";
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        ShellExecuteW(nullptr, L"open", L"cmd.exe", mutable_command.data(), nullptr, SW_HIDE);
    }
    return true;
}

bool IsDeployedToSystem() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return true;
    }
    const std::wstring desktop = KnownFolderPath(FOLDERID_Desktop);
    if (!desktop.empty()) {
        const std::wstring lnk = desktop + L"\\" + kAppName + L".lnk";
        if (GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return false;
}

} // namespace luling
