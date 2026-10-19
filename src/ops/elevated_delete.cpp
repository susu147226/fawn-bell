#include "elevated_delete.h"
#include "../common/path_utils.h"
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <sherrors.h>
#include <wrl/client.h>
#include <algorithm>
#include <new>

namespace luling::ops {
namespace {
using Microsoft::WRL::ComPtr;
constexpr HRESULT kCancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);
std::wstring ItemPath(IShellItem* item) {
    PWSTR raw = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) return {};
    std::wstring path;
    try { if (raw) path = raw; }
    catch (...) { CoTaskMemFree(raw); throw; }
    CoTaskMemFree(raw); return path;
}
bool Missing(const std::wstring& item) {
    if (GetFileAttributesW(item.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}
std::wstring ErrorText(HRESULT hr) {
    PWSTR text = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring message;
    if (count && text) message.assign(text, count);
    if (text) LocalFree(text);
    // No system text (e.g. COPYENGINE_E_*): leave it empty so the UI side can
    // describe the HRESULT in the user's language.
    return message;
}
// RecycleItem has a recycle-only contract. Unlike a generic DeleteItem request,
// failure is never interpreted here as permission to invoke permanent deletion.
class RecycleSink final : public ITransferAdviseSink {
public:
    RecycleSink(std::atomic<bool>& cancel, const ShellTransferProgress& progress)
        : cancel_(cancel), progress_(progress) {}
    HRESULT failure = S_OK;
    // Which file the shell is reporting on right now. One sink serves the whole
    // batch, so this moves with the run instead of being fixed at construction —
    // a fresh sink per file was part of what made large deletes crawl.
    std::wstring current;
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ITransferAdviseSink) return E_NOINTERFACE;
        *out = static_cast<ITransferAdviseSink*>(this); AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override { const ULONG refs = --refs_; if (!refs) delete this; return refs; }
    IFACEMETHODIMP UpdateProgress(ULONGLONG done, ULONGLONG total, int, int, int, int) override {
        try { if (progress_) progress_(current, total ? static_cast<float>(100.0 * static_cast<double>(done) / static_cast<double>(total)) : -1.0f); }
        catch (...) { return Fail(E_FAIL); }
        return cancel_.load() ? kCancelled : S_OK;
    }
    IFACEMETHODIMP UpdateTransferState(TRANSFER_ADVISE_STATE) override { return cancel_.load() ? kCancelled : S_OK; }
    IFACEMETHODIMP ConfirmOverwrite(IShellItem*, IShellItem*, LPCWSTR) override { return Fail(E_ACCESSDENIED); }
    IFACEMETHODIMP ConfirmEncryptionLoss(IShellItem*) override { return Fail(E_ACCESSDENIED); }
    IFACEMETHODIMP FileFailure(IShellItem*, LPCWSTR, HRESULT hr, LPWSTR, ULONG) override { return Fail(hr); }
    IFACEMETHODIMP SubStreamFailure(IShellItem*, LPCWSTR, HRESULT hr) override { return Fail(hr); }
    IFACEMETHODIMP PropertyFailure(IShellItem*, const PROPERTYKEY*, HRESULT hr) override { return Fail(hr); }
private:
    HRESULT Fail(HRESULT hr) { if (SUCCEEDED(hr)) hr = E_FAIL; if (SUCCEEDED(failure)) failure = hr; return hr; }
    std::atomic<ULONG> refs_{1};
    std::atomic<bool>& cancel_;
    const ShellTransferProgress& progress_;
};
// `transfer` and `bin` are supplied by the caller, which builds them once per
// batch (and per parent folder) rather than once per file — see the delete loop.
HRESULT RecycleOnly(const std::wstring& source, ITransferSource* transfer, IShellItem* bin,
                    RecycleSink& sink, std::atomic<bool>& cancel,
                    ShellTransferResult& result, const ShellTransferProgress& progress) {
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(path::StripExtendedPathPrefix(source).c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) return hr;
    sink.current = source;
    sink.failure = S_OK;
    ComPtr<IShellItem> recycled;
    bool attempted = false;
    if (cancel.load()) hr = kCancelled;
    else {
        if (progress) progress(source, -1.0f);
        if (cancel.load()) hr = kCancelled;
        else {
            attempted = true;
            hr = transfer->RecycleItem(item.Get(), bin, TSF_NORMAL, &recycled);
        }
    }
    const DWORD attributes = GetFileAttributesW(source.c_str());
    const DWORD source_error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    const bool missing = attributes == INVALID_FILE_ATTRIBUTES &&
        (source_error == ERROR_FILE_NOT_FOUND || source_error == ERROR_PATH_NOT_FOUND);
    // A returned recycled object proves a change even if the original path was
    // recreated concurrently. Unknown source state after the call cannot be
    // treated as proof of no mutation and must not enable replay.
    if (recycled || (attempted && attributes == INVALID_FILE_ATTRIBUTES)) result.mutated = true;
    if ((hr == S_OK || hr == COPYENGINE_S_DONT_PROCESS_CHILDREN) && recycled && missing) {
        result.sources.push_back(source);
        return FAILED(sink.failure) ? sink.failure : S_OK;
    }
    if (FAILED(hr)) return hr;
    if (FAILED(sink.failure)) return sink.failure;
    // Skipped/pending results and a missing output object are not completion.
    return E_FAIL;
}
class DeleteSink final : public IFileOperationProgressSink {
public:
    DeleteSink(const std::wstring& original, std::atomic<bool>& cancel,
               ShellTransferResult& result, const ShellTransferProgress& progress)
        : original_(original), cancel_(cancel), result_(result), progress_(progress) {
        const DWORD attributes = GetFileAttributesW(original.c_str());
        directory_ = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    bool DeferredDirectory() const { return deferred_directory_; }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileOperationProgressSink) return E_NOINTERFACE;
        *out = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    IFACEMETHODIMP_(ULONG) Release() override { const ULONG refs = --refs_; if (!refs) delete this; return refs; }
    HRESULT Cancel() const { return cancel_.load() ? kCancelled : S_OK; }
    IFACEMETHODIMP StartOperations() override { return Cancel(); }
    IFACEMETHODIMP FinishOperations(HRESULT hr) override { NoteFailure(hr); return S_OK; }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem* item) override {
        if (FAILED(Cancel())) return kCancelled;
        try {
            current_ = ItemPath(item);
            if (path::EqualInsensitive(path::StripExtendedPathPrefix(original_), current_))
                item->QueryInterface(IID_PPV_ARGS(&root_identity_));
            if (progress_) progress_(current_, -1.0f);
        } catch (...) { NoteFailure(E_OUTOFMEMORY); return E_OUTOFMEMORY; }
        return Cancel();
    }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* item, HRESULT hr, IShellItem*) override {
        NoteFailure(hr);
        try {
            const auto from = ItemPath(item);
            ComPtr<IUnknown> identity;
            if (item) item->QueryInterface(IID_PPV_ARGS(&identity));
            const bool root = path::EqualInsensitive(path::StripExtendedPathPrefix(original_), from) ||
                (root_identity_ && identity.Get() == root_identity_.Get());
            // The filesystem provider reports NOT_HANDLED for a directory
            // before deleting its children. It is not a completion callback;
            // only the final successful operation plus absence can confirm it.
            if (directory_ && root && hr == COPYENGINE_S_NOT_HANDLED)
                deferred_directory_ = true;
            // Ignore skip/pending codes; only actual delete completion counts.
            const bool completed_status = hr == S_OK || hr == COPYENGINE_S_DONT_PROCESS_CHILDREN;
            if (completed_status) {
                result_.mutated = true;
                if (root && Missing(original_)) {
                    if (result_.sources.empty() || result_.sources.back() != original_)
                        result_.sources.push_back(original_);
                }
            }
        } catch (...) { NoteFailure(E_OUTOFMEMORY); return E_OUTOFMEMORY; }
        return Cancel();
    }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return E_ACCESSDENIED; }
    IFACEMETHODIMP UpdateProgress(UINT total, UINT done) override {
        try { if (progress_) progress_(current_, total ? static_cast<float>(100.0 * done / total) : -1.0f); }
        catch (...) { NoteFailure(E_FAIL); return E_FAIL; }
        return Cancel();
    }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
