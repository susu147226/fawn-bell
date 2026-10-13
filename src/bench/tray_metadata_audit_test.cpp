#include "../app/tray_compare_metadata.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
using pulse::app::TrayCompareMetadata;
static int failures;
static void Check(bool value, const char* message) {
    printf("[%s] %s\n", value ? "PASS" : "FAIL", message); failures += !value;
}
static bool Wait(const std::function<bool()>& done) {
    const auto until = GetTickCount64() + 3000;
    while (!done()) { if (GetTickCount64() > until) return false; Sleep(5); }
    return true;
}
int main() {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> calls = 0;
    const DWORD ui = GetCurrentThreadId();
    std::atomic<bool> off_ui = true;
    auto cache = std::make_unique<TrayCompareMetadata>([&](const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& data) {
        if (GetCurrentThreadId() == ui) off_ui = false;
        ++calls;
        if (path == L"old") { SetEvent(entered); WaitForSingleObject(release, 3000); }
        data.nFileSizeLow = path == L"new" ? 42 : 7;
        SetEvent(returned);
        return DWORD{ERROR_SUCCESS};
    });
    cache->Request({L"old", L"unused"});
    Check(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "delayed provider runs on worker");
    const auto start = GetTickCount64();
    for (int i = 0; i < 10000; ++i) cache->Request({L"new", L"other"});
    Check(GetTickCount64() - start < 250 && calls == 1, "10000 UI requests stay bounded while provider blocks");
    SetEvent(release);
    Check(Wait([&] { return cache->Request({L"new", L"other"}).ready; }), "latest pair completes after old provider releases");
    const auto snapshot = cache->Request({L"new", L"other"});
    Check(snapshot.data[0].nFileSizeLow == 42 && calls == 3 && off_ui,
        "old pair cannot publish or read its second file after replacement");
    cache->Disable();
    const int stopped_calls = calls;
    Sleep(20);
    Check(calls == stopped_calls, "disabled comparison schedules no more probes");
    ResetEvent(entered); ResetEvent(release); ResetEvent(returned);
    cache->Request({L"old", L"unused"});
    Check(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "second delayed query enters provider");
    const auto close_start = GetTickCount64();
    cache.reset();
    Check(GetTickCount64() - close_start < 100, "closing while provider blocks never joins on UI thread");
    SetEvent(release);
    Check(WaitForSingleObject(returned, 3000) == WAIT_OBJECT_0, "closed cache drains without accessing UI state");
    CloseHandle(entered); CloseHandle(release); CloseHandle(returned);
    std::atomic<int> errors = 0;
    TrayCompareMetadata failing([&](const std::wstring&, WIN32_FILE_ATTRIBUTE_DATA&) { ++errors; return DWORD{ERROR_ACCESS_DENIED}; });
    for (int attempt = 0; attempt < 3; ++attempt) {
        failing.Request({L"unreadable", L"other"}, ~ULONGLONG{0});
        Check(Wait([&] { return failing.TakeChanged(); }), "failed metadata attempt reports completion");
    }
    for (int i = 0; i < 100; ++i) failing.Request({L"unreadable", L"other"}, ~ULONGLONG{0});
    Check(errors == 3 && failing.Request({L"unreadable", L"other"}).error == ERROR_ACCESS_DENIED,
        "unreadable pair retains error and limits retries to three");
    std::atomic<bool> deny = false;
    TrayCompareMetadata recovering([&](const std::wstring&, WIN32_FILE_ATTRIBUTE_DATA&) {
        return static_cast<DWORD>(deny ? ERROR_ACCESS_DENIED : ERROR_SUCCESS);
    });
    for (int refresh = 0; refresh < 4; ++refresh) {
        recovering.Request({L"refresh", L"other"}, ~ULONGLONG{0});
        Check(Wait([&] { return recovering.TakeChanged(); }), "successful metadata refresh completes");
    }
    deny = true;
    recovering.Request({L"refresh", L"other"}, ~ULONGLONG{0});
    Check(Wait([&] { return recovering.TakeChanged(); }) &&
        recovering.Request({L"refresh", L"other"}).error == ERROR_ACCESS_DENIED,
        "temporary metadata error is reported after repeated successful refreshes");
    deny = false;
    recovering.Request({L"refresh", L"other"}, ~ULONGLONG{0});
    Check(Wait([&] { return recovering.TakeChanged(); }) &&
        recovering.Request({L"refresh", L"other"}).error == ERROR_SUCCESS,
        "successful refreshes do not exhaust subsequent error recovery budget");
    const auto root = std::filesystem::absolute(L"bench_data/tray-metadata-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto file = root / L"sample.txt";
    { std::ofstream output(file, std::ios::binary); output << "fixture"; }
    TrayCompareMetadata actual;
    Check(Wait([&] { return actual.Request({file.wstring(), file.wstring()}).ready; }) &&
        actual.Request({file.wstring(), file.wstring()}).data[0].nFileSizeLow == 7,
        "production Windows provider reads actual isolated file metadata");
    actual.TakeChanged();
    { std::ofstream output(file, std::ios::binary); output << "updated fixture"; }
    actual.Request({file.wstring(), file.wstring()}, ~ULONGLONG{0});
    Check(Wait([&] { return actual.TakeChanged(); }) &&
        actual.Request({file.wstring(), file.wstring()}).data[0].nFileSizeLow == 15,
        "same file pair refreshes actual changed file size");
    std::filesystem::remove(file);
    actual.Request({file.wstring(), file.wstring()}, ~ULONGLONG{0});
    Check(Wait([&] { return actual.TakeChanged(); }) &&
        actual.Request({file.wstring(), file.wstring()}).error == ERROR_FILE_NOT_FOUND,
        "deleted fixture reports missing metadata rather than stale size");
    { std::ofstream output(file, std::ios::binary); output << "restored"; }
    actual.Request({file.wstring(), file.wstring()}, ~ULONGLONG{0});
    Check(Wait([&] { return actual.TakeChanged(); }) &&
        actual.Request({file.wstring(), file.wstring()}).error == ERROR_SUCCESS &&
        actual.Request({file.wstring(), file.wstring()}).data[0].nFileSizeLow == 8,
        "recreated fixture recovers actual metadata without changing selected pair");
    return failures ? 1 : 0;
}
