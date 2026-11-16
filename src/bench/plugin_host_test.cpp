// End-to-end check of Luling.Plugin.exe: start the host, load the fixture
// plugin, invoke commands over the pipe, and see the error paths.

#include "../ipc/protocol.h"
#include "../plugin/plugin_protocol.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

using luling::ipc::MsgHeader;
using luling::ipc::PayloadReader;
using luling::ipc::PayloadWriter;

namespace {

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

bool WriteUtf8File(const std::wstring& path, const std::string& body) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;
    const size_t written = fwrite(body.data(), 1, body.size(), file);
    fclose(file);
    return written == body.size();
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    return path;
}

bool SendRequest(HANDLE pipe, uint32_t type, uint32_t request_id, const PayloadWriter& writer) {
    MsgHeader header;
    header.magic = luling::plugin::kPluginMagic;
    header.type = type;
    header.request_id = request_id;
    header.payload_size = static_cast<uint32_t>(writer.data().size());
    return luling::plugin::PipeWriteAll(pipe, &header, sizeof(header)) &&
           (writer.data().empty() ||
            luling::plugin::PipeWriteAll(pipe, writer.data().data(),
                                         static_cast<DWORD>(writer.data().size())));
}

bool ReadResponse(HANDLE pipe, MsgHeader& header, std::vector<uint8_t>& payload) {
    if (!luling::plugin::PipeReadAll(pipe, &header, sizeof(header))) return false;
    if (header.magic != luling::plugin::kPluginMagic ||
        header.payload_size > luling::ipc::kMaxPayload)
        return false;
    payload.assign(header.payload_size, 0);
    return payload.empty() ||
           luling::plugin::PipeReadAll(pipe, payload.data(), header.payload_size);
}

HANDLE ConnectWithDeadline(const std::wstring& name, HANDLE process, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) break;
        Sleep(25);
    }
    return INVALID_HANDLE_VALUE;
}

bool Invoke(HANDLE pipe, uint32_t request_id, const std::wstring& plugin_id,
            const std::wstring& command_id, const std::vector<std::wstring>& paths,
            uint32_t& status, std::wstring& message) {
    PayloadWriter writer;
    writer.PutString(plugin_id);
    writer.PutString(command_id);
    writer.PutStringArray(paths);
    if (!SendRequest(pipe, luling::plugin::REQ_INVOKE, request_id, writer)) return false;
    MsgHeader header{};
    std::vector<uint8_t> payload;
    if (!ReadResponse(pipe, header, payload) || header.type != luling::plugin::RSP_INVOKE ||
        header.request_id != request_id)
        return false;
    PayloadReader reader(payload.data(), payload.size());
    return reader.GetU32(status) && reader.GetString(message);
}

bool RequestPreview(HANDLE pipe, uint32_t request_id, const std::wstring& path, uint32_t edge,
                    uint32_t& status, uint32_t& width, uint32_t& height,
                    std::vector<uint8_t>& pixels) {
    PayloadWriter writer;
    writer.PutString(path);
    writer.PutU32(edge);
    if (!SendRequest(pipe, luling::plugin::REQ_PREVIEW, request_id, writer)) return false;
    MsgHeader header{};
    std::vector<uint8_t> payload;
    if (!ReadResponse(pipe, header, payload) || header.type != luling::plugin::RSP_PREVIEW ||
        header.request_id != request_id)
        return false;
    PayloadReader reader(payload.data(), payload.size());
    std::wstring message;
    if (!reader.GetU32(status) || !reader.GetU32(width) || !reader.GetU32(height) ||
        !reader.GetString(message))
        return false;
    pixels.clear();
    if (status == luling::plugin::PREVIEW_OK && width != 0 && height != 0)
        return reader.GetBytes(pixels, static_cast<size_t>(width) * height * 4);
    return true;
}

bool RequestColumns(HANDLE pipe, uint32_t request_id, const std::wstring& plugin_id,
                    const std::wstring& column_id, const std::vector<std::wstring>& paths,
                    uint32_t& status, std::wstring& values) {
    PayloadWriter writer;
    writer.PutString(plugin_id);
    writer.PutString(column_id);
    writer.PutStringArray(paths);
    if (!SendRequest(pipe, luling::plugin::REQ_COLUMNS, request_id, writer)) return false;
    MsgHeader header{};
    std::vector<uint8_t> payload;
    if (!ReadResponse(pipe, header, payload) || header.type != luling::plugin::RSP_COLUMNS ||
        header.request_id != request_id)
        return false;
    PayloadReader reader(payload.data(), payload.size());
    return reader.GetU32(status) && reader.GetString(values);
}

