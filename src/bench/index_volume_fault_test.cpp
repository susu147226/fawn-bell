#include "../index/index_engine.h"
#include "../index/index_volume_fault_hooks.h"
#include "../index/index_paths.h"
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdio>

namespace pulse::index::volume_fault_test {
enum class Mode { Fallback, Stopped, Cancelled, UsnFailure, BadPage, CancelUsn, Complete, SecondVolumeFails };
Mode mode = Mode::Fallback;
unsigned opens = 0, closes = 0, mft_calls = 0, usn_calls = 0;
std::atomic<bool>* active_running = nullptr;
void Reset(Mode value) { mode = value; opens = closes = mft_calls = usn_calls = 0; active_running = nullptr; }
HANDLE OpenVolume(wchar_t letter) { ++opens; return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(letter)); }
BOOL CloseHandle(HANDLE) { ++closes; return TRUE; }
bool QueryJournal(HANDLE, uint64_t& id, int64_t& next) { id = 123; next = 456; return true; }
uint64_t RootFrn(wchar_t) { return 5; }
bool IsAdmin() { return true; }
std::vector<VolumeInfo> ConfiguredVolumes() {
    std::vector<VolumeInfo> result;
    for (const wchar_t letter : {L'Q', L'R'}) {
        VolumeInfo volume;
        volume.id = std::wstring(1, letter); volume.mount_point = std::wstring(1, letter) + L":\\";
        volume.kind = VolumeKind::Fixed;
        volume.online = volume.enabled = volume.supported = true;
        volume.file_system = L"NTFS";
        result.push_back(std::move(volume));
    }
    return result;
}
MftReadResult EnumerateMft(HANDLE volume, std::atomic<bool>* running,
    const std::function<void(size_t)>& progress, const std::function<bool(MftFile&&)>& emit) {
    ++mft_calls; active_running = running;
    MftFile file;
    file.frn = 42; file.parent = 5; file.name_type = 1;
    const bool complete = mode == Mode::Complete ||
        (mode == Mode::SecondVolumeFails && reinterpret_cast<uintptr_t>(volume) == L'Q');
    file.name = complete ? L"candidate.txt" : L"partial.txt";
    emit(std::move(file));
    progress(1);
    if (mode == Mode::Cancelled) { *running = false; return MftReadResult::Failed; }
    if (mode == Mode::Stopped) return MftReadResult::Stopped;
    return complete ? MftReadResult::Complete : MftReadResult::Failed;
}
BOOL DeviceIoControl(HANDLE, DWORD code, LPVOID input, DWORD, LPVOID output, DWORD capacity,
    LPDWORD returned, LPOVERLAPPED) {
    ++usn_calls;
    if (code != FSCTL_ENUM_USN_DATA) { SetLastError(ERROR_INVALID_FUNCTION); return FALSE; }
    if (mode == Mode::SecondVolumeFails) { SetLastError(ERROR_READ_FAULT); return FALSE; }
    const auto* med = static_cast<MFT_ENUM_DATA_V0*>(input);
    if (med->StartFileReferenceNumber) {
        SetLastError(mode == Mode::UsnFailure ? ERROR_READ_FAULT : ERROR_HANDLE_EOF);
        return FALSE;
    }
    const std::wstring name = L"fallback.txt";
    const DWORD record_size = static_cast<DWORD>((offsetof(USN_RECORD_V2, FileName) + name.size() * 2 + 7) & ~size_t{7});
    const DWORD size = static_cast<DWORD>(sizeof(USN)) + record_size;
    if (capacity < size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    std::memset(output, 0, size);
    const uint64_t cursor = 100;
    std::memcpy(output, &cursor, sizeof(cursor));
    auto* record = reinterpret_cast<USN_RECORD_V2*>(static_cast<BYTE*>(output) + sizeof(USN));
    record->RecordLength = record_size;
    record->MajorVersion = mode == Mode::BadPage ? 3 : 2;
    record->FileReferenceNumber = 77; record->ParentFileReferenceNumber = 5;
    record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
    record->FileNameLength = static_cast<WORD>(name.size() * sizeof(wchar_t));
    std::memcpy(record->FileName, name.data(), record->FileNameLength);
    *returned = size;
    if (mode == Mode::CancelUsn && active_running) *active_running = false;
    return TRUE;
}
}

