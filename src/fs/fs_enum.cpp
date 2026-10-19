#include "nt_directory_record.h"
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <thread>
#include <chrono>
// fs_enum.cpp
#include "fs_enum.h"
#include "shell_namespace.h"
#include "bounded_enumeration.h"
#include "../common/localization.h"
#include "../common/path_utils.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <propkey.h>
#include <wrl/client.h>
#include <lm.h>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#pragma comment(lib, "shlwapi.lib")

namespace luling::fs {

DWORD ReadReparseTag(const std::wstring& path) {
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileExW(NormalizePath(path).c_str(), FindExInfoBasic,
        &data, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return 0;
    FindClose(find);
    return (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? data.dwReserved0 : 0;
}

using NTSTATUS = LONG;
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_NO_MORE_FILES = static_cast<NTSTATUS>(0x80000006L);

struct NtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct NtObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    NtUnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
};

struct NtIoStatusBlock {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
};

using NtPioApcRoutine = VOID (NTAPI*)(PVOID ApcContext, NtIoStatusBlock* IoStatusBlock, ULONG Reserved);

using luling::fs::NtFileFullDirInformation;

enum NtFileInformationClass : int {
    NtFileFullDirectoryInformation = 2,
};

constexpr ULONG NT_FILE_LIST_DIRECTORY = 0x0001;
constexpr ULONG NT_SYNCHRONIZE = 0x00100000;
constexpr ULONG NT_FILE_OPEN = 1;
constexpr ULONG NT_FILE_DIRECTORY_FILE = 0x00000001;
constexpr ULONG NT_OBJ_CASE_INSENSITIVE = 0x00000040;

#define NtInitializeObjectAttributes(p, n, a, r, s) { \
    (p)->Length = sizeof(NtObjectAttributes); \
    (p)->RootDirectory = r; \
    (p)->Attributes = a; \
    (p)->ObjectName = n; \
    (p)->SecurityDescriptor = s; \
    (p)->SecurityQualityOfService = NULL; \
}

using NtCreateFile_t = NTSTATUS (NTAPI*)(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    NtObjectAttributes* ObjectAttributes,
    NtIoStatusBlock* IoStatusBlock,
    PLARGE_INTEGER AllocationSize,
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    PVOID EaBuffer,
    ULONG EaLength);

using NtQueryDirectoryFile_t = NTSTATUS (NTAPI*)(
    HANDLE FileHandle,
    HANDLE Event,
    NtPioApcRoutine ApcRoutine,
    PVOID ApcContext,
    NtIoStatusBlock* IoStatusBlock,
    PVOID FileInformationBuffer,
    ULONG Length,
    NtFileInformationClass FileInformationClass,
    BOOLEAN ReturnSingleEntry,
    NtUnicodeString* FileName,
    BOOLEAN RestartScan);

static NtCreateFile_t g_NtCreateFile = nullptr;
static NtQueryDirectoryFile_t g_NtQueryDirectoryFile = nullptr;

static void InitNtApi() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) throw std::runtime_error("ntdll.dll not loaded");
    g_NtCreateFile = reinterpret_cast<NtCreateFile_t>(GetProcAddress(ntdll, "NtCreateFile"));
    g_NtQueryDirectoryFile = reinterpret_cast<NtQueryDirectoryFile_t>(GetProcAddress(ntdll, "NtQueryDirectoryFile"));
    if (!g_NtCreateFile || !g_NtQueryDirectoryFile)
        throw std::runtime_error("NtCreateFile / NtQueryDirectoryFile not found");
    });
}

bool IsVirtualPath(const std::wstring& path) {
    return path.starts_with(L"luling:");
}

bool IsUncPath(const std::wstring& path) {
    return path.starts_with(L"\\\\?\\UNC\\") ||
           (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\"));
}

std::wstring NormalizePath(std::wstring path) {
    if (path.empty()) return path;
    if (IsVirtualPath(path)) return path;
    // Replace forward slashes with backslashes.
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\")) {
        // Drive roots must keep a trailing slash: \\?\E: is not a valid directory.
        if (path.size() == 6 && path[5] == L':') path += L'\\';
        return path;
    }
    // Keep the address bar's bare-drive -> root convention, but C:folder
    // still means that drive's working directory, not the active tab's path.
    if (path.size() == 2 && path[1] == L':') path += L'\\';
    // Resolve ALL ordinary DOS/UNC inputs before introducing the extended
    // prefix, which disables Win32 dot-segment and drive-relative handling.
    // Empty is the failure sentinel for a nonempty input; admission/cache
    // callers must distinguish it from an intentional This-PC empty path.
    if (path.find(L'\0') != std::wstring::npos) {
        SetLastError(ERROR_INVALID_NAME);
        return {};
    }
    wchar_t full[32768];
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(_countof(full)), full, nullptr);
    if (n == 0) return {};
    if (n >= _countof(full)) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return {};
    }
    path.assign(full, n);
    // Remove trailing backslash except for drive root.
    if (path.size() > 1 && path.back() == L'\\' && path[path.size() - 2] != L':')
        path.pop_back();
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring ParentPath(const std::wstring& path) {
    if (IsShellPath(path)) return ShellParentPath(path);
    std::wstring normalized = NormalizePath(path);
    if (normalized.empty()) throw std::runtime_error("Invalid directory path");
    std::wstring_view view = normalized;
    // Strip long-path prefix for find_last_of logic.
    std::wstring_view core = view;
    if (core.starts_with(L"\\\\?\\UNC\\")) {
        core.remove_prefix(8);
    } else if (core.starts_with(L"\\\\?\\")) {
        core.remove_prefix(4);
    }
    std::wstring temp(core);
    auto pos = temp.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos == 0) return normalized; // no parent
    temp.resize(pos);
    if (temp.size() == 2 && temp[1] == L':') temp += L'\\'; // drive root
    if (view.starts_with(L"\\\\?\\UNC\\")) return std::wstring(L"\\\\?\\UNC\\") + temp;
    if (view.starts_with(L"\\\\?\\")) return std::wstring(L"\\\\?\\") + temp;
    return temp;
}

