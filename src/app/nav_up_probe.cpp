#include "app_internal.h"
#include "app_input.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "../ui/ui_renderer.h"
#include <cstdio>
#include <exception>
#include <string>
#include <windowsx.h>

void Render(luling::AppState&);
bool WaitForShotReady(luling::AppState&);

// B0 / acceptance 19: "up" must be impossible at a top-level location rather
// than throwing, and a thrown exception must never leave the click path.
int RunNavUpProbe(luling::AppState& s, const wchar_t* output) {
    using namespace luling;
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"w") || !log) return 2;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const std::string& label) {
        ++checks;
        if (!ok) ++failures;
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label.c_str());
        std::fflush(log);
    };
    const auto pump = [&](DWORD ms) {
        const auto until = GetTickCount64() + ms;
        do {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            ProcessPendingResults(s);
            Render(s);
            Sleep(10);
        } while (GetTickCount64() < until);
    };
    const auto ready = [&] {
        s.shot.start = std::chrono::steady_clock::now();
        return WaitForShotReady(s);
    };

    // Locate the toolbar "up" button the way a click does: by hit-testing.
    const auto find_up = [&](int& out_x, int& out_y) {
        const auto vm = BuildVm(s);
        const D2D1_RECT_F rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                             static_cast<float>(s.compositor.Height()));
        for (int y = 0; y < 160; y += 2) {
            for (int x = 0; x < 760; x += 2) {
                if (s.renderer.HitTest(vm, rect, static_cast<float>(x), static_cast<float>(y)).region ==
                    ui::HitTestResult::NavUp) {
                    out_x = x;
                    out_y = y;
                    return true;
                }
            }
        }
        return false;
    };

    // Real click through the dispatcher; returns the exception text, or "".
    const auto click_up = [&]() -> std::string {
        int x = 0, y = 0;
        if (!find_up(x, y)) return "up button was not hit-testable";
        try {
            HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        } catch (const std::exception& e) {
            return std::string("std::exception: ") + e.what();
        } catch (...) {
            return "non-std exception";
        }
        return {};
    };

    // 1) This PC: nothing above it. The button must be un-hit-testable, and the
    // action itself must be a safe no-op even if another route reaches it.
    NavigateTo(s, L"");
    check(ready(), "This PC loads");
    pump(400);
    int x = 0, y = 0;
    check(!find_up(x, y), "up button has no hit target at This PC (18.2 B0)");
    std::string top_click;
    try {
        GoUp(s);
    } catch (const std::exception& e) {
        top_click = std::string("std::exception: ") + e.what();
    } catch (...) {
        top_click = "non-std exception";
    }
    check(top_click.empty(), "the up action at This PC throws nothing (" + top_click + ")");
    pump(400);
    check(s.hwnd != nullptr && IsWindow(s.hwnd) != 0, "window survives up at This PC");

    // 2) Drive root: "up" must reach This PC without throwing.
    NavigateTo(s, L"C:\\");
    check(ready(), "drive root loads");
    pump(400);
    const std::string root_click = click_up();
    check(root_click.empty(), "up at a drive root throws nothing (" + root_click + ")");
    pump(600);
    const app::Tab* tab = ActiveTab(s);
    check(tab && tab->current_path.empty(), "up at a drive root lands on This PC");
    check(s.hwnd != nullptr && IsWindow(s.hwnd) != 0, "window survives up at a drive root");

    std::fprintf(log, "checks=%u failures=%u\n", checks, failures);
    std::fclose(log);
    return failures ? 1 : 0;
}
