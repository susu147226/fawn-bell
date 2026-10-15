#include "network_discovery.h"

#include <winnetwk.h>

#include <string>
#include <vector>

namespace luling::app {
namespace {

// A single WNetEnumResource call cannot be interrupted, so these bound the walk
// between calls: without them a large or half-dead network keeps an IO worker
// busy long after the list has stopped mattering.
constexpr DWORD kDiscoveryBudgetMs = 4000;
constexpr size_t kMaximumComputers = 512;
constexpr int kMaximumDepth = 3;

struct EnumScope {
    HANDLE handle = nullptr;
    EnumScope() = default;
    explicit EnumScope(HANDLE value) : handle(value) {}
    EnumScope(const EnumScope&) = delete;
    EnumScope& operator=(const EnumScope&) = delete;
    ~EnumScope() { if (handle) WNetCloseEnum(handle); }
};

// One enumerated entry, with its strings copied out: the NETRESOURCE the API
// hands back points into the caller's buffer.
struct Discovered {
    std::wstring remote;
    bool container = false;
};

std::wstring TailName(const std::wstring& remote) {
    std::wstring name = remote;
    while (name.size() > 2 && name.back() == L'\\') name.pop_back();
    const auto slash = name.find_last_of(L'\\');
    return slash != std::wstring::npos && slash + 1 < name.size() ? name.substr(slash + 1) : name;
}

} // namespace

std::vector<NetworkLocation> EnumerateNetworkComputers(const std::function<bool()>& cancel) {
    std::vector<NetworkLocation> found;
    const ULONGLONG deadline = GetTickCount64() + kDiscoveryBudgetMs;
    auto spent = [&] {
        return found.size() >= kMaximumComputers || GetTickCount64() >= deadline ||
            (cancel && cancel());
    };

    std::function<void(const std::wstring&, int)> walk = [&](const std::wstring& container, int depth) {
        if (depth > kMaximumDepth || spent()) return;
        NETRESOURCEW scope{};
        scope.dwScope = RESOURCE_GLOBALNET;
        scope.dwType = RESOURCETYPE_DISK;
        scope.dwUsage = RESOURCEUSAGE_CONTAINER;
        if (!container.empty()) scope.lpRemoteName = const_cast<LPWSTR>(container.c_str());
        HANDLE raw = nullptr;
        // A null resource is the root of the network; a remote name is one of its
        // containers. Anything else is "not available", never a failure.
        if (WNetOpenEnumW(RESOURCE_GLOBALNET, RESOURCETYPE_DISK, RESOURCEUSAGE_CONTAINER,
                          container.empty() ? nullptr : &scope, &raw) != NO_ERROR ||
            !raw) return;
        EnumScope enumeration(raw);

        std::vector<Discovered> children;
        std::vector<BYTE> buffer(16 * 1024);
        for (;;) {
            if (spent()) return;
            DWORD count = 0xFFFFFFFF;
            DWORD size = static_cast<DWORD>(buffer.size());
            const DWORD status = WNetEnumResourceW(enumeration.handle, &count, buffer.data(), &size);
            if (status == ERROR_MORE_DATA) {
                buffer.resize(size);
                continue;
            }
            if (status != NO_ERROR) break; // ERROR_NO_MORE_ITEMS ends an ordinary walk
            const auto* entries = reinterpret_cast<const NETRESOURCEW*>(buffer.data());
            for (DWORD i = 0; i < count; ++i) {
                if (!entries[i].lpRemoteName || !entries[i].lpRemoteName[0]) continue;
                children.push_back({entries[i].lpRemoteName,
                    (entries[i].dwUsage & RESOURCEUSAGE_CONTAINER) != 0});
            }
        }

        for (const auto& child : children) {
            if (spent()) return;
            // \\NAME is a machine; a bare name is a workgroup, a domain or the
            // provider's own wrapper, and the machines live one level inside it.
            if (child.remote.starts_with(L"\\\\")) {
                if (child.container) found.push_back({TailName(child.remote), child.remote, false});
                continue;
            }
            if (child.container) walk(child.remote, depth + 1);
        }
    };

    walk({}, 1);
    return found;
}

} // namespace luling::app
