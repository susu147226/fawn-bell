#include "../ipc/preview_protocol.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
namespace {
int failures = 0;
void Check(bool ok, const char* label) { std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n'; failures += !ok; }
struct Host {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION child{};
    ~Host() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        if (child.hProcess) {
            if (WaitForSingleObject(child.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 91);
            CloseHandle(child.hProcess); CloseHandle(child.hThread);
        }
    }
    bool Start(const wchar_t* mode) {
        wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
        auto exe = std::filesystem::path(path).parent_path() / L"Pulse.Preview.exe";
        auto command = L"\"" + exe.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" " + mode;
        STARTUPINFOW start{sizeof(start)};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &child)) return false;
        const auto name = pulse::ipc::PreviewPipeName(GetCurrentProcessId());
        const auto end = GetTickCount64() + 5000;
        while (GetTickCount64() < end) {
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                ULONG server = 0; return GetNamedPipeServerProcessId(pipe, &server) && server == child.dwProcessId;
            }
            if (WaitForSingleObject(child.hProcess, 10) == WAIT_OBJECT_0) return false;
        }
        return false;
    }
    bool Read(void* output, DWORD size, ULONGLONG end) {
        auto* at = static_cast<unsigned char*>(output);
        while (size) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || GetTickCount64() >= end) return false;
            if (!available) { Sleep(1); continue; }
            DWORD got = 0;
            if (!ReadFile(pipe, at, (std::min)(available, size), &got, nullptr) || !got) return false;
            size -= got; at += got;
        }
        return true;
    }
    bool Request(const std::wstring& path, uint32_t id, bool fail_open,
                 pulse::ipc::PreviewResponse& response, bool& opened) {
        pulse::ipc::PreviewRequest request{};
        request.request_id = id; request.generation = id;
        request.path_chars = static_cast<uint32_t>(path.size());
        request.attrs = FILE_ATTRIBUTE_NORMAL; request.flags = pulse::ipc::kPreviewRequestFlagGrid; request.pixel_size = 32;
        if (!pulse::ipc::WriteAll(pipe, &request, sizeof(request)) ||
            !pulse::ipc::WriteAll(pipe, path.data(), request.path_chars * static_cast<DWORD>(sizeof(wchar_t)))) return false;
        const auto end = GetTickCount64() + 5000;
        if (!Read(&response, sizeof(response), end) || response.magic != pulse::ipc::kPreviewMagic ||
            response.request_id != id || response.generation != id || response.mapping_chars > 512 ||
            response.text_chars > 65536 || response.error_chars > 65536 || response.property_count) return false;
        std::wstring name(response.mapping_chars, L'\0');
        std::vector<wchar_t> text(response.text_chars), error(response.error_chars);
        if (!Read(name.data(), response.mapping_chars * static_cast<DWORD>(sizeof(wchar_t)), end) ||
            !Read(text.data(), response.text_chars * static_cast<DWORD>(sizeof(wchar_t)), end) ||
            !Read(error.data(), response.error_chars * static_cast<DWORD>(sizeof(wchar_t)), end)) return false;
        opened = false;
        if (!name.empty()) {
            if (fail_open) name += L".deliberately-absent";
            HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
            if (mapping) {
                void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, size_t(response.stride) * response.height);
                opened = view != nullptr;
                if (view) UnmapViewOfFile(view);
                CloseHandle(mapping);
            }
            // Production consumer acknowledges every advertised mapping, even if open failed.
            unsigned char ack = 1;
            if (!pulse::ipc::WriteAll(pipe, &ack, 1)) return false;
        }
        return true;
    }
};
}
int wmain() {
    const auto root = std::filesystem::absolute(L"bench_data"); std::filesystem::create_directories(root);
    const auto folder = root / (L"mapping-ack-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(folder.c_str(), nullptr)) return 2;
    const auto bitmap = folder / L"pixel.bmp", text = folder / L"next.txt";
    const unsigned char bmp[]{'B','M',58,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,1,0,0,0,1,0,0,0,1,0,24,0,
        0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,255,0};
    { std::ofstream file(bitmap, std::ios::binary); file.write(reinterpret_cast<const char*>(bmp), sizeof(bmp)); }
    { std::ofstream file(text); file << "next request stays aligned"; }
    const auto large_gif = folder / L"over-budget.gif";
    { const unsigned char gif[]{'G','I','F','8','9','a',255,255,255,255,0,0,0,0x3b};
      std::ofstream file(large_gif, std::ios::binary); file.write(reinterpret_cast<const char*>(gif), sizeof(gif)); }
    for (int scenario = 0; scenario < 4; ++scenario) {
        Host host;
        const wchar_t* mode = scenario == 0 ? L"--test-map-create-failure" : scenario == 1 ? L"--test-map-view-failure" : L"";
        Check(host.Start(mode), "start owned isolated preview host");
        if (host.pipe == INVALID_HANDLE_VALUE) continue;
        pulse::ipc::PreviewResponse response{}; bool opened = false;
        Check(host.Request(bitmap, 1, scenario == 2, response, opened), "first bitmap response completes within deadline");
        if (scenario < 2) Check(response.status == 2 && response.mapping_chars == 0 && !opened, "Create or Map failure advertises no mapping and needs no ACK");
        else Check(response.status == 0 && response.mapping_chars != 0 && opened == (scenario == 3), "client-open failure and normal mapping exercise advertised ACK");
        Check(host.Request(text, 2, false, response, opened) && response.status == 0, "same connection next header and payload remain aligned");
        Check(host.Request(bitmap, 3, false, response, opened) && response.status == 0 && opened, "same host subsequent bitmap mapping and ACK recover normally");
        Check(host.Request(large_gif, 4, false, response, opened) && response.status != 0 && response.mapping_chars == 0 && !opened,
              "GIF source budget is an explicit failed IPC response without bitmap or fallback");
        Check(host.Request(text, 5, false, response, opened) && response.status == 0,
              "GIF rejection leaves subsequent request aligned");
    }
    std::filesystem::remove_all(folder);
    std::cout << "Failures: " << failures << '\n'; return failures ? 1 : 0;
}