private:
    void NoteFailure(HRESULT hr) { if (FAILED(hr) && SUCCEEDED(result_.hr)) result_.hr = hr; }
    std::atomic<ULONG> refs_{1};
    const std::wstring& original_;
    bool directory_ = false;
    bool deferred_directory_ = false;
    std::atomic<bool>& cancel_;
    ShellTransferResult& result_;
    const ShellTransferProgress& progress_;
    std::wstring current_;
    ComPtr<IUnknown> root_identity_;
};
}
ShellTransferResult DeleteWithCurrentToken(const std::vector<std::wstring>& sources,
    bool permanent, HWND owner, std::atomic<bool>& cancel, ShellTransferProgress progress) {
    ShellTransferResult result;
    // Built once for the whole batch. Rebuilding the Recycle Bin item, the transfer
    // source and the advise sink for every file is what made deleting a large
    // selection crawl — each of those spins up shell machinery that serves any
    // number of items. A transfer source belongs to one parent folder, so it is
    // kept and reused until the batch moves elsewhere; selections are usually a
    // single folder anyway, and a mixed one still pays only once per folder.
    ComPtr<IShellItem> recycle_bin;
    ComPtr<RecycleSink> recycle_sink;
    ComPtr<ITransferSource> cached_transfer;
    std::wstring cached_parent;
    DWORD cached_cookie = 0;
    // Files the strict recycle path refused; retried through the shell's own delete
    // once the loop is done (see below).
    std::vector<std::wstring> deferred;
    if (!permanent) {
        result.hr = SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr,
                                         IID_PPV_ARGS(&recycle_bin));
        if (FAILED(result.hr)) { result.error = ErrorText(result.hr); return result; }
        recycle_sink.Attach(new (std::nothrow) RecycleSink(cancel, progress));
        if (!recycle_sink) { result.hr = E_OUTOFMEMORY; return result; }
    }
    // One IFileOperation serves the whole batch — it holds any number of items, and a
    // fresh one per file paid COM startup plus shell-engine spin-up for every single
    // file. Each file still gets its own sink (a sink tracks which item is the root
    // of its own subtree and when that subtree is gone), so the per-file bookkeeping
    // is unchanged; the work just happens in one PerformOperations at the end.
    struct PendingDelete { ComPtr<DeleteSink> sink; std::wstring source; DWORD cookie = 0; };
    ComPtr<IFileOperation> batch;
    std::vector<PendingDelete> pending;
    if (permanent) {
        result.hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&batch));
        if (FAILED(result.hr)) { result.error = ErrorText(result.hr); return result; }
        result.hr = batch->SetOwnerWindow(owner);
        if (SUCCEEDED(result.hr))
            result.hr = batch->SetOperationFlags(
                FOF_NO_UI | FOF_NO_CONNECTED_ELEMENTS | FOFX_EARLYFAILURE | FOFX_NOCOPYHOOKS);
        if (FAILED(result.hr)) { result.error = ErrorText(result.hr); return result; }
    }
    for (const auto& source : sources) {
        if (cancel.load()) { result.hr = kCancelled; result.cancelled = true; break; }
        if (!permanent) {
            const auto plain = path::StripExtendedPathPrefix(source);
            wchar_t volume[32768]{};
            if (plain.starts_with(L"\\\\")) {
                result.hr = COPYENGINE_E_RECYCLE_BIN_NOT_FOUND;
                result.error = L"This location cannot be safely sent to the Recycle Bin. The item was not deleted.";
                break;
            }
            if (!GetVolumePathNameW(source.c_str(), volume, 32768)) {
                result.hr = HRESULT_FROM_WIN32(GetLastError());
                break;
            }
            if (GetDriveTypeW(volume) != DRIVE_FIXED) {
                result.hr = COPYENGINE_E_RECYCLE_BIN_NOT_FOUND;
                result.error = L"This location cannot be safely sent to the Recycle Bin. The item was not deleted.";
                break;
            }
            const size_t slash = plain.find_last_of(L"\\/");
            const std::wstring parent_dir = slash == std::wstring::npos ? std::wstring{}
                                                                        : plain.substr(0, slash);
            if (!cached_transfer || parent_dir != cached_parent) {
                if (cached_transfer) cached_transfer->Unadvise(cached_cookie);
                cached_transfer.Reset();
                cached_cookie = 0;
                ComPtr<IShellItem> item, parent;
                if (SUCCEEDED(SHCreateItemFromParsingName(plain.c_str(), nullptr, IID_PPV_ARGS(&item))) &&
                    SUCCEEDED(item->GetParent(&parent)) &&
                    SUCCEEDED(parent->BindToHandler(nullptr, BHID_Transfer, IID_PPV_ARGS(&cached_transfer))) &&
                    SUCCEEDED(cached_transfer->Advise(recycle_sink.Get(), &cached_cookie))) {
                    cached_parent = parent_dir;
                } else {
                    cached_transfer.Reset();
                    result.hr = E_FAIL;
                    break;
                }
            }
            const HRESULT direct = RecycleOnly(source, cached_transfer.Get(), recycle_bin.Get(),
                                               *recycle_sink.Get(), cancel, result, progress);
            if (SUCCEEDED(direct)) {
                result.hr = direct;
                continue;
            }
            if (cancel.load()) {
                result.hr = kCancelled;
                result.cancelled = true;
                break;
            }
            // The transfer source refused this one. Keep it for the shell-delete
            // fallback instead of failing the whole batch: RecycleItem is stricter
            // than Explorer's own path and rejects some folders the shell handles
            // fine (OneDrive-backed ones report "recycling not available").
            deferred.push_back(source);
            continue;
        }
        // Only stage the item here; the batch runs once, after the loop.
        ComPtr<IShellItem> item;
        result.hr = SHCreateItemFromParsingName(path::StripExtendedPathPrefix(source).c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(result.hr)) break;
        PendingDelete entry;
        entry.sink.Attach(new (std::nothrow) DeleteSink(source, cancel, result, progress));
        if (!entry.sink) { result.hr = E_OUTOFMEMORY; break; }
        result.hr = batch->Advise(entry.sink.Get(), &entry.cookie);
        if (FAILED(result.hr)) break;
        result.hr = batch->DeleteItem(item.Get(), nullptr);
        if (FAILED(result.hr)) break;
        entry.source = source;
        pending.push_back(std::move(entry));
    }

    // Fallback for whatever the strict path refused: the shell's own delete — the
    // same call Explorer makes. FOF_ALLOWUNDO keeps it a recycle, and
    // FOFX_RECYCLEONDELETE additionally forces oversized items into the bin rather
    // than dropping them. The strict path stays the default precisely because it
    // cannot fall back to a permanent delete on its own; this one can, so it only
    // ever sees the leftovers.
    if (!deferred.empty() && !cancel.load()) {
        ComPtr<IFileOperation> shell_batch;
        HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&shell_batch));
        if (SUCCEEDED(hr)) hr = shell_batch->SetOwnerWindow(owner);
        if (SUCCEEDED(hr))
            hr = shell_batch->SetOperationFlags(FOF_NO_UI | FOF_NO_CONNECTED_ELEMENTS |
                                                FOFX_EARLYFAILURE | FOFX_NOCOPYHOOKS |
                                                FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE);
        if (SUCCEEDED(hr)) {
            std::vector<std::wstring> queued;
            for (const auto& source : deferred) {
                if (cancel.load()) break;
                ComPtr<IShellItem> item;
                if (FAILED(SHCreateItemFromParsingName(path::StripExtendedPathPrefix(source).c_str(),
                                                       nullptr, IID_PPV_ARGS(&item)))) continue;
                if (FAILED(shell_batch->DeleteItem(item.Get(), nullptr))) continue;
                queued.push_back(source);
            }
            if (!queued.empty()) {
                const HRESULT performed = shell_batch->PerformOperations();
                if (FAILED(performed) && SUCCEEDED(result.hr)) result.hr = performed;
                for (const auto& source : queued) {
                    if (!Missing(source)) continue;
                    result.mutated = true;
                    if (std::find(result.sources.begin(), result.sources.end(), source) == result.sources.end())
                        result.sources.push_back(source);
                }
            }
        }
    }

    // Run the batch, then let each sink report on its own file.
    if (permanent && !pending.empty() && SUCCEEDED(result.hr) && !cancel.load()) {
        const HRESULT performed = batch->PerformOperations();
        if (FAILED(performed)) result.hr = performed;
        BOOL aborted = FALSE;
        const HRESULT queried = batch->GetAnyOperationsAborted(&aborted);
        if (FAILED(queried) && SUCCEEDED(result.hr)) result.hr = queried;
        if (aborted && SUCCEEDED(result.hr)) result.hr = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
        for (auto& entry : pending) {
            const bool source_missing = Missing(entry.source);
            if (source_missing) result.mutated = true;
            if (SUCCEEDED(result.hr) && !aborted && entry.sink->DeferredDirectory() && source_missing &&
                (result.sources.empty() || result.sources.back() != entry.source))
                result.sources.push_back(entry.source);
            if (result.sources.empty() || result.sources.back() != entry.source) {
                // No completion callback must never become a fabricated success.
                if (SUCCEEDED(result.hr)) result.hr = E_FAIL;
                break;
            }
        }
    }
    for (auto& entry : pending) batch->Unadvise(entry.cookie);
    result.cancelled = cancel.load() || result.hr == kCancelled || result.hr == E_ABORT ||
                       result.hr == COPYENGINE_E_USER_CANCELLED;
    if (result.cancelled && SUCCEEDED(result.hr)) result.hr = kCancelled;
    if (cached_transfer) cached_transfer->Unadvise(cached_cookie);
    if (FAILED(result.hr) && !result.cancelled && result.error.empty()) result.error = ErrorText(result.hr);
    return result;
}
}
