#include "../app/tray_compare_metadata.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
using luling::app::TrayCompareMetadata;
using luling::app::TrayMetadataKey;
using luling::app::TrayMetadataSnapshot;
static int failures;
static void Check(bool value, const char* message) {
    printf("[%s] %s\n", value ? "PASS" : "FAIL", message); failures += !value;
}
static bool Wait(const std::function<bool()>& done) {
    const auto until = GetTickCount64() + 4000;
    while (!done()) { if (GetTickCount64() > until) return false; Sleep(5); }
    return true;
}
static TrayMetadataKey Key(const std::wstring& first, const std::wstring& second) {
    return TrayMetadataKey{{}, {first, second}};
}
static bool WaitStatus(TrayCompareMetadata& cache, const TrayMetadataKey& key,
                       TrayMetadataSnapshot::Status status) {
    return Wait([&] { return cache.Read(key, nullptr).status == status; });
}
int main() {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> calls = 0;
    const DWORD ui = GetCurrentThreadId();
    std::atomic<bool> off_ui = true;
    auto provider = [&](const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& data, const std::atomic<bool>& cancelled) {
        if (GetCurrentThreadId() == ui) off_ui = false;
        ++calls;
        if (path.starts_with(L"old")) { SetEvent(entered); WaitForSingleObject(release, 3000); }
        if (cancelled) return DWORD(ERROR_CANCELLED);
        data.nFileSizeLow = path == L"new" ? 42 : 7;
        SetEvent(returned);
        return DWORD(ERROR_SUCCESS);
    };
    auto cache = std::make_unique<TrayCompareMetadata>(provider);
    const auto old_key = Key(L"old", L"unused");
    Check(cache->Read(old_key, nullptr).status == TrayMetadataSnapshot::Status::Pending,
          "first read schedules the provider");
    Check(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "delayed provider runs on worker");
    const auto start = GetTickCount64();
    for (int i = 0; i < 10000; ++i) cache->Read(old_key, nullptr);
    Check(GetTickCount64() - start < 250 && calls == 1,
          "10000 reads of a pending pair stay cached while the provider blocks");
    SetEvent(release);
    Check(WaitStatus(*cache, old_key, TrayMetadataSnapshot::Status::Ready),
          "blocked pair completes once the provider is released");
    const auto snapshot = cache->Read(old_key, nullptr);
    Check(snapshot.files[0].nFileSizeLow == 7 && snapshot.files[1].nFileSizeLow == 7 && calls == 2 && off_ui,
          "completed pair reads both files off the UI thread");

    // A replacement pair cancels the blocked one; the old job must not publish.
    ResetEvent(entered); ResetEvent(release); ResetEvent(returned);
    const auto new_key = Key(L"new", L"other");
    const auto blocking_key = Key(L"old2", L"unused");
    Check(cache->Read(blocking_key, nullptr).status == TrayMetadataSnapshot::Status::Pending,
          "second pair starts a new generation");
    Check(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "replacement provider is entered");
    cache->Read(new_key, nullptr);
    Check(WaitStatus(*cache, new_key, TrayMetadataSnapshot::Status::Ready),
          "replacement pair completes while the old provider still blocks");
    Check(cache->Read(new_key, nullptr).files[0].nFileSizeLow == 42,
          "replacement pair publishes its own metadata");
    SetEvent(release);
    Sleep(50);
    const auto after_old = cache->Read(new_key, nullptr);
    Check(after_old.status == TrayMetadataSnapshot::Status::Ready && after_old.files[0].nFileSizeLow == 42,
          "old generation cannot overwrite the replacement result");

    // Cancel drops the in-flight pair without publishing it.
    ResetEvent(entered); ResetEvent(release);
    Check(cache->Read(Key(L"old3", L"unused"), nullptr).status == TrayMetadataSnapshot::Status::Pending,
          "cancelled pair starts");
    Check(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "cancelled provider is entered");
    cache->Cancel();
    SetEvent(release);
    Sleep(50);
    const auto after_cancel = cache->Read(new_key, nullptr);
    Check(after_cancel.status == TrayMetadataSnapshot::Status::Pending || after_cancel.files[0].nFileSizeLow == 42,
          "a cancelled generation never publishes stale metadata");

    // Destruction never joins on the UI thread while a provider blocks.
    ResetEvent(entered); ResetEvent(release); ResetEvent(returned);
    auto closing = std::make_unique<TrayCompareMetadata>(provider);
    closing->Read(old_key, nullptr);
    Check(WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0, "closing provider is entered");
    const auto close_start = GetTickCount64();
    closing.reset();
    Check(GetTickCount64() - close_start < 100, "closing while the provider blocks never joins on the UI thread");
    SetEvent(release);
    Check(WaitForSingleObject(returned, 3000) == WAIT_OBJECT_0, "closed cache drains without touching UI state");

    // Errors are cached until the retry window opens; another pair probes again.
    std::atomic<int> errors = 0;
    TrayCompareMetadata failing([&](const std::wstring&, WIN32_FILE_ATTRIBUTE_DATA&, const std::atomic<bool>&) {
        ++errors;
        return DWORD(ERROR_ACCESS_DENIED);
    });
    const auto bad_key = Key(L"unreadable", L"other");
    Check(WaitStatus(failing, bad_key, TrayMetadataSnapshot::Status::Error),
          "failed metadata attempt reports an error");
    for (int i = 0; i < 100; ++i) failing.Read(bad_key, nullptr);
    Check(errors == 1 && failing.Read(bad_key, nullptr).error == ERROR_ACCESS_DENIED,
          "an unreadable pair keeps its error inside the retry window");
    const auto bad_key2 = Key(L"unreadable2", L"other");
    Check(WaitStatus(failing, bad_key2, TrayMetadataSnapshot::Status::Error) && errors == 2,
          "a different pair probes again instead of reusing the error");

    // Production provider over a real isolated file.
    const auto root = std::filesystem::absolute(L"bench_data/tray-metadata-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    const auto file = root / L"sample.txt";
    { std::ofstream output(file, std::ios::binary); output << "fixture"; }
    TrayCompareMetadata actual;
    const auto file_key = Key(file.wstring(), file.wstring());
    Check(WaitStatus(actual, file_key, TrayMetadataSnapshot::Status::Ready) &&
          actual.Read(file_key, nullptr).files[0].nFileSizeLow == 7,
          "production Windows provider reads actual isolated file metadata");
    { std::ofstream output(file, std::ios::binary); output << "updated fixture"; }
    Check(actual.Read(file_key, nullptr).files[0].nFileSizeLow == 7,
          "a fresh success is served from cache inside its refresh window");
    Sleep(2100);
    Check(WaitStatus(actual, file_key, TrayMetadataSnapshot::Status::Ready) &&
          actual.Read(file_key, nullptr).files[0].nFileSizeLow == 15,
          "same file pair refreshes actual changed file size after the window");
    std::filesystem::remove(file);
    Sleep(2100);
    Check(WaitStatus(actual, file_key, TrayMetadataSnapshot::Status::Error) &&
          actual.Read(file_key, nullptr).error == ERROR_FILE_NOT_FOUND,
          "deleted fixture reports missing metadata rather than stale size");
    std::filesystem::remove_all(root);
    CloseHandle(entered); CloseHandle(release); CloseHandle(returned);
    return failures ? 1 : 0;
}