std::wstring StripLnkSuffix(const std::wstring& name) {
    if (name.size() < 4) return name;
    if (_wcsicmp(name.c_str() + name.size() - 4, L".lnk") != 0) return name;
    return name.substr(0, name.size() - 4);
}

static void EnumerateFindFirstFileEx(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic<bool>* cancelled = nullptr) {
    std::wstring pattern = path;
    if (!pattern.ends_with(L"\\")) pattern += L"\\";
    pattern += L"*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(
        pattern.c_str(),
        FindExInfoBasic,
        &fd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return;
        std::ostringstream oss;
        oss << "FindFirstFileExW failed, error=" << err;
        throw std::runtime_error(oss.str());
    }
    do {
        if (cancelled && cancelled->load()) { FindClose(h); throw EnumerationCancelled(); }
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        DirEntry e;
        e.name = fd.cFileName;
        e.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        e.mtime = fd.ftLastWriteTime;
        e.ctime = fd.ftCreationTime;
        e.atime = fd.ftLastAccessTime;
        e.attrs = fd.dwFileAttributes;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.is_reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e.reparse_tag = e.is_reparse ? fd.dwReserved0 : 0;
        e.cloud_recall = (fd.dwFileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    const DWORD error = GetLastError();
    FindClose(h);
    if (error != ERROR_NO_MORE_FILES) {
        out.clear();
        throw std::runtime_error("FindNextFileW failed, error=" + std::to_string(error));
    }
}

static void EnumerateNtQuery(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic<bool>* cancelled = nullptr) {
    InitNtApi();

    std::wstring target = path;
    // Convert Win32 long-path prefix to NT object prefix.
    if (target.starts_with(L"\\\\?\\")) {
        target = L"\\??\\" + target.substr(4);
    }
    if (target.ends_with(L"\\")) target.pop_back();

    NtUnicodeString us{};
    us.Buffer = const_cast<PWSTR>(target.c_str());
    us.Length = static_cast<USHORT>(target.size() * sizeof(WCHAR));
    us.MaximumLength = static_cast<USHORT>((target.size() + 1) * sizeof(WCHAR));

    NtObjectAttributes oa{};
    NtInitializeObjectAttributes(&oa, &us, NT_OBJ_CASE_INSENSITIVE, nullptr, nullptr);

    NtIoStatusBlock iosb{};
    HANDLE h;
    NTSTATUS status = g_NtCreateFile(
        &h,
        NT_FILE_LIST_DIRECTORY | NT_SYNCHRONIZE,
        &oa,
        &iosb,
        nullptr,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NT_FILE_OPEN,
        NT_FILE_DIRECTORY_FILE,
        nullptr,
        0);

    if (status != STATUS_SUCCESS) {
        std::ostringstream oss;
        oss << "NtCreateFile failed, status=0x" << std::hex << status;
        throw std::runtime_error(oss.str());
    }

    const auto close_handle = [](void* handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(close_handle)> directory_owner(h, close_handle);
    constexpr SIZE_T kBufSize = 64 * 1024;
    std::vector<BYTE> buffer(kBufSize);
    bool restart = true;
    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent) {
        throw std::runtime_error("CreateEvent failed");
    }

    std::unique_ptr<void, decltype(close_handle)> event_owner(hEvent, close_handle);
    for (;;) {
        if (cancelled && cancelled->load()) {
            throw EnumerationCancelled();
        }
        iosb = {};
        status = g_NtQueryDirectoryFile(
            h,
            hEvent,
            nullptr,
            nullptr,
            &iosb,
            buffer.data(),
            static_cast<ULONG>(kBufSize),
            NtFileFullDirectoryInformation,
            FALSE,
            nullptr,
            restart ? TRUE : FALSE);

        restart = false;

        if (status == STATUS_PENDING) {
            while (WaitForSingleObject(hEvent, 25) == WAIT_TIMEOUT) {
                if (cancelled && cancelled->load()) {
                    CancelIoEx(h, nullptr);
                    // The caller can retire this worker. These stack-owned NT
                    // buffers must nevertheless survive until completion.
                    WaitForSingleObject(hEvent, INFINITE);
                    break;
                }
            }
            status = iosb.Status;
        }

        if (status == STATUS_NO_MORE_FILES) break;
        if (status != STATUS_SUCCESS) {
            std::ostringstream oss;
            oss << "NtQueryDirectoryFile failed, status=0x" << std::hex << status;
            throw std::runtime_error(oss.str());
        }

        if (iosb.Information > buffer.size()) throw std::runtime_error("Invalid NT directory buffer size");
        auto* info = reinterpret_cast<NtFileFullDirInformation*>(buffer.data());
        for (;;) {
            luling::fs::ValidateNtDirectoryRecord(info, static_cast<size_t>(iosb.Information) -
                static_cast<size_t>(reinterpret_cast<BYTE*>(info) - buffer.data()));
            DirEntry e;
            e.name.assign(info->FileName, info->FileNameLength / sizeof(WCHAR));
            if (!(e.name.size() == 1 && e.name[0] == L'.') &&
                !(e.name.size() == 2 && e.name[0] == L'.' && e.name[1] == L'.')) {
                e.size = static_cast<uint64_t>(info->EndOfFile.QuadPart);
                e.mtime.dwLowDateTime = info->LastWriteTime.LowPart;
                e.mtime.dwHighDateTime = info->LastWriteTime.HighPart;
                e.ctime.dwLowDateTime = info->CreationTime.LowPart;
                e.ctime.dwHighDateTime = info->CreationTime.HighPart;
                e.atime.dwLowDateTime = info->LastAccessTime.LowPart;
                e.atime.dwHighDateTime = info->LastAccessTime.HighPart;
                e.attrs = info->FileAttributes;
                e.is_dir = (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                e.is_reparse = (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
                // FileFullDirectoryInformation returns the reparse tag in EaSize.
                e.reparse_tag = e.is_reparse ? info->EaSize : 0;
                e.cloud_recall = (info->FileAttributes & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) != 0;
                out.push_back(std::move(e));
            }

            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<NtFileFullDirInformation*>(
                reinterpret_cast<BYTE*>(info) + info->NextEntryOffset);
        }
    }



}

// "This PC" view (empty path): one entry per logical drive, label matches
// the sidebar ("Label (C:)" or a localized fallback). full_path is set so
// the app layer can navigate/open without joining parent+name.
static void EnumerateThisPc(std::vector<DirEntry>& out) {
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1 << i))) continue;
        wchar_t root[4] = { wchar_t(L'A' + i), L':', L'\\', L'\0' };
        wchar_t volName[MAX_PATH + 1] = {};
        GetVolumeInformationW(root, volName, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        DirEntry e;
        e.name = std::wstring(volName[0] ? volName : l10n::Pick(L"本地磁盘", L"Local Disk")) +
                 L" (" + root[0] + L":)";
        e.full_path = NormalizePath(root);
        e.is_dir = true;
        e.attrs = FILE_ATTRIBUTE_DIRECTORY;
        // Local metadata only (no media access): the UI shows it as the type.
        e.drive_type = static_cast<uint8_t>(GetDriveTypeW(root));
        // Same figures as the sidebar drive rows; the tile view draws them.
        ULARGE_INTEGER free_bytes{}, total_bytes{};
        if (GetDiskFreeSpaceExW(root, &free_bytes, &total_bytes, nullptr)) {
            e.drive_total = total_bytes.QuadPart;
            e.drive_free = free_bytes.QuadPart;
        }
        out.push_back(std::move(e));
    }
    // Phones and cameras live in the Shell namespace, not on a drive letter.
    AppendPortableDevices(out);
}

