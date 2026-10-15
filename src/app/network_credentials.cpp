#include "network_credentials.h"

#include "../common/localization.h"

#include <wincred.h>
#include <winnetwk.h>

#include <vector>

namespace luling::app {
namespace {

// The server a sign-in applies to: the whole host, not one share of it, which is
// what Windows asks for as well.
std::wstring ServerRoot(const std::wstring& unc) {
    std::wstring path = unc;
    while (path.size() > 2 && path.back() == L'\\') path.pop_back();
    const auto share = path.find(L'\\', 2);
    if (share != std::wstring::npos) path.erase(share);
    return path;
}

// The share itself. Probing the server alone would succeed on an anonymous IPC
// session while the share the user asked for stays closed, and the dialog would
// then never be offered.
std::wstring ShareRoot(const std::wstring& unc) {
    std::wstring path = unc;
    while (path.size() > 2 && path.back() == L'\\') path.pop_back();
    const auto share = path.find(L'\\', 2);
    if (share == std::wstring::npos) return path;
    const auto deeper = path.find(L'\\', share + 1);
    if (deeper != std::wstring::npos) path.erase(deeper);
    return path;
}

bool Connect(const std::wstring& target, const wchar_t* user, const wchar_t* password, DWORD& error) {
    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = const_cast<LPWSTR>(target.c_str());
    error = WNetAddConnection2W(&resource, password, user, CONNECT_TEMPORARY);
    // A drive that already points here is not a failure: the share is reachable.
    return error == NO_ERROR || error == ERROR_ALREADY_ASSIGNED;
}

// The failures worth a password prompt. Anything else — a host that is off, a
// name that does not resolve, a share that is gone — would only produce a dialog
// the user cannot answer.
bool LooksLikeSignIn(DWORD error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_LOGON_FAILURE ||
        error == ERROR_INVALID_PASSWORD || error == ERROR_SESSION_CREDENTIAL_CONFLICT ||
        error == ERROR_LOGON_TYPE_NOT_GRANTED || error == ERROR_ACCOUNT_DISABLED ||
        error == ERROR_PASSWORD_EXPIRED || error == ERROR_ACCOUNT_EXPIRED ||
        error == ERROR_PASSWORD_MUST_CHANGE || error == ERROR_NO_SUCH_USER ||
        error == ERROR_NO_LOGON_SERVERS;
}

} // namespace

bool PromptForShareCredentials(HWND owner, const std::wstring& unc) {
    const std::wstring server = ServerRoot(unc);
    const std::wstring share = ShareRoot(unc);
    if (share.size() < 3) return false;
    // Ask without a password first, so an unreachable host never opens a dialog.
    DWORD error = 0;
    if (Connect(share, nullptr, nullptr, error)) return true;
    if (!LooksLikeSignIn(error)) return false;

    const std::wstring caption = l10n::Get(l10n::StringId::ShareCredentialCaption);
    const std::wstring message = l10n::Get(l10n::StringId::ShareCredentialMessage);
    CREDUI_INFOW info{};
    info.cbSize = sizeof(info);
    info.hwndParent = owner;
    info.pszCaptionText = caption.c_str();
    info.pszMessageText = message.c_str();
    std::vector<wchar_t> user(CREDUI_MAX_USERNAME_LENGTH + 1);
    std::vector<wchar_t> password(CREDUI_MAX_PASSWORD_LENGTH + 1);
    BOOL save = FALSE;
    std::wstring target = server;
    const DWORD asked = CredUIPromptForCredentialsW(&info, target.data(), nullptr, 0,
        user.data(), static_cast<DWORD>(user.size()),
        password.data(), static_cast<DWORD>(password.size()), &save,
        CREDUI_FLAGS_DO_NOT_PERSIST | CREDUI_FLAGS_GENERIC_CREDENTIALS |
            CREDUI_FLAGS_EXCLUDE_CERTIFICATES);
    bool connected = false;
    if (asked == NO_ERROR)
        connected = Connect(share, user.data(), password.data(), error);
    // The password only ever lives in this buffer.
    SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    SecureZeroMemory(user.data(), user.size() * sizeof(wchar_t));
    return connected;
}

} // namespace luling::app
