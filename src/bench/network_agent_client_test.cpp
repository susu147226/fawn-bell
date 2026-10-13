#include "../index/network_agent_client.h"
#include <atomic>
#include <cstdio>
#include <future>

namespace luling::index {
struct NetworkAgentClientTestAccess {
    static void Start(NetworkAgentClient& client, const std::wstring& pipe, std::promise<void>& done) {
        client.pipe_name_ = pipe;
        client.running_ = true;
        // This live process handle bypasses helper launching without touching the real singleton.
        client.agent_process_ = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
        client.session_requests_[7] = 77;
        client.StartWatchdog();
        client.search_thread_ = std::thread([&client, &done] {
            Query query; query.session_id = 7; query.needle = L"fixture";
            client.SearchRequest(query, 77);
            done.set_value();
        });
    }
    static bool Released(NetworkAgentClient& client) {
        std::lock_guard lock(client.pipe_mu_);
        return client.active_pipe_ == INVALID_HANDLE_VALUE;
    }
    // A client whose peer answers every frame, driven straight through Request.
    static void FastStart(NetworkAgentClient& client, const std::wstring& pipe) {
        client.pipe_name_ = pipe;
        client.running_ = true;
        client.agent_process_ = OpenProcess(SYNCHRONIZE, FALSE, GetCurrentProcessId());
        client.StartWatchdog();
    }
    static bool Request(NetworkAgentClient& client, uint32_t type, uint32_t id) {
        uint32_t response_type = 0;
        std::vector<uint8_t> response;
        return client.Request(type, id, {}, response_type, response);
    }
};
}
int main() {
    using namespace luling;
    const auto name = LR"(\\.\pipe\LulingNetworkFixture-)" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (pipe == INVALID_HANDLE_VALUE || !release) return 2;
    std::thread server([&] {
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            ipc::MsgHeader header{};
            if (ipc::PipeRead(pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
                std::vector<uint8_t> payload(header.payload_size);
                if (!payload.empty()) ipc::PipeRead(pipe, payload.data(), static_cast<DWORD>(payload.size()));
                WaitForSingleObject(release, 15000);
            }
        }
        DisconnectNamedPipe(pipe);
    });
    index::NetworkAgentClient client;
    std::promise<void> done; auto completed = done.get_future();
    const auto start = GetTickCount64();
    index::NetworkAgentClientTestAccess::Start(client, name, done);
    const bool timely = completed.wait_for(std::chrono::seconds(12)) == std::future_status::ready;
    const auto elapsed = GetTickCount64() - start;
    index::SearchResult result;
    const bool terminal = timely && client.TakeResult(77, result) && result.error != ERROR_SUCCESS;
    const bool released = timely && index::NetworkAgentClientTestAccess::Released(client);
    SetEvent(release);
    const auto stop = GetTickCount64(); client.Stop();
    const bool joined = GetTickCount64() - stop < 2000;
    server.join(); CloseHandle(pipe); CloseHandle(release);
    printf("[%s] silent isolated pipe produces a terminal search failure after deadline (%llu ms)\n",
        timely && terminal && elapsed >= 4500 ? "PASS" : "FAIL", elapsed);
    printf("[%s] timed-out request releases pipe and joins client thread\n", released && joined ? "PASS" : "FAIL");

    // A peer that answers every frame. Each answered request used to build a
    // jthread and wait on it with a stop_token timed wait, which left one Timer
    // and one Semaphore behind per call (issue #218). The watchdog is now the
    // only thing that touches kernel objects, so the handle count must hold.
    const auto fast_name = LR"(\\.\pipe\LulingNetworkFast-)" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64());
    HANDLE fast_pipe = CreateNamedPipeW(fast_name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (fast_pipe == INVALID_HANDLE_VALUE) return 2;
    std::atomic<bool> serving{true};
    std::thread fast_server([&] {
        while (serving.load()) {
            const BOOL connected = ConnectNamedPipe(fast_pipe, nullptr);
            if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) break;
            ipc::MsgHeader header{};
            if (ipc::PipeRead(fast_pipe, reinterpret_cast<uint8_t*>(&header), sizeof(header))) {
                std::vector<uint8_t> payload(header.payload_size);
                if (payload.empty() ||
                    ipc::PipeRead(fast_pipe, payload.data(), static_cast<DWORD>(payload.size()))) {
                    const auto reply = index::agent::MakeHeader(index::agent::RSP_ROOTS, header.request_id, 0);
                    ipc::PipeWrite(fast_pipe, reinterpret_cast<const uint8_t*>(&reply), sizeof(reply));
                }
            }
            DisconnectNamedPipe(fast_pipe);
        }
    });
    index::NetworkAgentClient fast_client;
    index::NetworkAgentClientTestAccess::FastStart(fast_client, fast_name);
    constexpr int kRequests = 400;
    DWORD handles_before = 0, handles_after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles_before);
    int answered = 0;
    for (uint32_t i = 0; i < kRequests; ++i) {
        if (index::NetworkAgentClientTestAccess::Request(fast_client, index::agent::REQ_ROOTS, 1000 + i))
            ++answered;
    }
    GetProcessHandleCount(GetCurrentProcess(), &handles_after);
    const long growth = static_cast<long>(handles_after) - static_cast<long>(handles_before);
    serving = false;
    HANDLE unblock = CreateFileW(fast_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (unblock != INVALID_HANDLE_VALUE) CloseHandle(unblock);
    fast_client.Stop();
    fast_server.join();
    CloseHandle(fast_pipe);
    printf("[%s] responsive peer answers every request (%d/%d)\n",
        answered == kRequests ? "PASS" : "FAIL", answered, kRequests);
    printf("[%s] %d requests leak no kernel handles (handles %ld -> %ld, growth %ld)\n",
        growth <= 16 ? "PASS" : "FAIL", kRequests,
        static_cast<long>(handles_before), static_cast<long>(handles_after), growth);
    return timely && terminal && released && joined && answered == kRequests && growth <= 16 ? 0 : 1;
}