// Server-root view (\\server, normalized as \\?\UNC\server with no further
// separator): one entry per disk share, enumerated via NetShareEnum. Hidden
// admin shares (name ends in '$') are skipped. full_path is set so the app
// layer can navigate into a share without joining parent+name.
static void EnumerateServerShares(const std::wstring& server, std::vector<DirEntry>& out) {
    std::wstring host = L"\\\\" + server;
    BYTE* buf = nullptr;
    DWORD read = 0, total = 0;
    NET_API_STATUS status = NetShareEnum(
        const_cast<LPWSTR>(host.c_str()), 1, &buf, MAX_PREFERRED_LENGTH,
        &read, &total, nullptr);
    if (status != NERR_Success) {
        std::ostringstream oss;
        oss << "NetShareEnum failed, error=" << status;
        throw std::runtime_error(oss.str());
    }
    const auto* infos = reinterpret_cast<const SHARE_INFO_1*>(buf);
    for (DWORD i = 0; i < read; ++i) {
        const DWORD type = infos[i].shi1_type & ~(STYPE_SPECIAL | STYPE_TEMPORARY);
        if (type != STYPE_DISKTREE) continue;
        std::wstring name = infos[i].shi1_netname;
        if (name.empty() || name.back() == L'$') continue;
        DirEntry e;
        e.name = name;
        e.full_path = NormalizePath(host + L"\\" + name);
        e.is_dir = true;
        e.attrs = FILE_ATTRIBUTE_DIRECTORY;
        out.push_back(std::move(e));
    }
    NetApiBufferFree(buf);
}

