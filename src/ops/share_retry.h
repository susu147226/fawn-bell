#pragma once
// share_retry.h - retries for a handful of share hiccups. SMB can drop one
// request while the session is renewed (credential refresh, brief reconnect);
// the retry asks Windows to re-establish the share first, so the next call does
// not fail the same way.
//
// Self-contained on purpose: the elevated helper links neither the fs library
// nor mpr, so the network check is local and mpr is loaded on demand.
#include <windows.h>
#include <winnetwk.h>

#include <string>

namespace luling::ops::share_retry {

inline bool IsUnc(const std::wstring& path) {
    if (path.size() >= 8 && _wcsnicmp(path.c_str(), L"\\\\?\\UNC\\", 8) == 0) return true;
    if (path.starts_with(L"\\\\?\\")) return false;
    return path.starts_with(L"\\\\");
}

inline bool IsTransient(DWORD error) noexcept {
    switch (error) {
    case ERROR_NETNAME_DELETED:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_UNEXP_NET_ERR:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_NETWORK_BUSY:
    case ERROR_SEM_TIMEOUT:
    case ERROR_NOT_READY:
        return true;
    default:
        return false;
    }
}

inline std::wstring ShareRootOf(const std::wstring& path) {
    std::wstring text = path;
    if (text.size() >= 8 && _wcsnicmp(text.c_str(), L"\\\\?\\UNC\\", 8) == 0)
        text = L"\\\\" + text.substr(8);
    else if (text.starts_with(L"\\\\?\\")) return {};
    if (!text.starts_with(L"\\\\")) return {};
    const size_t server_end = text.find(L'\\', 2);
    if (server_end == std::wstring::npos) return text;
    const size_t share_end = text.find(L'\\', server_end + 1);
    return share_end == std::wstring::npos ? text : text.substr(0, share_end);
}

// Re-opens the session with the current user's credentials. A share that is
// already connected simply reports ERROR_ALREADY_ASSIGNED; nothing is written
// to the credential store.
inline void TouchShare(const std::wstring& path) {
    if (!IsUnc(path)) return;
    const std::wstring share = ShareRootOf(path);
    if (share.size() < 3) return;
    using AddConnection = DWORD(WINAPI*)(LPNETRESOURCEW, LPCWSTR, LPCWSTR, DWORD);
    static AddConnection add = [] {
        const HMODULE mpr = LoadLibraryW(L"mpr.dll");
        return mpr ? reinterpret_cast<AddConnection>(GetProcAddress(mpr, "WNetAddConnection2W"))
                   : nullptr;
    }();
    if (!add) return;
    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = const_cast<LPWSTR>(share.c_str());
    add(&resource, nullptr, nullptr, CONNECT_TEMPORARY);
}

inline HRESULT CopyFile2WithRetry(const wchar_t* source, const wchar_t* destination,
                                  COPYFILE2_EXTENDED_PARAMETERS* parameters) {
    HRESULT hr = CopyFile2(source, destination, parameters);
    for (int attempt = 1; attempt <= 2 && FAILED(hr); ++attempt) {
        const DWORD error = HRESULT_CODE(hr);
        if (!IsTransient(error)) break;
        if (!IsUnc(source) && !IsUnc(destination)) break;
        Sleep(300u * static_cast<DWORD>(attempt));
        TouchShare(source);
        TouchShare(destination);
        hr = CopyFile2(source, destination, parameters);
    }
    return hr;
}

// MoveFileEx with one retry for a transient share error; `error` receives the
// last Win32 code either way.
inline BOOL MoveFileExWithRetry(const std::wstring& from, const std::wstring& to, DWORD flags,
                                DWORD& error) {
    if (MoveFileExW(from.c_str(), to.c_str(), flags)) {
        error = ERROR_SUCCESS;
        return TRUE;
    }
    error = GetLastError();
    if (!IsTransient(error) || (!IsUnc(from) && !IsUnc(to))) return FALSE;
    Sleep(300);
    TouchShare(from);
    TouchShare(to);
    if (MoveFileExW(from.c_str(), to.c_str(), flags)) {
        error = ERROR_SUCCESS;
        return TRUE;
    }
    error = GetLastError();
    return FALSE;
}

} // namespace luling::ops::share_retry
