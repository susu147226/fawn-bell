#include "app_commands.h"
#include "app_internal.h"
#include "app_input.h"
#include "app_navigation.h"
#include "app_ops_ui.h"
#include "app_runtime.h"
#include "places.h"
#include "../ui/ui_renderer.h"
#include <cstdio>
#include <exception>
#include <string>
#include <windowsx.h>

void Render(luling::AppState&);
bool WaitForShotReady(luling::AppState&);

// Issue #126: the staging tray must not swallow Ctrl+C / Ctrl+X, and it is off
// until the user turns it on in Settings.
int RunStagingTrayProbe(luling::AppState& s, const wchar_t* output) {
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
    const auto tray_count = [&] {
        size_t n = 0;
        for (const auto& batch : s.tray.batches()) n += batch.items.size();
        return n;
    };

    // 1) Off by default, the panel is hidden, and a copy leaves the tray empty.
    check(!s.appPrefs.staging_tray, "staging tray is off by default");
    NavigateTo(s, L"C:\\Program Files");
    check(ready(), "a folder with rows loads");
    pump(400);
    check(BuildVm(s, false).tray_deck.hidden, "the tray panel is hidden while the tray is off");
    if (app::Tab* tab = ActiveTab(s); tab && tab->snapshot && !tab->snapshot->empty()) {
        tab->SelectOnly(0);
        pump(150);
        MirrorSelectionToClipboard(s, false);   // what Ctrl+C now does
        pump(150);
        check(tray_count() == 0, "copy leaves the staging tray empty");
        check(!s.cutPaths.empty() == false, "copy does not mark rows as cut");
        MirrorSelectionToClipboard(s, true);    // what Ctrl+X now does
        pump(150);
        check(tray_count() == 0, "cut leaves the staging tray empty");
        check(!s.cutPaths.empty(), "cut still marks the rows as cut");
        s.cutPaths.clear();
    } else {
        check(false, "the folder listing was empty; copy semantics untested");
    }

    // 2) An explicit collect still fills it.
    CollectPathsToTray(s, {L"C:\\Program Files"}, false);
    check(tray_count() == 1, "the explicit collect still fills the tray");

    // 3) The Settings switch toggles the preference.
    NavigateTo(s, app::MakeSettingsPath());
    check(ready(), "the settings page loads");
    pump(400);
    // The switch sits below the fold; scroll the page so it is laid out in view.
    {
        const auto vm = BuildVm(s, false);
        const float maximum = s.renderer.SettingsMaxScroll(
            vm, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        s.settings.SetScroll(maximum, maximum);
    }
    pump(400);
    const auto find_toggle = [&](int index) {
        const auto vm = BuildVm(s);
        const D2D1_RECT_F rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                             static_cast<float>(s.compositor.Height()));
        for (int y = 0; y < static_cast<int>(rect.bottom); y += 6) {
            for (int x = 0; x < static_cast<int>(rect.right); x += 6) {
                const auto hit = s.renderer.HitTest(vm, rect, static_cast<float>(x), static_cast<float>(y));
                if (hit.region == ui::HitTestResult::SettingsToggle && hit.index == index)
                    return true;
            }
        }
        return false;
    };
    constexpr int kStagingTrayToggle = 35;
    check(find_toggle(kStagingTrayToggle), "the staging tray switch is present in Settings");
    std::string thrown;
    try {
        // Drive the same path a click on the switch takes (SettingsToggle click).
        s.settings.ToggleUi(kStagingTrayToggle);
    } catch (const std::exception& e) {
        thrown = std::string("std::exception: ") + e.what();
    } catch (...) {
        thrown = "non-std exception";
    }
    pump(200);
    check(thrown.empty(), "toggling the switch throws nothing (" + thrown + ")");
    check(s.appPrefs.staging_tray, "the switch turns the staging tray on");
    // Back to the folder: a copy must stay clipboard-only even with the tray on.
    NavigateTo(s, L"C:\\Program Files");
    ready();
    pump(300);
    check(!BuildVm(s, false).tray_deck.hidden, "the tray panel appears once the tray is enabled");
    if (app::Tab* tab = ActiveTab(s); tab && tab->snapshot && !tab->snapshot->empty()) {
        tab->SelectOnly(0);
        pump(120);
        MirrorSelectionToClipboard(s, false);
        pump(120);
        check(tray_count() == 1, "copy still does not fill the tray with the tray enabled");
    } else {
        check(false, "the folder listing was empty; the enabled-tray copy check did not run");
    }
    // Turn it off again from Settings.
    NavigateTo(s, app::MakeSettingsPath());
    ready();
    pump(300);
    {
        const auto vm = BuildVm(s, false);
        const float maximum = s.renderer.SettingsMaxScroll(
            vm, static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
        s.settings.SetScroll(maximum, maximum);
    }
    pump(300);
    s.settings.ToggleUi(kStagingTrayToggle);
    pump(200);
    check(!s.appPrefs.staging_tray, "the switch turns it off again");

    std::fprintf(log, "checks=%u failures=%u\n", checks, failures);
    std::fclose(log);
    return failures ? 1 : 0;
}