static void EnumerateDirectoryImpl(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic<bool>* cancelled) {
    out.clear();
    if (IsShellPath(path)) {
        EnumerateShellDirectory(path, out, cancelled);
        return;
    }
    if (path.empty()) {
        EnumerateThisPc(out);
        return;
    }
    std::wstring normalized = NormalizePath(path);
    if (normalized.empty()) throw std::runtime_error("Invalid directory path");
    if (normalized.starts_with(L"\\\\?\\UNC\\")) {
        std::wstring rest = normalized.substr(8);
        while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
        if (!rest.empty() && rest.find(L'\\') == std::wstring::npos) {
            EnumerateServerShares(rest, out);
            return;
        }
    }
    try {
        EnumerateNtQuery(normalized, out, cancelled);
        return;
    } catch (...) {
        if (cancelled && cancelled->load()) throw;
        // Fall back to FindFirstFileExW.
    }
    out.clear();
    EnumerateFindFirstFileEx(normalized, out, cancelled);
}

void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out, const std::atomic_bool* cancelled) {
    EnumerateDirectoryImpl(path, out, cancelled);
}

namespace {
std::atomic<unsigned> active_directory_requests{0};
std::atomic<unsigned> active_remote_directory_requests{0};
struct DirectoryRequest {
    bool remote = false;
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::vector<DirEntry> entries;
    std::exception_ptr error;
    ~DirectoryRequest() {
        if (remote) --active_remote_directory_requests;
        --active_directory_requests;
    }
};
}
void EnumerateDirectory(const std::wstring& path, std::vector<DirEntry>& out, const EnumerationOptions& options) {
    out.clear();
    if (options.cancelled && options.cancelled()) throw EnumerationCancelled();
    const bool remote = IsUncPath(path);
    if (remote && active_remote_directory_requests.fetch_add(1) >= 16) {
        --active_remote_directory_requests;
        throw std::runtime_error("Remote directory provider retirement limit reached");
    }
    if (active_directory_requests.fetch_add(1) >= 32) {
        --active_directory_requests;
        if (remote) --active_remote_directory_requests;
        throw std::runtime_error("Directory provider retirement limit reached");
    }
    std::shared_ptr<DirectoryRequest> request;
    try { request = std::make_shared<DirectoryRequest>(); request->remote = remote; }
    catch (...) {
        --active_directory_requests;
        if (remote) --active_remote_directory_requests;
        throw;
    }
    std::thread worker([request, path] {
        try { EnumerateDirectoryImpl(path, request->entries, &request->cancelled); }
        catch (...) { request->error = std::current_exception(); }
        { std::lock_guard lock(request->mutex); request->done = true; }
        request->changed.notify_one();
    });
    const auto start = GetTickCount64();
    try {
        std::unique_lock lock(request->mutex);
        for (;;) {
            const bool cancelled = options.cancelled && options.cancelled();
            if (cancelled || GetTickCount64() - start >= options.timeout_ms) {
                request->cancelled = true;
                CancelSynchronousIo(worker.native_handle());
                worker.detach();
                throw std::runtime_error(cancelled ? "Directory enumeration cancelled" : "Directory enumeration timed out");
            }
            if (request->done) break;
            request->changed.wait_for(lock, std::chrono::milliseconds(25));
        }
        lock.unlock();
        worker.join();
    } catch (...) {
        if (worker.joinable()) {
            request->cancelled = true;
            CancelSynchronousIo(worker.native_handle());
            worker.detach();
        }
        throw;
    }
    if (request->error) std::rethrow_exception(request->error);
    out = std::move(request->entries);
}

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::wstring_view kShellPathPrefix = L"luling:shell:";

void LogDeviceCopy(const std::wstring& text) {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return;
    const std::wstring path = std::wstring(temp) + L"luling-device-copy.log";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") != 0 || !file) return;
    fwprintf(file, L"%s\n", text.c_str());
    fclose(file);
}