bool RequestPanel(HANDLE pipe, uint32_t request_id, const std::wstring& panel_id,
                  uint32_t& status, std::wstring& items) {
    PayloadWriter writer;
    writer.PutString(panel_id);
    writer.PutU32(luling::plugin::kPanelMaxItems);
    if (!SendRequest(pipe, luling::plugin::REQ_PANEL, request_id, writer)) return false;
    MsgHeader header{};
    std::vector<uint8_t> payload;
    if (!ReadResponse(pipe, header, payload) || header.type != luling::plugin::RSP_PANEL ||
        header.request_id != request_id)
        return false;
    PayloadReader reader(payload.data(), payload.size());
    return reader.GetU32(status) && reader.GetString(items);
}

bool RequestPanelAction(HANDLE pipe, uint32_t request_id, const std::wstring& panel_id,
                        const std::wstring& action_id, uint32_t& status,
                        std::wstring& message) {
    PayloadWriter writer;
    writer.PutString(panel_id);
    writer.PutString(action_id);
    if (!SendRequest(pipe, luling::plugin::REQ_PANEL_ACTION, request_id, writer)) return false;
    MsgHeader header{};
    std::vector<uint8_t> payload;
    if (!ReadResponse(pipe, header, payload) ||
        header.type != luling::plugin::RSP_PANEL_ACTION || header.request_id != request_id)
        return false;
    PayloadReader reader(payload.data(), payload.size());
    return reader.GetU32(status) && reader.GetString(message);
}

} // namespace

