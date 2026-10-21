#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>
#include <string_view>

namespace luling::app {

// Notification-area icon rule (#57). `background`: closing keeps Luling running
// (keep running after close, or global search). `mode`: 0 always, 1 only while
// the window is hidden, 2 never.
inline bool TrayIconWanted(bool background, int mode, bool window_hidden) {
    if (mode == 2) return false;
    return background && (mode != 1 || window_hidden);
}

class TrayController {
public:
    enum class CallbackResult { NotHandled, Handled, ExitRequested, RestartRequested };
    static constexpr UINT kCallbackMessage = WM_APP + 50;

    TrayController() = default;
    ~TrayController();
    TrayController(const TrayController&) = delete;
    TrayController& operator=(const TrayController&) = delete;

    void Attach(HWND hwnd, HINSTANCE instance);
    void Detach();
    bool SetVisible(bool visible);
    // `show_icon` false (icon set to never): hide without an icon; a second
    // launch of Luling brings the window back. The icon follows `window`, so
    // closing one of several windows hides that one rather than the window
    // the icon was created on.
    void HideWindow(HWND window, bool show_icon = true);
    void RestoreWindow();
    // Sign-in launch into the tray: add the icon and keep the window hidden.
    // Returns false (caller shows the window) when the icon cannot be added
    // although the taskbar exists; before Explorer is up the icon is added
    // on TaskbarCreated. `maximized`: the first restore shows it maximized.
    // `show_icon` false: stay hidden without an icon (see HideWindow).
    bool StartHidden(bool maximized, bool show_icon = true);
    // Explorer (re)created the taskbar: icons added earlier are gone.
    static UINT TaskbarCreatedMessage();
    void HandleTaskbarCreated();
    bool IconVisible() const { return icon_added_; }
    // Runs inside RestoreWindow() just before a hidden window is shown again
    // (tray click, tray "Open", a second launch), so the app can reset it first.
    void SetBeforeRestore(std::function<void()> hook) { before_restore_ = std::move(hook); }
    // Explains the first close-to-tray, where the window disappears but the app
    // keeps running. Only works once the icon is visible.
    bool ShowBalloon(std::wstring_view title, std::wstring_view message);
    CallbackResult HandleCallback(LPARAM event);

    bool IsVisible() const noexcept { return icon_added_; }
    // The window whose hide/restore the icon drives; a closing window hands
    // it to the one used next (multi-window).
    HWND Window() const noexcept { return hwnd_; }
    void Retarget(HWND hwnd) noexcept {
        if (hwnd_ && hwnd) hwnd_ = hwnd;
    }

private:
    NOTIFYICONDATAW IconData(UINT flags = 0) const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    bool icon_added_ = false;
    bool wanted_visible_ = false;   // last SetVisible request, for TaskbarCreated
    bool restore_maximized_ = false;
    std::function<void()> before_restore_;
};

} // namespace luling::app