struct ShellComInit {
    HRESULT hr = E_FAIL;
    ShellComInit() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ShellComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
    // Another apartment is fine: the Shell calls still work, and this thread
    // must not tear down an initialization it did not perform.
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

bool DecodeShellHex(std::wstring_view hex, std::vector<BYTE>& bytes) {
    auto digit = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    if (hex.empty() || (hex.size() & 1u) != 0) return false;
    bytes.resize(hex.size() / 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const int high = digit(hex[i * 2]);
        const int low = digit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        bytes[i] = static_cast<BYTE>((high << 4) | low);
    }
    return true;
}

bool ParseShellPidl(const std::wstring& path, PIDLIST_ABSOLUTE& pidl) {
    pidl = nullptr;
    if (!IsShellPath(path)) return false;
    std::vector<BYTE> bytes;
    if (!DecodeShellHex(std::wstring_view(path).substr(kShellPathPrefix.size()), bytes))
        return false;
    if (bytes.size() < sizeof(USHORT) || bytes[bytes.size() - 1] != 0 ||
        bytes[bytes.size() - 2] != 0)
        return false;
    void* memory = CoTaskMemAlloc(bytes.size());
    if (!memory) return false;
    std::memcpy(memory, bytes.data(), bytes.size());
    pidl = reinterpret_cast<PIDLIST_ABSOLUTE>(memory);
    return true;
}

ComPtr<IShellItem> ShellItemFromPidl(PCIDLIST_ABSOLUTE pidl) {
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item)))) return {};
    return item;
}

std::wstring ShellNormalName(IShellItem* item) {
    if (!item) return {};
    PWSTR text = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &text)) || !text) return {};
    std::wstring name(text);
    CoTaskMemFree(text);
    return name;
}

void FillShellProperties(IShellItem2* item, DirEntry& entry) {
    if (!item) return;
    PROPVARIANT size{};
    PropVariantInit(&size);
    if (SUCCEEDED(item->GetProperty(PKEY_Size, &size))) {
        if (size.vt == VT_UI8) entry.size = size.uhVal.QuadPart;
        else if (size.vt == VT_UI4) entry.size = size.ulVal;
    }
    PropVariantClear(&size);
    FILETIME modified{};
    if (SUCCEEDED(item->GetFileTime(PKEY_DateModified, &modified))) entry.mtime = modified;
    FILETIME created{};
    if (SUCCEEDED(item->GetFileTime(PKEY_DateCreated, &created))) entry.ctime = created;
}

bool IsPortableDevice(IShellItem* item) {    if (!item) return false;
    // Explorer's own test: a portable device's parsing name carries the
    // Portable Devices category GUID. Some handlers leave the category
    // property empty, so the path is the dependable signal.
    PWSTR parsing = nullptr;
    bool portable = false;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing)) && parsing) {
        std::wstring text(parsing);
        CoTaskMemFree(parsing);
        CharLowerW(text.data());
        portable = text.find(L"{6ac27878-a6fa-4155-ba85-f98f491d4f33}") != std::wstring::npos;
    }
    if (portable) return true;
    ComPtr<IShellItem2> item2;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(&item2))) || !item2) return false;
    PROPVARIANT categories{};
    PropVariantInit(&categories);
    if (SUCCEEDED(item2->GetProperty(PKEY_Devices_CategoryIds, &categories))) {
        if (categories.vt == (VT_VECTOR | VT_LPWSTR)) {
            const auto& list = categories.calpwstr;
            for (ULONG i = 0; i < list.cElems && !portable; ++i) {
                portable = list.pElems[i] &&
                    _wcsicmp(list.pElems[i], L"{6AC27878-A6FA-4155-BA85-F98F491D4F33}") == 0;
            }
        } else if (categories.vt == VT_LPWSTR) {
            portable = categories.pwszVal &&
                _wcsicmp(categories.pwszVal, L"{6AC27878-A6FA-4155-BA85-F98F491D4F33}") == 0;
        }
    }
    PropVariantClear(&categories);
    return portable;
}

bool RunShellFileOperation(const std::vector<ComPtr<IShellItem>>& sources,
                           IShellItem* destination, bool move) {    if (!destination || sources.empty()) return false;
    ComPtr<IFileOperation> operation;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&operation))))
        return false;
    if (FAILED(operation->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOERRORUI |
                                            FOF_ALLOWUNDO | FOF_RENAMEONCOLLISION))) {
        LogDeviceCopy(L"op: SetOperationFlags failed");
        return false;
    }
    for (const auto& source : sources) {
        if (!source) continue;
        const HRESULT queued = move
            ? operation->MoveItem(source.Get(), destination, nullptr, nullptr)
            : operation->CopyItem(source.Get(), destination, nullptr, nullptr);
        wchar_t line[64]{};
        swprintf_s(line, L"op: queue hr=0x%08X", static_cast<unsigned>(queued));
        LogDeviceCopy(line);
        if (FAILED(queued)) return false;
    }
    const HRESULT performed = operation->PerformOperations();
    {
        wchar_t line[64]{};
        swprintf_s(line, L"op: perform hr=0x%08X", static_cast<unsigned>(performed));
        LogDeviceCopy(line);
    }
    if (FAILED(performed)) return false;
    BOOL aborted = FALSE;
    operation->GetAnyOperationsAborted(&aborted);
    LogDeviceCopy(std::wstring(L"op: aborted=") + (aborted ? L"1" : L"0"));
    return aborted == FALSE;
}

