#include "plugin_client.h"

#include "../app/app_messages.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include "../ipc/protocol.h"

#include <algorithm>
#include <cstdlib>

namespace luling::app {
namespace {

using luling::diagnostics::runtime::Level;

constexpr DWORD kConnectTimeoutMs = 3000;
constexpr DWORD kHelloTimeoutMs = 5000;
constexpr DWORD kPreviewTimeoutMs = 10000;
// Pipes are synchronous byte streams: poll for readability so every read has
// an absolute deadline without moving the handle to overlapped mode.
bool ReadWithDeadline(HANDLE pipe, void* data, DWORD bytes, ULONGLONG deadline,
                      const std::atomic<bool>& stopping) {
    auto* out = static_cast<unsigned char*>(data);
    while (bytes) {
        if (stopping.load() || GetTickCount64() >= deadline) return false;
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available == 0) {
            Sleep(5);
            continue;
        }
        const DWORD want = available < bytes ? available : bytes;
        DWORD got = 0;
        if (!ReadFile(pipe, out, want, &got, nullptr) || got == 0) return false;
        out += got;
        bytes -= got;
    }
    return true;
}

bool SendRequest(HANDLE pipe, uint32_t type, uint32_t request_id,
                 const ipc::PayloadWriter& writer) {    ipc::MsgHeader header;
    header.magic = plugin::kPluginMagic;
    header.type = type;
    header.request_id = request_id;
    header.payload_size = static_cast<uint32_t>(writer.data().size());
    if (!plugin::PipeWriteAll(pipe, &header, sizeof(header))) return false;
    return writer.data().empty() ||
           plugin::PipeWriteAll(pipe, writer.data().data(),
                                static_cast<DWORD>(writer.data().size()));
}

// One string per path, joined by U+001F; missing trailing entries read empty.
std::vector<std::wstring> SplitColumnValues(const std::wstring& text, size_t count) {
    std::vector<std::wstring> out;
    out.reserve(count);
    size_t start = 0;
    while (out.size() < count) {
        const size_t pos = text.find(plugin::kColumnValueSeparator, start);
        if (pos == std::wstring::npos) {
            out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    out.resize(count);
    return out;
}

// Records joined by U+001E; each record is glyph, title, subtitle, action.
std::vector<PluginPanelItem> ParsePanelItems(const std::wstring& text) {
    std::vector<PluginPanelItem> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find(plugin::kPanelRecordSeparator, start);
        const std::wstring record = text.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!record.empty()) {
            std::wstring fields[4];
            size_t pos = 0;
            for (int i = 0; i < 4; ++i) {
                const size_t sep = record.find(plugin::kColumnValueSeparator, pos);
                fields[i] = record.substr(
                    pos, sep == std::wstring::npos ? std::wstring::npos : sep - pos);
                if (sep == std::wstring::npos) break;
                pos = sep + 1;
            }
            PluginPanelItem item;
            item.glyph = std::move(fields[0]);
            item.title = std::move(fields[1]);
            item.subtitle = std::move(fields[2]);
            item.action = std::move(fields[3]);
            if (!item.title.empty()) out.push_back(std::move(item));
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return out;
}

} // namespace

PluginClient& PluginClient::Instance() {
    static PluginClient instance;
    return instance;
}

PluginClient::~PluginClient() { Stop(); }

void PluginClient::Start(HWND notify_window) {
    std::lock_guard lock(mutex_);
    notify_ = notify_window;
    if (running_) return;
    stopping_ = false;
    running_ = true;
    worker_ = std::thread([this] { Run(); });
}

void PluginClient::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
        stopping_ = true;
        notify_ = nullptr;
        queue_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void PluginClient::Invoke(const std::wstring& plugin_id, const std::wstring& command_id,
                          const std::wstring& plugin_name, const std::wstring& command_title,
                          const std::vector<std::wstring>& paths,
                          std::vector<plugin::PluginEntry> plugins) {
    std::lock_guard lock(mutex_);
    if (!running_ || !notify_) return;
    Task task;
    task.plugin_id = plugin_id;
    task.command_id = command_id;
    task.plugin_name = plugin_name;
    task.command_title = command_title;
    task.paths = paths;
    task.plugins = std::move(plugins);
    task.request_id = next_request_id_++;
    if (next_request_id_ == 0) next_request_id_ = 1;
    queue_.push_back(std::move(task));
    wake_.notify_one();
}

void PluginClient::Preview(const std::wstring& path, uint32_t max_px,
                           std::vector<plugin::PluginEntry> plugins) {
    std::lock_guard lock(mutex_);
    if (!running_ || !notify_) return;
    Task task;
    task.kind = Task::Kind::Preview;
    task.preview_path = path;
    task.preview_edge = max_px;
    task.plugins = std::move(plugins);
    task.request_id = next_request_id_++;
    if (next_request_id_ == 0) next_request_id_ = 1;
    queue_.push_back(std::move(task));
    wake_.notify_one();
}

void PluginClient::Columns(int index, const std::wstring& plugin_id,
                           const std::wstring& column_id, std::vector<std::wstring> paths,
                           std::vector<plugin::PluginEntry> plugins) {
    std::lock_guard lock(mutex_);
    if (!running_ || !notify_ || paths.empty()) return;
    Task task;
    task.kind = Task::Kind::Columns;
    task.column_index = index;
    task.plugin_id = plugin_id;
    task.column_id = column_id;
    task.paths = std::move(paths);
    task.plugins = std::move(plugins);
    task.request_id = next_request_id_++;
    if (next_request_id_ == 0) next_request_id_ = 1;
    queue_.push_back(std::move(task));
    wake_.notify_one();
}

bool PluginClient::Panel(const std::wstring& panel_id,
                         std::vector<plugin::PluginEntry> plugins) {
    std::lock_guard lock(mutex_);
    if (!running_ || !notify_ || panel_id.empty()) return false;
    Task task;
    task.kind = Task::Kind::Panel;
    task.panel_id = panel_id;
    task.plugins = std::move(plugins);
    task.request_id = next_request_id_++;
    if (next_request_id_ == 0) next_request_id_ = 1;
    queue_.push_back(std::move(task));
    wake_.notify_one();
    return true;
}

bool PluginClient::PanelAction(const std::wstring& panel_id, const std::wstring& action_id,
                               std::vector<plugin::PluginEntry> plugins) {
    std::lock_guard lock(mutex_);
    if (!running_ || !notify_ || panel_id.empty() || action_id.empty()) return false;
    Task task;
    task.kind = Task::Kind::PanelAction;
    task.panel_id = panel_id;
    task.action_id = action_id;
    task.plugins = std::move(plugins);
    task.request_id = next_request_id_++;
    if (next_request_id_ == 0) next_request_id_ = 1;
    queue_.push_back(std::move(task));
    wake_.notify_one();
    return true;
}

void PluginClient::Deliver(PluginCommandResult result) {
    auto* payload = new PluginCommandResult(std::move(result));
    HWND window = nullptr;
    {
        std::lock_guard lock(mutex_);
        window = notify_;
    }
    if (!window || !PostMessageW(window, WM_PLUGIN_COMMAND_DONE, 0,
                                 reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void PluginClient::DeliverPreview(PluginPreviewResult result) {
    auto* payload = new PluginPreviewResult(std::move(result));
    HWND window = nullptr;
    {
        std::lock_guard lock(mutex_);
        window = notify_;
    }
    if (!window || !PostMessageW(window, WM_PLUGIN_PREVIEW_DONE, 0,
                                 reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void PluginClient::DeliverColumns(PluginColumnsResult result) {
    auto* payload = new PluginColumnsResult(std::move(result));
    HWND window = nullptr;
    {
        std::lock_guard lock(mutex_);
        window = notify_;
    }
    if (!window || !PostMessageW(window, WM_PLUGIN_COLUMNS_DONE, 0,
                                 reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void PluginClient::DeliverPanel(PluginPanelResult result) {
    auto* payload = new PluginPanelResult(std::move(result));
    HWND window = nullptr;
    {
        std::lock_guard lock(mutex_);
        window = notify_;
    }
    if (!window || !PostMessageW(window, WM_PLUGIN_PANEL_DONE, 0,
                                 reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void PluginClient::DeliverPanelAction(PluginPanelActionResult result) {
    auto* payload = new PluginPanelActionResult(std::move(result));
    HWND window = nullptr;
    {
        std::lock_guard lock(mutex_);
        window = notify_;
    }
    if (!window || !PostMessageW(window, WM_PLUGIN_PANEL_ACTION_DONE, 0,
                                 reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void PluginClient::StopChild() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (child_.hProcess) {
        WaitForSingleObject(child_.hProcess, 200);
        if (WaitForSingleObject(child_.hProcess, 0) != WAIT_OBJECT_0)
            TerminateProcess(child_.hProcess, 1);
        CloseHandle(child_.hProcess);
        child_.hProcess = nullptr;
    }
    if (child_.hThread) {
        CloseHandle(child_.hThread);
        child_.hThread = nullptr;
    }
    child_.dwProcessId = 0;
    hello_done_ = false;
    hello_signature_.clear();
}

bool PluginClient::EnsureHost() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        ULONG server_pid = 0;
        const bool same_host = GetNamedPipeServerProcessId(pipe_, &server_pid) &&
                               server_pid == child_.dwProcessId;
        const bool alive = !child_.hProcess ||
                           WaitForSingleObject(child_.hProcess, 0) != WAIT_OBJECT_0;
        if (same_host && alive) return true;
        StopChild();
    }
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe))) return false;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    *(slash + 1) = 0;
    token_ = static_cast<uint32_t>(GetCurrentProcessId()) ^
             static_cast<uint32_t>(GetTickCount64());
    if (token_ == 0) token_ = 1;
    const std::wstring pipe_name = plugin::PluginPipeName(GetCurrentProcessId(), token_);
    std::wstring command_line = L"\"" + std::wstring(exe) + L"Luling.Plugin.exe\" " +
                                std::to_wstring(GetCurrentProcessId()) + L" " +
                                std::to_wstring(token_);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_FORCEOFFFEEDBACK;
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child_)) {
        const DWORD error = GetLastError();
        child_ = {};
        diagnostics::runtime::Event("plugin_host_failure", {{"stage", 1}, {"error", error}},
                                   Level::Error);
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + kConnectTimeoutMs;
    DWORD last_error = ERROR_SUCCESS;
    while (GetTickCount64() < deadline) {
        pipe_ = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            ULONG server_pid = 0;
            if (GetNamedPipeServerProcessId(pipe_, &server_pid) &&
                server_pid == child_.dwProcessId)
                return true;
            last_error = GetLastError();
            StopChild();
            return false;
        }
        last_error = GetLastError();
        if (WaitForSingleObject(child_.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(25);
    }
    diagnostics::runtime::Event("plugin_host_failure", {{"stage", 2}, {"error", last_error}},
                               Level::Error);
    StopChild();
    return false;
}

bool PluginClient::HelloIfNeeded(const std::vector<plugin::PluginEntry>& plugins) {
    std::wstring signature;
    for (const plugin::PluginEntry& entry : plugins) {
        signature += entry.id;
        signature += L'\x01';
        signature += entry.dir;
        signature += L'\x02';
    }
    if (hello_done_ && hello_signature_ == signature && pipe_ != INVALID_HANDLE_VALUE) return true;
    ipc::PayloadWriter writer;
    writer.PutU32(static_cast<uint32_t>(plugins.size()));
    for (const plugin::PluginEntry& entry : plugins) {
        writer.PutString(entry.id);
        writer.PutString(entry.dir);
    }
    const ULONGLONG deadline = GetTickCount64() + kHelloTimeoutMs;
    if (!SendRequest(pipe_, plugin::REQ_HELLO, 0, writer)) {
        StopChild();
        return false;
    }
    ipc::MsgHeader header{};
    if (!ReadWithDeadline(pipe_, &header, sizeof(header), deadline, stopping_) ||
        header.magic != plugin::kPluginMagic || header.type != plugin::RSP_HELLO) {
        diagnostics::runtime::Event("plugin_host_failure", {{"stage", 3}, {"error", 0}},
                                   Level::Error);
        StopChild();
        return false;
    }
    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size &&
        !ReadWithDeadline(pipe_, payload.data(), header.payload_size, deadline, stopping_)) {
        StopChild();
        return false;
    }
    hello_done_ = true;
    hello_signature_ = std::move(signature);
    return true;
}

uint32_t PluginClient::Execute(Task& task, std::wstring& message) {
    if (!EnsureHost()) {
        message = l10n::Pick(L"无法启动插件宿主进程", L"Could not start the plugin host");
        return plugin::INVOKE_PLUGIN_FAILED;
    }
    if (!HelloIfNeeded(task.plugins)) {
        message = l10n::Pick(L"插件宿主没有响应", L"The plugin host did not respond");
        return plugin::INVOKE_PLUGIN_FAILED;
    }
    ipc::PayloadWriter writer;
    writer.PutString(task.plugin_id);
    writer.PutString(task.command_id);
    writer.PutStringArray(task.paths);
    if (!SendRequest(pipe_, plugin::REQ_INVOKE, task.request_id, writer)) {
        StopChild();
        message = l10n::Pick(L"插件宿主已退出", L"The plugin host exited");
        return plugin::INVOKE_PLUGIN_FAILED;
    }
    const ULONGLONG deadline = GetTickCount64() + plugin::kInvokeTimeoutMs;
    ipc::MsgHeader header{};
    if (!ReadWithDeadline(pipe_, &header, sizeof(header), deadline, stopping_) ||
        header.magic != plugin::kPluginMagic || header.type != plugin::RSP_INVOKE ||
        header.request_id != task.request_id) {
        diagnostics::runtime::Event("plugin_invoke_failure", {{"stage", 1}, {"error", 0}},
                                   Level::Error);
        StopChild();
        message = l10n::Pick(L"插件没有在限定时间内返回", L"The plugin did not answer in time");
        return plugin::INVOKE_TIMEOUT;
    }
    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size &&
        !ReadWithDeadline(pipe_, payload.data(), header.payload_size, deadline, stopping_)) {
        StopChild();
        message = l10n::Pick(L"插件宿主已退出", L"The plugin host exited");
        return plugin::INVOKE_PLUGIN_FAILED;
    }
    ipc::PayloadReader reader(payload.data(), payload.size());
    uint32_t status = plugin::INVOKE_PLUGIN_FAILED;
    std::wstring text;
    if (!reader.GetU32(status) || !reader.GetString(text)) {
        StopChild();
        return plugin::INVOKE_PLUGIN_FAILED;
    }
    message = text;
    return status;
}

void PluginClient::ExecutePreview(Task& task, PluginPreviewResult& result) {
    if (!EnsureHost()) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    if (!HelloIfNeeded(task.plugins)) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    ipc::PayloadWriter writer;
    writer.PutString(task.preview_path);
    writer.PutU32(task.preview_edge);
    if (!SendRequest(pipe_, plugin::REQ_PREVIEW, task.request_id, writer)) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    const ULONGLONG deadline = GetTickCount64() + kPreviewTimeoutMs;
    ipc::MsgHeader header{};
    if (!ReadWithDeadline(pipe_, &header, sizeof(header), deadline, stopping_) ||
        header.magic != plugin::kPluginMagic || header.type != plugin::RSP_PREVIEW ||
        header.request_id != task.request_id) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size &&
        !ReadWithDeadline(pipe_, payload.data(), header.payload_size, deadline, stopping_)) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    ipc::PayloadReader reader(payload.data(), payload.size());
    uint32_t status = plugin::PREVIEW_FAILED;
    uint32_t width = 0;
    uint32_t height = 0;
    std::wstring message;
    if (!reader.GetU32(status) || !reader.GetU32(width) || !reader.GetU32(height) ||
        !reader.GetString(message)) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    result.status = status;
    result.width = width;
    result.height = height;
    result.message = std::move(message);
    if (status == plugin::PREVIEW_OK && width != 0 && height != 0) {
        const size_t bytes = static_cast<size_t>(width) * height * 4;
        if (!reader.GetBytes(result.bgra, bytes)) {
            result.status = plugin::PREVIEW_FAILED;
            result.bgra.clear();
        }
    }
}

void PluginClient::ExecuteColumns(Task& task, PluginColumnsResult& result) {
    result.index = task.column_index;
    result.paths = task.paths;
    result.values.assign(task.paths.size(), std::wstring());
    if (!EnsureHost()) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    if (!HelloIfNeeded(task.plugins)) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    ipc::PayloadWriter writer;
    writer.PutString(task.plugin_id);
    writer.PutString(task.column_id);
    writer.PutStringArray(task.paths);
    if (!SendRequest(pipe_, plugin::REQ_COLUMNS, task.request_id, writer)) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    const ULONGLONG deadline = GetTickCount64() + kPreviewTimeoutMs;
    ipc::MsgHeader header{};
    if (!ReadWithDeadline(pipe_, &header, sizeof(header), deadline, stopping_) ||
        header.magic != plugin::kPluginMagic || header.type != plugin::RSP_COLUMNS ||
        header.request_id != task.request_id) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size &&
        !ReadWithDeadline(pipe_, payload.data(), header.payload_size, deadline, stopping_)) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    ipc::PayloadReader reader(payload.data(), payload.size());
    uint32_t status = plugin::PREVIEW_FAILED;
    std::wstring values;
    if (!reader.GetU32(status) || !reader.GetString(values)) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    result.status = status;
    if (status == plugin::PREVIEW_OK)
        result.values = SplitColumnValues(values, task.paths.size());
}

void PluginClient::ExecutePanel(Task& task, PluginPanelResult& result) {
    result.panel_id = task.panel_id;
    if (!EnsureHost() || !HelloIfNeeded(task.plugins)) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    ipc::PayloadWriter writer;
    writer.PutString(task.panel_id);
    writer.PutU32(plugin::kPanelMaxItems);
    if (!SendRequest(pipe_, plugin::REQ_PANEL, task.request_id, writer)) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    const ULONGLONG deadline = GetTickCount64() + kPreviewTimeoutMs;
    ipc::MsgHeader header{};
    if (!ReadWithDeadline(pipe_, &header, sizeof(header), deadline, stopping_) ||
        header.magic != plugin::kPluginMagic || header.type != plugin::RSP_PANEL ||
        header.request_id != task.request_id) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size &&
        !ReadWithDeadline(pipe_, payload.data(), header.payload_size, deadline, stopping_)) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    ipc::PayloadReader reader(payload.data(), payload.size());
    uint32_t status = plugin::PREVIEW_FAILED;
    std::wstring text;
    if (!reader.GetU32(status) || !reader.GetString(text)) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    result.status = status;
    if (status == plugin::PREVIEW_OK) result.items = ParsePanelItems(text);
}

void PluginClient::ExecutePanelAction(Task& task, PluginPanelActionResult& result) {
    result.panel_id = task.panel_id;
    result.action_id = task.action_id;
    if (!EnsureHost() || !HelloIfNeeded(task.plugins)) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    ipc::PayloadWriter writer;
    writer.PutString(task.panel_id);
    writer.PutString(task.action_id);
    if (!SendRequest(pipe_, plugin::REQ_PANEL_ACTION, task.request_id, writer)) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    const ULONGLONG deadline = GetTickCount64() + kPreviewTimeoutMs;
    ipc::MsgHeader header{};
    if (!ReadWithDeadline(pipe_, &header, sizeof(header), deadline, stopping_) ||
        header.magic != plugin::kPluginMagic || header.type != plugin::RSP_PANEL_ACTION ||
        header.request_id != task.request_id) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    std::vector<uint8_t> payload(header.payload_size);
    if (header.payload_size &&
        !ReadWithDeadline(pipe_, payload.data(), header.payload_size, deadline, stopping_)) {
        StopChild();
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    ipc::PayloadReader reader(payload.data(), payload.size());
    uint32_t status = plugin::PREVIEW_FAILED;
    std::wstring message;
    if (!reader.GetU32(status) || !reader.GetString(message)) {
        result.status = plugin::PREVIEW_FAILED;
        return;
    }
    result.status = status;
    result.message = std::move(message);
}

void PluginClient::Run() {
    for (;;) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return !running_ || !queue_.empty(); });
            if (!running_ && queue_.empty()) break;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        if (task.kind == Task::Kind::Preview) {
            PluginPreviewResult result;
            result.path = task.preview_path;
            ExecutePreview(task, result);
            DeliverPreview(std::move(result));
            continue;
        }
        if (task.kind == Task::Kind::Columns) {
            PluginColumnsResult result;
            ExecuteColumns(task, result);
            DeliverColumns(std::move(result));
            continue;
        }
        if (task.kind == Task::Kind::Panel) {
            PluginPanelResult result;
            ExecutePanel(task, result);
            DeliverPanel(std::move(result));
            continue;
        }
        if (task.kind == Task::Kind::PanelAction) {
            PluginPanelActionResult result;
            ExecutePanelAction(task, result);
            DeliverPanelAction(std::move(result));
            continue;
        }
        std::wstring message;
        PluginCommandResult result;
        result.plugin_name = task.plugin_name;
        result.command_title = task.command_title;
        result.status = Execute(task, message);
        result.message = std::move(message);
        Deliver(std::move(result));
    }
    if (pipe_ != INVALID_HANDLE_VALUE) {
        SendRequest(pipe_, plugin::REQ_SHUTDOWN, 0, ipc::PayloadWriter{});
        Sleep(50);
    }
    StopChild();
}

} // namespace luling::app
