// host_window.h — the hidden process window (multi-window).
//
// One per process, created before the first Luling window. It owns the
// single-instance endpoint (a second launch sends WM_COPYDATA here) and, as
// the port continues, the tray icon, global hotkey and service callbacks,
// handing each to the window it belongs to. It is a hidden top-level window
// rather than an HWND_MESSAGE one: message-only windows miss broadcasts such
// as TaskbarCreated, WM_SETTINGCHANGE and WM_ENDSESSION.
#pragma once
#include <windows.h>
#include <functional>
#include <optional>

namespace luling::app {

class HostWindow {
public:
    // Return a result to consume `message`; std::nullopt falls through to
    // DefWindowProc. Not called once Destroy has started.
    using Handler = std::function<std::optional<LRESULT>(HWND, UINT, WPARAM, LPARAM)>;

    HostWindow() = default;
    ~HostWindow() { Destroy(); }
    HostWindow(const HostWindow&) = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    bool Create(HINSTANCE instance, Handler handler);
    void Destroy();
    HWND Hwnd() const noexcept { return hwnd_; }
    static const wchar_t* ClassName() noexcept;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    HWND hwnd_ = nullptr;
    Handler handler_;
};

} // namespace luling::app