std::mutex g_portable_mutex;
std::vector<DirEntry> g_portable_devices;
ULONGLONG g_portable_devices_at = 0;
// Display names are asked for on every title/breadcrumb build; resolving the
// PIDL each time would put a Shell call on the paint path.
std::mutex g_shell_name_mutex;
std::unordered_map<std::wstring, std::wstring> g_shell_names;

} // namespace

bool IsShellPath(const std::wstring& path) {
    return path.starts_with(kShellPathPrefix);
}

std::wstring MakeShellPath(PCIDLIST_ABSOLUTE pidl) {
    if (!pidl) return {};
    const UINT bytes = ILGetSize(pidl);
    if (bytes < sizeof(USHORT)) return {};
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out(kShellPathPrefix);
    out.reserve(out.size() + static_cast<size_t>(bytes) * 2);
    const auto* data = reinterpret_cast<const BYTE*>(pidl);
    for (UINT i = 0; i < bytes; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

void EnumerateShellDirectory(const std::wstring& path, std::vector<DirEntry>& out,
                             const std::atomic<bool>* cancelled) {
    out.clear();
    ShellComInit com;
    if (!com.ok()) throw std::runtime_error("Shell COM initialization failed");
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!ParseShellPidl(path, pidl)) throw std::runtime_error("Invalid shell path");
    ComPtr<IShellFolder> folder;
    HRESULT hr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder));
    if (FAILED(hr) || !folder) {
        CoTaskMemFree(pidl);
        throw std::runtime_error("Could not open the portable device");
    }
    ComPtr<IEnumIDList> items;
    hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &items);
    if (FAILED(hr) || !items) {
        CoTaskMemFree(pidl);
        throw std::runtime_error("Could not enumerate the portable device");
    }
    LPITEMIDLIST child = nullptr;
    while (items->Next(1, &child, nullptr) == S_OK) {
        if (cancelled && cancelled->load()) {
            CoTaskMemFree(child);
            break;
        }
        SFGAOF attributes = SFGAO_FOLDER | SFGAO_STREAM;
        PCUITEMID_CHILD child_ref = child;
        folder->GetAttributesOf(1, &child_ref, &attributes);
        PIDLIST_ABSOLUTE absolute = ILCombine(pidl, child);
        ComPtr<IShellItem2> item;
        if (absolute) {
            ComPtr<IShellItem> plain;
            SHCreateItemFromIDList(absolute, IID_PPV_ARGS(&plain));
            if (plain) plain.As(&item);
        }
        DirEntry entry;
        entry.name = ShellNormalName(item.Get());
        if (entry.name.empty()) {
            STRRET text{};
            if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_NORMAL, &text))) {
                wchar_t buffer[512]{};
                if (SUCCEEDED(StrRetToBufW(&text, child, buffer, ARRAYSIZE(buffer))))
                    entry.name.assign(buffer);
            }
        }
        if (entry.name.empty() || entry.name == L"." || entry.name == L"..") {
            if (absolute) CoTaskMemFree(absolute);
            CoTaskMemFree(child);
            continue;
        }
        entry.is_dir = (attributes & SFGAO_FOLDER) != 0;
        entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        if (absolute) entry.full_path = MakeShellPath(absolute);
        FillShellProperties(item.Get(), entry);
        if (absolute) CoTaskMemFree(absolute);
        CoTaskMemFree(child);
        out.push_back(std::move(entry));
    }
    CoTaskMemFree(pidl);
}

void AppendPortableDevices(std::vector<DirEntry>& out) {
    std::vector<DirEntry> devices;
    ShellComInit com;
    if (!com.ok()) return;
    PIDLIST_ABSOLUTE computer = nullptr;
    if (FAILED(SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &computer)) || !computer)
        return;
    ComPtr<IShellFolder> folder;
    if (FAILED(SHBindToObject(nullptr, computer, nullptr, IID_PPV_ARGS(&folder))) || !folder) {
        CoTaskMemFree(computer);
        return;
    }
    ComPtr<IEnumIDList> items;
    if (FAILED(folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &items)) ||
        !items) {
        CoTaskMemFree(computer);
        return;
    }
    LPITEMIDLIST child = nullptr;
    while (items->Next(1, &child, nullptr) == S_OK) {
        SFGAOF attributes = SFGAO_FILESYSTEM | SFGAO_FOLDER | SFGAO_STORAGE |
                            SFGAO_FILESYSANCESTOR;
        PCUITEMID_CHILD child_ref = child;
        folder->GetAttributesOf(1, &child_ref, &attributes);
        const bool filesystem = (attributes & SFGAO_FILESYSTEM) != 0;
        if (filesystem) {
            CoTaskMemFree(child);
            continue;
        }
        PIDLIST_ABSOLUTE absolute = ILCombine(computer, child);
        if (!absolute) {
            CoTaskMemFree(child);
            continue;
        }
        ComPtr<IShellItem> item = ShellItemFromPidl(absolute);
        if (!IsPortableDevice(item.Get())) {
            CoTaskMemFree(absolute);
            CoTaskMemFree(child);
            continue;
        }
        DirEntry entry;
        entry.name = ShellNormalName(item.Get());
        entry.is_dir = true;
        entry.attrs = FILE_ATTRIBUTE_DIRECTORY;
        entry.full_path = MakeShellPath(absolute);
        CoTaskMemFree(absolute);
        CoTaskMemFree(child);
        if (!entry.name.empty() && !entry.full_path.empty())
            devices.push_back(std::move(entry));
    }
    CoTaskMemFree(computer);
    {
        std::lock_guard<std::mutex> lock(g_portable_mutex);
        g_portable_devices = devices;
        g_portable_devices_at = GetTickCount64();
    }
    out.insert(out.end(), devices.begin(), devices.end());
}