namespace pulse::index {
struct EngineTestAccess {
    static bool Volume(Engine& engine) {
        engine.running_ = true;
        return engine.IndexVolumeMft(volume_fault_test::ConfiguredVolumes().front());
    }
    static bool Candidate(Engine& engine, const wchar_t* name) {
        for (const auto& node : engine.build_.nodes)
            if (std::wstring_view(engine.build_.pool.data() + node.off, node.len) == name) return true;
        return false;
    }
    static bool EmptyCandidate(Engine& engine) { return engine.build_.nodes.empty() && engine.build_vols_.empty(); }
    static bool Seed(Engine& engine, const std::wstring& path) {
        Engine::Store store;
        engine.AddNodeLocked(store, -1, L"Q:", Engine::kFlagDir);
        engine.AddNodeLocked(store, 0, L"previous.txt", 0);
        if (!engine.WriteIndexFile(path, store, {}, 123456) ||
            !MoveFileExW((path + L".tmp").c_str(), path.c_str(), 0)) return false;
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!engine.MapIndexFile(path, mapped)) return false;
        engine.AdoptMappedLocked(std::move(mapped));
        engine.ready_ = true;
        return true;
    }
    static const void* Mapping(Engine& engine) { return engine.map_.get(); }
    static bool Gap(Engine& engine) { return engine.folder_size_gap_; }
    static void Rebuild(Engine& engine) { engine.running_ = true; engine.FullRebuild("fault-test"); }
    static void Stop(Engine& engine) { engine.running_ = false; }
};
}

int main() {
    using namespace pulse::index;
    namespace vf = volume_fault_test;
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(stdout); failures += !ok; };
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto root = parent / (L"volume-fault-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(root)) return 2;
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    SetMachineIndexScope(false);
    SetActiveIndexDirectory(root.wstring());
    for (const auto mode : {vf::Mode::Fallback, vf::Mode::Stopped, vf::Mode::Cancelled,
                           vf::Mode::UsnFailure, vf::Mode::BadPage, vf::Mode::CancelUsn}) {
        vf::Reset(mode);
        Engine engine;
        const bool result = EngineTestAccess::Volume(engine);
        check(result == (mode == vf::Mode::Fallback), "production volume scan returns expected terminal result");
        check(!EngineTestAccess::Candidate(engine, L"partial.txt"), "failed partial MFT records never enter tree");
        if (mode == vf::Mode::Fallback)
            check(EngineTestAccess::Candidate(engine, L"fallback.txt") && vf::usn_calls == 2,
                "USN fallback builds its own records and reaches real EOF branch");
        else
            check(EngineTestAccess::EmptyCandidate(engine), "stopped or failed enumeration publishes no partial tree");
        if (mode == vf::Mode::Stopped || mode == vf::Mode::Cancelled)
            check(vf::usn_calls == 0, "stopped/cancelled MFT never enters USN fallback");
        check(vf::opens == 1 && vf::closes == 1, "fake volume lifetime is balanced");
        EngineTestAccess::Stop(engine);
    }
    const auto snapshot = fs::path(CacheFilePath());
    auto bytes = [&] { std::ifstream file(snapshot, std::ios::binary); return std::vector<char>(std::istreambuf_iterator<char>(file), {}); };
    auto matches = [](Engine& engine, const wchar_t* name) { Query query; query.needle = L"\"" + std::wstring(name) + L"\""; return engine.Search(query).total; };
    {
        Engine engine;
        check(EngineTestAccess::Seed(engine, snapshot.wstring()), "write and map real private old snapshot");
        const auto original = bytes();
        const auto* mapping = EngineTestAccess::Mapping(engine);
        const auto count = engine.Count();
        for (const auto mode : {vf::Mode::SecondVolumeFails, vf::Mode::Cancelled, vf::Mode::Stopped}) {
            vf::Reset(mode);
            EngineTestAccess::Rebuild(engine);
            check(!original.empty() && bytes() == original, "failed rebuild preserves disk snapshot byte for byte");
            check(EngineTestAccess::Mapping(engine) == mapping && engine.Count() == count &&
                matches(engine, L"previous.txt") == 1 && matches(engine, L"candidate.txt") == 0,
                "failed rebuild preserves mapped search results and count");
            check(EngineTestAccess::EmptyCandidate(engine) && EngineTestAccess::Gap(engine),
                "failed rebuild discards candidate and marks incomplete coverage");
            check(engine.Status().find(L"未完成") != std::wstring::npos,
                "failed rebuild reports incomplete scan instead of ready success");
            if (mode == vf::Mode::SecondVolumeFails)
                check(vf::mft_calls == 2 && vf::usn_calls == 1, "later volume fails after earlier volume built successfully");
            else check(vf::usn_calls == 0, "cancelled/stopped rebuild never starts fallback");
        }
        vf::Reset(vf::Mode::Complete);
        EngineTestAccess::Rebuild(engine);
        check(matches(engine, L"candidate.txt") == 2 && matches(engine, L"previous.txt") == 0,
            "all-success control publishes both new volume trees");
        check(bytes() != original && !bytes().empty() && !EngineTestAccess::Gap(engine),
            "all-success control replaces real disk snapshot and clears gap");
        EngineTestAccess::Stop(engine);
    }
    SetActiveIndexDirectory({});
    if (root.parent_path() != parent || !root.filename().wstring().starts_with(L"volume-fault-")) return 2;
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "release mapping then remove exclusive fixture");
    return failures ? 1 : 0;
}
