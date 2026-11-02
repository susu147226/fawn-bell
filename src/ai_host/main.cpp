// Luling.AI.exe - the MCP host: JSON-RPC over stdio for Claude and other AI
// apps, forwarded to luling.exe's broker over the named pipe. The broker owns
// the permissions, the confirmation dialogs and the activity log.
#include "../common/json_value.h"
#include "../ipc/ai_tool_protocol.h"

#include <windows.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <string>

namespace {

HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::wstring g_client = L"AI 应用";
int64_t g_request_id = 1;

void Disconnect() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

bool Connect() {
    if (g_pipe != INVALID_HANDLE_VALUE) return true;
    for (int attempt = 0; attempt < 5; ++attempt) {
        g_pipe = CreateFileW(luling::ipc::ai::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
        if (g_pipe != INVALID_HANDLE_VALUE) break;
        WaitNamedPipeW(luling::ipc::ai::kPipeName, 200);
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    luling::json::Value hello = luling::json::Value::Object();
    hello.set("id", g_request_id++);
    hello.set("op", "hello");
    hello.set("client", luling::json::Utf8(g_client));
    if (!luling::ipc::ai::WriteFrame(g_pipe, luling::json::Serialize(hello))) {
        Disconnect();
        return false;
    }
    std::string frame;
    luling::json::Value reply;
    if (!luling::ipc::ai::ReadFrame(g_pipe, frame) || !luling::json::Parse(frame, reply) ||
        !reply.get_bool("ok", false)) {
        Disconnect();
        return false;
    }
    int64_t protocol = 0;
    const luling::json::Value* version = reply.find("protocol");
    if (!version || !version->get_int(protocol) || protocol != luling::ipc::ai::kProtocolVersion) {
        Disconnect();
        return false;
    }
    return true;
}

// One request/response with the broker; false when it is not running.
bool BrokerCall(luling::json::Value request, luling::json::Value& response) {
    if (!Connect()) return false;
    request.set("id", g_request_id++);
    if (!luling::ipc::ai::WriteFrame(g_pipe, luling::json::Serialize(request))) {
        Disconnect();
        return false;
    }
    std::string frame;
    if (!luling::ipc::ai::ReadFrame(g_pipe, frame) || !luling::json::Parse(frame, response)) {
        Disconnect();
        return false;
    }
    return true;
}

void WriteLine(const luling::json::Value& message) {
    const std::string text = luling::json::Serialize(message) + "\n";
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

luling::json::Value Result(const luling::json::Value* id, luling::json::Value result) {
    luling::json::Value out = luling::json::Value::Object();
    out.set("jsonrpc", "2.0");
    if (id) out.set("id", *id);
    out.set("result", std::move(result));
    return out;
}

luling::json::Value Error(const luling::json::Value* id, int code, const std::string& message) {
    luling::json::Value out = luling::json::Value::Object();
    out.set("jsonrpc", "2.0");
    if (id) out.set("id", *id);
    luling::json::Value error = luling::json::Value::Object();
    error.set("code", static_cast<int64_t>(code));
    error.set("message", message);
    out.set("error", std::move(error));
    return out;
}

luling::json::Value TextResult(const std::string& text, bool is_error) {
    luling::json::Value content = luling::json::Value::Array();
    luling::json::Value entry = luling::json::Value::Object();
    entry.set("type", "text");
    entry.set("text", text);
    content.push(std::move(entry));
    luling::json::Value result = luling::json::Value::Object();
    result.set("content", std::move(content));
    if (is_error) result.set("isError", true);
    return result;
}

void Handle(const luling::json::Value& message) {
    const luling::json::Value* id = message.find("id");
    const std::string method = message.get_string("method");
    const luling::json::Value* params = message.find("params");
    if (method.empty()) return;   // a response or a stray object: nothing to do

    if (method == "initialize") {
        // Version negotiation (MCP lifecycle): answer with the client's
        // version when we speak it, else with the newest one we do.
        constexpr const char* kVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};
        std::string asked;
        if (params && params->is_object()) {
            asked = params->get_string("protocolVersion");
            if (const luling::json::Value* info = params->find("clientInfo"); info && info->is_object()) {
                const std::wstring name = luling::json::Wide(info->get_string("name"));
                if (!name.empty() && name.size() <= 64) g_client = name;
            }
        }
        std::string agreed = kVersions[0];
        for (const char* known : kVersions) {
            if (asked == known) {
                agreed = known;
                break;
            }
        }
        // Ready for tools/list; the broker connects on the first call.
        luling::json::Value result = luling::json::Value::Object();
        result.set("protocolVersion", agreed);
        luling::json::Value capabilities = luling::json::Value::Object();
        luling::json::Value tools = luling::json::Value::Object();
        tools.set("listChanged", false);
        capabilities.set("tools", std::move(tools));
        // Listed empty, but a client that asks finds a well-formed answer.
        luling::json::Value prompts = luling::json::Value::Object();
        prompts.set("listChanged", false);
        capabilities.set("prompts", std::move(prompts));
        luling::json::Value resources = luling::json::Value::Object();
        resources.set("subscribe", false);
        resources.set("listChanged", false);
        capabilities.set("resources", std::move(resources));
        result.set("capabilities", std::move(capabilities));
        result.set("instructions",
                   "Luling's files. Read tools first; anything that changes files asks the user "
                   "in Luling before it runs.");
        luling::json::Value info = luling::json::Value::Object();
        info.set("name", "luling");
        info.set("version", "1.0");
        result.set("serverInfo", std::move(info));
        if (id) WriteLine(Result(id, std::move(result)));
        return;
    }
    if (method == "notifications/initialized" || method == "notifications/cancelled") return;
    if (!id) return;

    if (method == "ping" || method == "logging/setLevel") {
        WriteLine(Result(id, luling::json::Value::Object()));
        return;
    }
    if (method == "prompts/list") {
        luling::json::Value result = luling::json::Value::Object();
        result.set("prompts", luling::json::Value::Array());
        WriteLine(Result(id, std::move(result)));
        return;
    }
    if (method == "resources/list") {
        luling::json::Value result = luling::json::Value::Object();
        result.set("resources", luling::json::Value::Array());
        WriteLine(Result(id, std::move(result)));
        return;
    }
    if (method == "resources/templates/list") {
        luling::json::Value result = luling::json::Value::Object();
        result.set("resourceTemplates", luling::json::Value::Array());
        WriteLine(Result(id, std::move(result)));
        return;
    }
    if (method == "tools/list") {
        luling::json::Value request = luling::json::Value::Object();
        request.set("op", "tools");
        luling::json::Value response;
        luling::json::Value result = luling::json::Value::Object();
        luling::json::Value list = luling::json::Value::Array();
        if (BrokerCall(std::move(request), response) && response.get_bool("ok", false)) {
            // The broker hands out the app's OpenAI-shaped tool table; MCP
            // wants name / description / inputSchema at the top level.
            if (const luling::json::Value* tools = response.find("tools"); tools && tools->is_array()) {
                for (const luling::json::Value& entry : tools->items()) {
                    const luling::json::Value* fn = entry.find("function");
                    if (!fn || !fn->is_object()) continue;
                    const std::string name = fn->get_string("name");
                    if (name.empty()) continue;
                    luling::json::Value tool = luling::json::Value::Object();
                    tool.set("name", name);
                    tool.set("description", fn->get_string("description"));
                    if (const luling::json::Value* schema = fn->find("parameters");
                        schema && schema->is_object()) {
                        luling::json::Value copy = *schema;
                        tool.set("inputSchema", std::move(copy));
                    } else {
                        tool.set("inputSchema", luling::json::Value::Object());
                    }
                    list.push(std::move(tool));
                }
            }
        }
        result.set("tools", std::move(list));
        WriteLine(Result(id, std::move(result)));
        return;
    }
    if (method == "tools/call") {
        std::string name;
        luling::json::Value arguments = luling::json::Value::Object();
        if (params && params->is_object()) {
            name = params->get_string("name");
            if (const luling::json::Value* args = params->find("arguments"); args && args->is_object())
                arguments = *args;
        }
        if (name.empty()) {
            WriteLine(Result(id, TextResult("The call did not name a tool.", true)));
            return;
        }
        luling::json::Value request = luling::json::Value::Object();
        request.set("op", "call");
        request.set("tool", name);
        request.set("arguments", std::move(arguments));
        luling::json::Value response;
        if (!BrokerCall(std::move(request), response)) {
            WriteLine(Result(id, TextResult(
                "Luling is not running. Start Luling and try again; the MCP host talks to its window.",
                true)));
            return;
        }
        if (!response.get_bool("ok", false)) {
            std::string text = response.get_string("message");
            const std::string code = response.get_string("code");
            if (!code.empty()) text += " (" + code + ")";
            WriteLine(Result(id, TextResult(text, true)));
            return;
        }
        const luling::json::Value* value = response.find("result");
        const std::string text = value ? luling::json::Serialize(*value) : std::string("{}");
        WriteLine(Result(id, TextResult(text, false)));
        return;
    }
    WriteLine(Error(id, -32601, "Method not found: " + method));
}

} // namespace

int main() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        luling::json::Value message;
        std::string error;
        if (!luling::json::Parse(line, message, &error) || !message.is_object()) {
            WriteLine(Error(nullptr, -32700, "Parse error: " + error));
            continue;
        }
        Handle(message);
    }
    Disconnect();
    return 0;
}