std::vector<DirEntry> PortableDevicesSnapshot() {
    {
        std::lock_guard<std::mutex> lock(g_portable_mutex);
        if (g_portable_devices_at && GetTickCount64() - g_portable_devices_at < 30000)
            return g_portable_devices;
    }
    std::vector<DirEntry> devices;
    AppendPortableDevices(devices);
    return devices;
}

std::wstring ShellParentPath(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!ParseShellPidl(path, pidl)) return {};
    PIDLIST_ABSOLUTE parent = ILClone(pidl);
    CoTaskMemFree(pidl);
    if (!parent) return {};
    if (!ILRemoveLastID(parent)) {
        CoTaskMemFree(parent);
        return {};
    }
    if (ILIsEmpty(parent)) {
        CoTaskMemFree(parent);
        return {};
    }
    PIDLIST_ABSOLUTE computer = nullptr;
    SHGetKnownFolderIDList(FOLDERID_ComputerFolder, 0, nullptr, &computer);
    const bool is_this_pc = computer && ILIsEqual(parent, computer);
    if (computer) CoTaskMemFree(computer);
    std::wstring result;
    if (!is_this_pc) result = MakeShellPath(parent);
    CoTaskMemFree(parent);
    return result;
}

std::wstring ShellDisplayName(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(g_shell_name_mutex);
        const auto found = g_shell_names.find(path);
        if (found != g_shell_names.end()) return found->second;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!ParseShellPidl(path, pidl)) return {};
    ComPtr<IShellItem> item = ShellItemFromPidl(pidl);
    CoTaskMemFree(pidl);
    std::wstring name = ShellNormalName(item.Get());
    if (!name.empty()) {
        std::lock_guard<std::mutex> lock(g_shell_name_mutex);
        if (g_shell_names.size() >= 512) g_shell_names.clear();
        g_shell_names.emplace(path, name);
    }
    return name;
}

bool ShellOpen(const std::wstring& path) {
    ShellComInit com;
    if (!com.ok()) return false;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!ParseShellPidl(path, pidl)) return false;
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_IDLIST;
    execute.lpVerb = L"open";
    execute.lpIDList = pidl;
    execute.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW(&execute) != FALSE;
    CoTaskMemFree(pidl);
    return ok;
}

bool ShellCopyToLocal(const std::vector<std::wstring>& shell_sources,
                      const std::wstring& local_dir, bool move) {
    ShellComInit com;
    if (!com.ok()) return false;
    ComPtr<IShellItem> destination;
    const std::wstring plain_destination = luling::path::StripExtendedPathPrefix(local_dir);
    if (FAILED(SHCreateItemFromParsingName(plain_destination.c_str(), nullptr,
                                           IID_PPV_ARGS(&destination))))
        return false;
    std::vector<ComPtr<IShellItem>> sources;
    for (const auto& path : shell_sources) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (!ParseShellPidl(path, pidl)) continue;
        ComPtr<IShellItem> item = ShellItemFromPidl(pidl);
        CoTaskMemFree(pidl);
        if (item) sources.push_back(std::move(item));
    }
    return RunShellFileOperation(sources, destination.Get(), move);
}

bool ShellCopyFromLocal(const std::vector<std::wstring>& local_sources,
                        const std::wstring& shell_dest, bool move) {
    ShellComInit com;
    if (!com.ok()) return false;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!ParseShellPidl(shell_dest, pidl)) return false;
    ComPtr<IShellItem> destination = ShellItemFromPidl(pidl);
    CoTaskMemFree(pidl);
    if (!destination) return false;
    std::vector<ComPtr<IShellItem>> sources;
    for (const auto& path : local_sources) {
        ComPtr<IShellItem> item;
        const std::wstring plain = luling::path::StripExtendedPathPrefix(path);
        if (SUCCEEDED(SHCreateItemFromParsingName(plain.c_str(), nullptr,
                                                  IID_PPV_ARGS(&item))))
            sources.push_back(std::move(item));
    }
    return RunShellFileOperation(sources, destination.Get(), move);
}

