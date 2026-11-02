#include "svn_status.h"
#include "fs_enum.h"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace luling::fs {
namespace {

constexpr UINT kSvnStatusReady = WM_APP + 87;
constexpr uint64_t kCacheMs = 5000;
constexpr uint64_t kUnavailableMs = 30000;
constexpr DWORD kRunTimeoutMs = 3000;

std::wstring LowerKey(std::wstring text) {
    for (wchar_t& c : text) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        else if (c == L'/') c = L'\\';
    }
    while (text.size() > 3 && text.back() == L'\\') text.pop_back();
    return text;
}

std::wstring ResolveSvnExe() {
    wchar_t env[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(L"LULING_SVN_PATH", env,
                                            static_cast<DWORD>(std::size(env)));
    if (n > 0 && n < std::size(env) && GetFileAttributesW(env) != INVALID_FILE_ATTRIBUTES)
        return env;
    wchar_t found[MAX_PATH * 2]{};
    if (SearchPathW(nullptr, L"svn.exe", nullptr, static_cast<DWORD>(std::size(found)),
                    found, nullptr))
        return found;
    return {};
}

bool HasWorkingCopy(const std::wstring& directory) {
    std::wstring current = directory;
    for (int depth = 0; depth < 32; ++depth) {
        if (current.empty()) return false;
        const std::wstring marker = current + L"\\.svn";
        const DWORD attrs = GetFileAttributesW(marker.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return true;
        if (current.size() <= 3) return false;
        const size_t slash = current.find_last_of(L'\\');
        if (slash == std::wstring::npos || slash < 2) return false;
        current.resize(slash == 2 ? 3 : slash);
    }
    return false;
}

struct SvnRun {
    bool ok = false;
    std::string bytes;
};

SvnRun RunSvnStatus(const std::wstring& exe, const std::wstring& directory) {
    SvnRun run;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return run;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = L"\"" + exe + L"\" status --xml --depth=immediates \"" +
                           directory + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!started) {
        CloseHandle(readPipe);
        return run;
    }

    const uint64_t deadline = GetTickCount64() + kRunTimeoutMs;
    char buffer[8192];
    for (;;) {
        DWORD available = 0;
        if (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            if (!ReadFile(readPipe, buffer, (std::min)(available, static_cast<DWORD>(sizeof(buffer))),
                          &got, nullptr) || got == 0)
                break;
            run.bytes.append(buffer, got);
            continue;
        }
        const DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        if (wait == WAIT_OBJECT_0) {
            // Drain what is left after exit.
            for (;;) {
                DWORD got = 0;
                if (!ReadFile(readPipe, buffer, static_cast<DWORD>(sizeof(buffer)), &got, nullptr) ||
                    got == 0)
                    break;
                run.bytes.append(buffer, got);
            }
            run.ok = true;
            break;
        }
        if (GetTickCount64() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    return run;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::wstring Attribute(const std::wstring& block, const std::wstring& name) {
    const std::wstring needle = name + L"=\"";
    const size_t at = block.find(needle);
    if (at == std::wstring::npos) return {};
    const size_t from = at + needle.size();
    const size_t to = block.find(L'"', from);
    if (to == std::wstring::npos) return {};
    return block.substr(from, to - from);
}

SvnItem ItemFromName(const std::wstring& item) {
    if (item == L"modified") return SvnItem::Modified;
    if (item == L"added") return SvnItem::Added;
    if (item == L"deleted") return SvnItem::Deleted;
    if (item == L"unversioned") return SvnItem::Unversioned;
    if (item == L"conflicted") return SvnItem::Conflicted;
    if (item == L"ignored") return SvnItem::Ignored;
    if (item == L"replaced") return SvnItem::Replaced;
    if (item == L"missing") return SvnItem::Missing;
    return SvnItem::None;
}

SvnStatusMap ParseStatus(const std::string& utf8) {
    SvnStatusMap map;
    map.in_working_copy = true;
    const std::wstring xml = Utf8ToWide(utf8);
    size_t at = 0;
    while ((at = xml.find(L"<entry ", at)) != std::wstring::npos) {
        const size_t end = xml.find(L"</entry>", at);
        const size_t blockEnd = end == std::wstring::npos ? xml.size() : end;
        const std::wstring block = xml.substr(at, blockEnd - at);
        const std::wstring path = Attribute(block, L"path");
        std::wstring item_text;
        const size_t wc = block.find(L"<wc-status");
        if (wc != std::wstring::npos) {
            const size_t itemAt = block.find(L"item=\"", wc);
            if (itemAt != std::wstring::npos) {
                const size_t from = itemAt + 6;
                const size_t to = block.find(L'"', from);
                if (to != std::wstring::npos) item_text = block.substr(from, to - from);
            }
        }
        const SvnItem item = ItemFromName(item_text);
        if (!path.empty() && item != SvnItem::None &&
            item != SvnItem::Ignored && item != SvnItem::Unversioned) {
            const size_t slash = path.find_last_of(L"\\/");
            std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
            map.items[LowerKey(std::move(name))] = item;
        }
        at = blockEnd;
    }
    return map;
}

struct ProbeResult {
    SvnStatusMap map;
    bool client = false;
};

ProbeResult Probe(const std::wstring& directory) {
    ProbeResult result;
    if (directory.empty()) return result;
    const std::wstring exe = ResolveSvnExe();
    if (exe.empty()) return result;
    result.client = true;
    if (!HasWorkingCopy(directory)) return result; // map.in_working_copy stays false
    const SvnRun run = RunSvnStatus(exe, directory);
    if (!run.ok || run.bytes.empty()) return result;
    result.map = ParseStatus(run.bytes);
    return result;
}

struct CacheEntry {
    SvnStatusMap map;
    uint64_t tick = 0;
    uint64_t ttl = kCacheMs;
};

std::mutex g_mutex;
std::unordered_map<std::wstring, CacheEntry> g_cache;
std::unordered_set<std::wstring> g_pending;
std::vector<std::pair<std::wstring, HWND>> g_queue;
bool g_worker_running = false;

void WorkerLoop() {
    for (;;) {
        std::pair<std::wstring, HWND> job;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_queue.empty()) {
                g_worker_running = false;
                return;
            }
            job = std::move(g_queue.back());
            g_queue.pop_back();
        }
        const ProbeResult probe = Probe(job.first);
        const uint64_t now = GetTickCount64();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_cache[job.first] = {probe.map, now, probe.client ? kCacheMs : kUnavailableMs};
            g_pending.erase(job.first);
        }
        if (job.second) PostMessageW(job.second, kSvnStatusReady, 0, 0);
    }
}

} // namespace

bool SvnClientAvailable() {
    static std::atomic<int> cached{-1};
    int value = cached.load(std::memory_order_relaxed);
    if (value < 0) {
        value = ResolveSvnExe().empty() ? 0 : 1;
        cached.store(value, std::memory_order_relaxed);
    }
    return value > 0;
}

SvnStatusMap QuerySvnStatus(const std::wstring& directory, HWND notify) {
    SvnStatusMap result;
    if (directory.empty()) return result;
    const std::wstring key = LowerKey(directory);
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_cache.find(key);
    if (it != g_cache.end() && now - it->second.tick < it->second.ttl) return it->second.map;
    if (it != g_cache.end()) result = it->second.map;
    if (g_pending.insert(key).second) {
        g_queue.emplace_back(key, notify);
        if (!g_worker_running) {
            g_worker_running = true;
            std::thread(WorkerLoop).detach();
        }
    }
    return result;
}

} // namespace luling::fs
