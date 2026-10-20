// network_credentials.h — sign in to a share that turned the current account away.
#pragma once
#include <windows.h>

#include <string>

namespace luling::app {

// Offers the system's credential dialog for a share Luling could not list, the
// way Explorer does when a folder needs a sign-in.
//
// Nothing is written to the credential store: the connection is established with
// CONNECT_TEMPORARY and lasts for the session. The sign-in is only asked for when
// the failure actually looks like an authentication problem — an unreachable host
// gets no dialog. Returns true once the share is reachable, so the caller can
// list it again. `failure` receives a reason when the attempt fails.
bool PromptForShareCredentials(HWND owner, const std::wstring& unc, std::wstring* failure = nullptr);

// Background-safe: probes the share without credentials and reports whether a
// sign-in dialog would help. Never touches a window or prompts.
bool ShareSignInNeeded(const std::wstring& unc);

// Human-readable reason for a failed share connection; empty when unknown.
std::wstring ShareSignInFailureText(DWORD error);

} // namespace luling::app