static bool DeleteShellItemsOnce(const std::vector<std::wstring>& paths) {
    ShellComInit com;
    if (!com.ok()) return false;
    ComPtr<IFileOperation> operation;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&operation))))
        return false;
    // A device has no Recycle Bin, so FOF_ALLOWUNDO makes the delete fail.
    if (FAILED(operation->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOERRORUI)))
        return false;
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (!ParseShellPidl(path, pidl)) continue;
        ComPtr<IShellItem> item = ShellItemFromPidl(pidl);
        CoTaskMemFree(pidl);
        if (item) {
            const HRESULT queued = operation->DeleteItem(item.Get(), nullptr);
            wchar_t line[64]{};
            swprintf_s(line, L"delete: queue hr=0x%08X", static_cast<unsigned>(queued));
            LogDeviceCopy(line);
        }
    }
    const HRESULT performed = operation->PerformOperations();
    {
        wchar_t line[64]{};
        swprintf_s(line, L"delete: perform hr=0x%08X", static_cast<unsigned>(performed));
        LogDeviceCopy(line);
    }
    if (FAILED(performed)) return false;
    BOOL aborted = FALSE;
    operation->GetAnyOperationsAborted(&aborted);
    LogDeviceCopy(std::wstring(L"delete: aborted=") + (aborted ? L"1" : L"0"));
    return aborted == FALSE;
}

static bool DeleteShellItems(const std::vector<std::wstring>& paths) {
    // The device can still be finishing the upload that just landed.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (DeleteShellItemsOnce(paths)) return true;
        if (attempt == 0) Sleep(300);
    }
    return false;
}

static std::wstring MakeStagingDirectory() {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return {};
    std::wstring directory = std::wstring(temp) + L"luling-device-copy-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return {};
    return directory;
}

static std::vector<std::wstring> ListStagingItems(const std::wstring& directory) {
    std::vector<std::wstring> items;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = directory + L"\\*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return items;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
            continue;
        items.push_back(directory + L"\\" + data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return items;
}

bool ShellRename(const std::wstring& path, const std::wstring& new_name) {
    ShellComInit com;
    if (!com.ok() || new_name.empty()) return false;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (!ParseShellPidl(path, pidl)) return false;
    ComPtr<IShellItem> item = ShellItemFromPidl(pidl);
    CoTaskMemFree(pidl);
    if (!item) return false;
    ComPtr<IFileOperation> operation;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&operation))))
        return false;
    if (FAILED(operation->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOERRORUI |
                                            FOF_ALLOWUNDO))) {
        LogDeviceCopy(L"rename: SetOperationFlags failed");
        return false;
    }
    const HRESULT queued = operation->RenameItem(item.Get(), new_name.c_str(), nullptr);
    {
        wchar_t line[64]{};
        swprintf_s(line, L"rename: queue hr=0x%08X", static_cast<unsigned>(queued));
        LogDeviceCopy(line);
    }
    if (FAILED(queued)) return false;
    const HRESULT performed = operation->PerformOperations();
    {
        wchar_t line[64]{};
        swprintf_s(line, L"rename: perform hr=0x%08X", static_cast<unsigned>(performed));
        LogDeviceCopy(line);
    }
    if (FAILED(performed)) return false;
    BOOL aborted = FALSE;
    operation->GetAnyOperationsAborted(&aborted);
    LogDeviceCopy(std::wstring(L"rename: aborted=") + (aborted ? L"1" : L"0"));
    return aborted == FALSE;
}

bool ShellDelete(const std::vector<std::wstring>& paths) {
    return DeleteShellItems(paths);
}

bool ShellCopyWithin(const std::vector<std::wstring>& shell_sources,
                     const std::wstring& shell_dest, bool move) {
    if (shell_sources.empty()) return false;
    LogDeviceCopy(L"within: start sources=" + std::to_wstring(shell_sources.size()));
    // MTP rejects device-to-device copies, and that failed first attempt can
    // leave the device busy for the next operation. Stage through a local
    // folder instead: download, upload, then drop the local copies.
    const std::wstring staging = MakeStagingDirectory();
    if (staging.empty()) {
        LogDeviceCopy(L"within: staging directory failed");
        return false;
    }
    bool ok = ShellCopyToLocal(shell_sources, staging, false);
    if (!ok) {
        // A device that just finished another operation can refuse the next
        // one; one short retry recovers without disturbing the user.
        Sleep(300);
        ok = ShellCopyToLocal(shell_sources, staging, false);
    }
    LogDeviceCopy(L"within: download " + std::wstring(ok ? L"ok" : L"failed"));
    std::vector<std::wstring> items;
    if (ok) items = ListStagingItems(staging);
    LogDeviceCopy(L"within: staged items=" + std::to_wstring(items.size()));
    ok = ok && !items.empty() && ShellCopyFromLocal(items, shell_dest, false);
    LogDeviceCopy(L"within: upload " + std::wstring(ok ? L"ok" : L"failed"));
    if (ok && move) {
        const bool deleted = DeleteShellItems(shell_sources);
        LogDeviceCopy(L"within: delete source " + std::wstring(deleted ? L"ok" : L"failed"));
    }
    std::error_code ignored;
    std::filesystem::remove_all(staging, ignored);
    return ok;
}

} // namespace luling::fs