int main() {
    const std::wstring exe_dir = ExeDir();
    const std::wstring plugin_dir = exe_dir + L"fixture_plugin";
    Check(GetFileAttributesW((plugin_dir + L"\\fixture.dll").c_str()) != INVALID_FILE_ATTRIBUTES,
          "fixture plugin dll is present");
    const std::string manifest = R"({
  "schema": 2, "id": "fixture", "name": "Fixture", "version": "1.0.0",
  "api_version": 1, "main": "fixture.dll",
  "capabilities": ["ui.command", "ui.preview", "ui.column", "ui.panel"],
  "contributes": {
    "commands": [
      {"id": "echo", "title": "Echo"},
      {"id": "fail", "title": "Fail"}
    ],
    "previewers": [ { "extensions": ["fix"], "priority": 10 } ],
    "columns": [ { "id": "name-length", "title": "Name length", "width": 100 } ],
    "panels": [ { "id": "fixture-panel", "title": "Fixture panel" } ]
  }
})";
    Check(WriteUtf8File(plugin_dir + L"\\manifest.json", manifest), "fixture manifest written");

    const uint32_t token = 0x5A5A0001u ^ static_cast<uint32_t>(GetCurrentProcessId());
    std::wstring command_line = L"\"" + exe_dir + L"Luling.Plugin.exe\" " +
                                std::to_wstring(GetCurrentProcessId()) + L" " +
                                std::to_wstring(token);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    Check(CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &child),
          "plugin host started");
    if (!child.hProcess) return 1;
    HANDLE pipe = ConnectWithDeadline(luling::plugin::PluginPipeName(GetCurrentProcessId(), token),
                                      child.hProcess, 3000);
    Check(pipe != INVALID_HANDLE_VALUE, "connected to the plugin host");
    if (pipe == INVALID_HANDLE_VALUE) {
        TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hProcess);
        CloseHandle(child.hThread);
        return 1;
    }

    PayloadWriter hello;
    hello.PutU32(1);
    hello.PutString(L"fixture");
    hello.PutString(plugin_dir);
    Check(SendRequest(pipe, luling::plugin::REQ_HELLO, 1, hello), "hello sent");
    MsgHeader header{};
    std::vector<uint8_t> payload;
    Check(ReadResponse(pipe, header, payload) && header.type == luling::plugin::RSP_HELLO,
          "hello answered");
    {
        PayloadReader reader(payload.data(), payload.size());
        uint32_t count = 0;
        std::wstring id;
        std::wstring error;
        uint32_t status = 0xFFFFFFFFu;
        Check(reader.GetU32(count) && count == 1 && reader.GetString(id) && id == L"fixture" &&
                  reader.GetU32(status) && reader.GetString(error),
              "hello payload is well formed");
        Check(status == luling::plugin::LOAD_OK, "fixture plugin loaded");
    }

    uint32_t status = 0xFFFFFFFFu;
    std::wstring message;
    Check(Invoke(pipe, 2, L"fixture", L"echo", {L"C:\\a.txt", L"C:\\b.txt"}, status, message),
          "echo invocation answered");
    Check(status == luling::plugin::INVOKE_OK, "echo succeeded");
    Check(message == L"count=2 first=C:\\a.txt", "the plugin saw the paths");

    Check(Invoke(pipe, 3, L"fixture", L"fail", {}, status, message), "failing invocation answered");
    Check(status == luling::plugin::INVOKE_PLUGIN_FAILED, "a plugin failure is reported");
    Check(message == L"failed on purpose", "the plugin's message is forwarded");

    Check(Invoke(pipe, 4, L"fixture", L"nope", {}, status, message), "unknown command answered");
    Check(status == luling::plugin::INVOKE_UNKNOWN_COMMAND, "unknown commands are refused");

    Check(Invoke(pipe, 5, L"missing", L"echo", {}, status, message), "unknown plugin answered");
    Check(status == luling::plugin::INVOKE_UNKNOWN_PLUGIN, "unknown plugins are refused");

    Check(SendRequest(pipe, luling::plugin::REQ_PING, 6, PayloadWriter{}), "ping sent");
    Check(ReadResponse(pipe, header, payload) && header.type == luling::plugin::RSP_PONG,
          "ping answered");

    uint32_t preview_status = 0xFFFFFFFFu;
    uint32_t preview_w = 0;
    uint32_t preview_h = 0;
    std::vector<uint8_t> preview_pixels;
    Check(RequestPreview(pipe, 8, L"C:\\sample.fix", 128, preview_status, preview_w, preview_h,
                         preview_pixels),
          "preview answered");
    Check(preview_status == luling::plugin::PREVIEW_OK, "plugin preview rendered");
    Check(preview_w == 128 && preview_h == 128, "preview honours the requested edge");
    Check(preview_pixels.size() == static_cast<size_t>(128) * 128 * 4,
          "preview pixel buffer size");
    Check(RequestPreview(pipe, 9, L"C:\\sample.txt", 128, preview_status, preview_w, preview_h,
                         preview_pixels),
          "unclaimed preview answered");
    Check(preview_status == luling::plugin::PREVIEW_NONE,
          "extensions without a previewer are not claimed");

    uint32_t columns_status = 0xFFFFFFFFu;
    std::wstring columns_values;
    Check(RequestColumns(pipe, 11, L"fixture", L"name-length",
                         {L"C:\\alpha.txt", L"C:\\dir\\beta.fix"}, columns_status,
                         columns_values),
          "column values answered");
    Check(columns_status == luling::plugin::PREVIEW_OK, "column values rendered");
    Check(columns_values == L"9\x1F" L"8", "column values follow the path order");
    Check(RequestColumns(pipe, 12, L"fixture", L"unknown", {L"C:\\a"}, columns_status,
                         columns_values),
          "unknown column answered");
    Check(columns_status == luling::plugin::PREVIEW_NONE, "unknown columns are refused");

    uint32_t panel_status = 0xFFFFFFFFu;
    std::wstring panel_items;
    Check(RequestPanel(pipe, 13, L"fixture-panel", panel_status, panel_items),
          "panel answered");
    Check(panel_status == luling::plugin::PREVIEW_OK, "panel rows rendered");
    Check(panel_items.find(L"示例条目二") != std::wstring::npos, "panel rows carry titles");
    Check(panel_items.find(L"\x1Fthree") != std::wstring::npos, "panel rows carry actions");
    Check(RequestPanel(pipe, 14, L"missing-panel", panel_status, panel_items),
          "unknown panel answered");
    Check(panel_status == luling::plugin::PREVIEW_NONE, "unknown panels are refused");

    uint32_t action_status = 0xFFFFFFFFu;
    std::wstring action_message;
    Check(RequestPanelAction(pipe, 15, L"fixture-panel", L"one", action_status,
                             action_message),
          "panel action answered");
    Check(action_status == luling::plugin::PREVIEW_OK, "panel action ran");
    Check(RequestPanelAction(pipe, 16, L"fixture-panel", L"nope", action_status,
                             action_message),
          "unknown panel action answered");
    Check(action_status == luling::plugin::PREVIEW_FAILED &&
              action_message == L"unknown action",
          "unknown panel actions are reported");

    Check(SendRequest(pipe, luling::plugin::REQ_SHUTDOWN, 17, PayloadWriter{}), "shutdown sent");
    CloseHandle(pipe);
    Check(WaitForSingleObject(child.hProcess, 3000) == WAIT_OBJECT_0, "host exited on shutdown");
    if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hProcess);
    CloseHandle(child.hThread);

    if (failures == 0) {
        std::printf("[done] PASS\n");
        return 0;
    }
    std::printf("[done] FAIL (%d)\n", failures);
    return 1;
}
