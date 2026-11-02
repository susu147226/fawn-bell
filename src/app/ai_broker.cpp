// ai_broker.cpp - Named-pipe broker for Luling.AI.exe: permissions from
// ai.json, a user confirmation before anything changes, the five tools on
// the app's queue, and the activity log the settings page shows.
#include "ai_broker.h"

#include "ai_delete.h"
#include "ai_rename.h"
#include "ai_settings.h"
#include "ai_tools.h"
#include "ai_write.h"
#include "app_internal.h"
#include "app_runtime.h"
#include "../common/json_value.h"
#include "../ipc/ai_tool_protocol.h"
#include "../ui/confirm_dialog.h"
#include "../ops/ops_manager.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace luling {
namespace {

using app::ai::ToolOutcome;

std::atomic<bool> g_running{false};
std::thread g_thread;
AppState* g_app = nullptr;

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring full(path, n);
    const size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

std::wstring LocalAppDataDir() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) || !raw) return {};
    std::wstring path(raw);
    CoTaskMemFree(raw);
    return path;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

bool PathExists(const std::wstring& path) {
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring TimeLabel(int64_t epoch) {
    const time_t t = static_cast<time_t>(epoch);
    tm local{};
    localtime_s(&local, &t);
    wchar_t text[16] = {};
    swprintf_s(text, L"%02d:%02d", local.tm_hour, local.tm_min);
    return text;
}

int64_t NowEpoch() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void AppendActivity(const std::wstring& client, const std::string& tool, bool ok, bool declined,
                    const std::wstring& summary) {
    const std::wstring path = AiActivityLogPath();
    if (path.empty()) return;
    json::Value line = json::Value::Object();
    line.set("t", NowEpoch());
    line.set("client", json::Utf8(client));
    line.set("tool", tool);
    line.set("ok", ok);
    line.set("declined", declined);
    line.set("summary", json::Utf8(summary));
    const std::string text = json::Serialize(line) + "\n";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

std::string GroupForTool(const std::string& tool) {
    if (tool == "get_state" || tool == "list_folder" || tool == "reveal" ||
        tool == "list_tabs" || tool == "open_tab" || tool == "get_settings")
        return "view";
    if (tool == "organize_files") return "organize";
    if (tool == "rename_files") return "rename";
    if (tool == "delete_files") return "delete";
    if (tool == "copy_files" || tool == "move_files" || tool == "create_folder" ||
        tool == "undo_last")
        return "files";
    if (tool == "set_setting") return "settings";
    return {};
}

bool EnsureAiDir() {
    const std::wstring base = LocalAppDataDir();
    if (base.empty()) return false;
    const std::wstring dir = Join(Join(base, L"Luling"), L"ai");
    CreateDirectoryW(Join(base, L"Luling").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return true;
}

// ---- UI-thread calls -------------------------------------------------------

// The pipe of the connection this thread serves, so a modal card can notice
// that the client hung up while it waits. INVALID_HANDLE_VALUE on the UI thread.
thread_local HANDLE t_client_pipe = INVALID_HANDLE_VALUE;
// Set by the watch thread below, read by the confirm dialog's message loop.
std::atomic<bool> g_ui_cancelled{false};

bool PipeAlive(HANDLE pipe) {
    if (!pipe || pipe == INVALID_HANDLE_VALUE) return true;
    DWORD available = 0;
    return PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) != FALSE;
}

ToolOutcome CallUi(AppState* s, app::ai::BrokerUiCall& call) {
    if (!s || !s->hwnd) {
        ToolOutcome out;
        out.code = "no_window";
        out.message = "Luling has no window open.";
        return out;
    }
    g_ui_cancelled.store(false);
    std::atomic<bool> watching{true};
    std::thread watcher;
    if (t_client_pipe != INVALID_HANDLE_VALUE) {
        const HANDLE pipe = t_client_pipe;
        watcher = std::thread([pipe, &watching] {
            while (watching.load()) {
                Sleep(80);
                if (!PipeAlive(pipe)) {
                    g_ui_cancelled.store(true);
                    break;
                }
            }
        });
    }
    SendMessageW(s->hwnd, kAiBrokerUiMessage, 0, reinterpret_cast<LPARAM>(&call));
    watching.store(false);
    if (watcher.joinable()) watcher.join();
    return std::move(call.outcome);
}

// A confirmation the user answers on a Luling card, not a system message box.
bool Ask(AppState* s, app::ai::BrokerUiCall call) {
    call.kind = app::ai::BrokerUiCall::Kind::Confirm;
    CallUi(s, call);
    return call.confirmed;
}

bool GetSelection(AppState* s, std::vector<std::wstring>& selection, std::wstring& folder) {
    app::ai::BrokerUiCall call;
    call.kind = app::ai::BrokerUiCall::Kind::Selection;
    CallUi(s, call);
    selection = std::move(call.selection);
    folder = std::move(call.folder);
    return true;
}

app::ai::BrokerUiCall ConfirmCard(std::wstring title, std::wstring message,
                                  std::wstring confirm_text, int tone, const wchar_t* glyph,
                                  std::vector<std::wstring> items, std::wstring note,
                                  bool danger, bool allow_always = false) {
    app::ai::BrokerUiCall call;
    call.title = std::move(title);
    call.message = std::move(message);
    call.confirm_text = std::move(confirm_text);
    call.cancel_text = L"取消";
    call.tone = tone;
    call.glyph = glyph ? glyph : L"";
    call.items = std::move(items);
    call.note = std::move(note);
    call.danger = danger;
    call.secondary_allow = allow_always;
    call.default_confirm = false;
    return call;
}

// ---- permissions -----------------------------------------------------------

struct Permission {
    bool allowed = false;
    std::string code;
    std::string message;
};

Permission CheckPermission(const std::wstring& client, const std::string& tool) {
    bool malformed = false;
    app::ai::Settings st = app::ai::LoadSettings(&malformed);
    if (malformed) return {false, "settings_broken", "Luling's AI settings cannot be read."};
    if (!st.mcp_enabled)
        return {false, "disabled", "AI apps are turned off in Luling (设置 › AI 与 MCP)."};
    if (client.empty())
        return {false, "no_client", "The AI app did not say who it is."};
    const app::ai::ClientRecord* record = nullptr;
    for (const auto& c : st.clients)
        if (c.name == client) record = &c;
    if (record && record->grant == app::ai::ClientGrant::Denied)
        return {false, "denied", "This AI app was denied in Luling (设置 › AI 与 MCP)."};
    if (!record || record->grant == app::ai::ClientGrant::Ask) {
        app::ai::BrokerUiCall ask =
            ConfirmCard(L"允许 AI 应用连接？",
                        L"「" + client + L"」想连接鹿铃，并使用你允许的分组。",
                        L"允许", 2, L"\xE8D4", {},
                        L"之后可以在「设置 › AI 与 MCP › 已连接的 AI 应用」里改。", false);
        ask.cancel_text = L"拒绝";
        if (!Ask(g_app, ask)) {
            if (!record) {
                st.clients.push_back({client, app::ai::ClientGrant::Denied});
            } else {
                for (auto& c : st.clients)
                    if (c.name == client) c.grant = app::ai::ClientGrant::Denied;
            }
            (void)app::ai::SaveSettings(st);
            return {false, "denied", "The user denied this AI app."};
        }
        if (!record) {
            st.clients.push_back({client, app::ai::ClientGrant::Allowed});
        } else {
            for (auto& c : st.clients)
                if (c.name == client) c.grant = app::ai::ClientGrant::Allowed;
        }
        if (!app::ai::SaveSettings(st))
            return {false, "settings_write", "The allow decision could not be saved."};
    }
    const std::string group = GroupForTool(tool);
    if (group.empty()) return {false, "unknown_tool", "Luling has no such tool."};
    if (!app::ai::McpAllows(st, group))
        return {false, "group_off",
                "That group is turned off in Luling (设置 › AI 与 MCP › AI 应用可以做的事)."};
    return {true, {}, {}};
}

// ---- execution -------------------------------------------------------------

bool WaitForTasks(AppState* s, const std::vector<uint64_t>& tasks, bool& all_ok) {
    all_ok = true;
    for (uint64_t id : tasks)
        if (id == 0) all_ok = false;
    if (tasks.empty()) return true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    std::vector<bool> finished(tasks.size(), false);
    size_t done = 0;
    while (done < tasks.size() && std::chrono::steady_clock::now() < deadline) {
        for (size_t i = 0; i < tasks.size(); ++i) {
            if (finished[i] || tasks[i] == 0) {
                if (!finished[i]) { finished[i] = true; ++done; }
                continue;
            }
            if (const auto status = s->ops.FinalStatus(tasks[i])) {
                finished[i] = true;
                ++done;
                if (status->phase != ops::OpPhase::Completed) all_ok = false;
            }
        }
        if (done >= tasks.size()) break;
        Sleep(50);
    }
    return done >= tasks.size();
}

uint64_t NewUndoGroup() {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

ToolOutcome RunOrganize(AppState* s, const std::wstring& client, json::Value args) {
    ToolOutcome out;
    std::vector<std::wstring> selection;
    std::wstring folder;
    GetSelection(s, selection, folder);
    if (!args.find("folder") && !folder.empty()) args.set("folder", json::Utf8(folder));
    auto plan = std::make_shared<app::ai::OrganizePlan>();
    if (!app::ai::ParseOrganizePlan(args, *plan, out.code, out.message)) return out;

    std::vector<std::wstring> items;
    for (const auto& g : plan->groups) {
        if (items.size() >= 10) break;
        items.push_back(g.name + L"（" + std::to_wstring(g.items.size()) + L" 项）");
    }
    app::ai::BrokerUiCall ask = ConfirmCard(
        L"整理 " + std::to_wstring(plan->total) + L" 项？",
        L"「" + client + L"」请求把这 " + std::to_wstring(plan->total) + L" 项放进 " +
            std::to_wstring(plan->groups.size()) + L" 个文件夹：" + plan->folder,
        L"整理", 3, L"\xE8B7", std::move(items),
        L"确认后可在鹿铃里撤销。", false);
    if (!Ask(s, ask)) {
        out.code = "user_denied";
        out.message = "The user declined; nothing moved.";
        return out;
    }
    const uint64_t group = NewUndoGroup();
    std::vector<uint64_t> tasks;
    for (const auto& g : plan->groups) {
        if (g.exists) continue;
        ops::OpRequest req;
        req.type = ops::OpType::CreateFolder;
        req.sources.push_back(Join(plan->folder, g.name));
        req.undo_group = group;
        req.undo_label = L"AI 应用整理";
        tasks.push_back(s->ops.Submit(std::move(req)));
    }
    for (const auto& g : plan->groups) {
        ops::OpRequest req;
        req.type = ops::OpType::Move;
        for (const auto& name : g.items) req.sources.push_back(Join(plan->folder, name));
        req.dest_dir = Join(plan->folder, g.name);
        req.collision_policy = ops::CollisionPolicy::KeepBoth;
        req.undo_group = group;
        req.undo_label = L"AI 应用整理";
        tasks.push_back(s->ops.Submit(std::move(req)));
    }
    bool all_ok = true;
    const bool finished = WaitForTasks(s, tasks, all_ok);
    size_t moved = 0;
    for (const auto& g : plan->groups)
        for (const auto& name : g.items)
            if (!PathExists(Join(plan->folder, name))) ++moved;
    if (moved == 0) {
        out.code = finished && all_ok ? "move_failed" : "move_unfinished";
        out.message = "Nothing moved; the plan did not go through.";
        return out;
    }
    out.ok = true;
    out.data = json::Value::Object();
    out.data.set("ok", true);
    out.data.set("moved", static_cast<int64_t>(moved));
    out.data.set("created", static_cast<int64_t>(plan->created));
    out.data.set("undo_label", "AI 应用整理");
    return out;
}

ToolOutcome RunRename(AppState* s, const std::wstring& client, const json::Value& args) {
    ToolOutcome out;
    std::vector<std::wstring> selection;
    std::wstring folder;
    GetSelection(s, selection, folder);
    app::ai::RenamePlan plan;
    if (!app::ai::ParseRenamePlan(args, selection, plan, out.code, out.message)) return out;
    std::vector<std::wstring> items;
    for (const auto& row : plan.rows) {
        if (items.size() >= 10) break;
        items.push_back(row.first + L"  →  " + row.second);
    }
    app::ai::BrokerUiCall ask = ConfirmCard(
        L"重命名 " + std::to_wstring(plan.old_paths.size()) + L" 个文件？",
        L"「" + client + L"」请求按这条规则改名：" + plan.rule, L"重命名", 3, L"\xE8AC",
        std::move(items), L"扩展名默认不变；确认后可在鹿铃里撤销。", false);
    if (!Ask(s, ask)) {
        out.code = "user_denied";
        out.message = "The user declined; nothing changed.";
        return out;
    }
    ops::OpRequest req;
    req.type = ops::OpType::BatchRename;
    req.sources = plan.old_paths;
    req.new_names = plan.new_names;
    req.undo_group = NewUndoGroup();
    req.undo_label = L"AI 应用重命名";
    std::vector<uint64_t> tasks{s->ops.Submit(std::move(req))};
    bool all_ok = true;
    WaitForTasks(s, tasks, all_ok);
    size_t renamed = 0;
    for (const auto& old_path : plan.old_paths)
        if (!PathExists(old_path)) ++renamed;
    if (renamed == 0) {
        out.code = "rename_failed";
        out.message = "The renames did not go through.";
        return out;
    }
    out.ok = true;
    out.data = json::Value::Object();
    out.data.set("ok", true);
    out.data.set("renamed", static_cast<int64_t>(renamed));
    out.data.set("undo_label", "AI 应用重命名");
    return out;
}

ToolOutcome RunDelete(AppState* s, const std::wstring& client, const json::Value& args) {
    ToolOutcome out;
    std::vector<std::wstring> selection;
    std::wstring folder;
    GetSelection(s, selection, folder);
    app::ai::DeletePlan plan;
    if (!app::ai::ParseDeletePlan(args, selection, plan, out.code, out.message)) return out;
    const std::wstring count = std::to_wstring(plan.paths.size());
    std::vector<std::wstring> items;
    for (const auto& name : plan.names) {
        if (items.size() >= 10) break;
        items.push_back(name);
    }
    if (plan.permanent) {
        app::ai::BrokerUiCall first = ConfirmCard(
            L"永久删除 " + count + L" 项？",
            L"「" + client + L"」请求永久删除。不会进回收站，之后找不回来。", L"永久删除", 4,
            L"\xE74D", items, L"删除后无法恢复。", true);
        if (!Ask(s, first)) {
            out.code = "user_denied";
            out.message = "The user declined.";
            return out;
        }
        app::ai::BrokerUiCall second = ConfirmCard(
            L"再确认一次", L"永久删除 " + count + L" 项，无法恢复。", L"仍要删除", 4,
            L"\xE74D", {}, L"这是最后一次确认。", true);
        if (!Ask(s, second)) {
            out.code = "user_denied";
            out.message = "The user declined the second confirmation.";
            return out;
        }
    } else {
        app::ai::BrokerUiCall ask = ConfirmCard(
            L"移到回收站？", L"「" + client + L"」请求删除 " + count + L" 项。", L"移到回收站", 3,
            L"\xE74D", std::move(items),
            L"回收站里还能找回；确认后可在鹿铃里撤销。", false);
        if (!Ask(s, ask)) {
            out.code = "user_denied";
            out.message = "The user declined.";
            return out;
        }
    }
    ops::OpRequest req;
    req.type = plan.permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
    req.sources = plan.paths;
    if (!plan.permanent) {
        req.undo_group = NewUndoGroup();
        req.undo_label = L"AI 应用删除";
    }
    std::vector<uint64_t> tasks{s->ops.Submit(std::move(req))};
    bool all_ok = true;
    WaitForTasks(s, tasks, all_ok);
    size_t gone = 0;
    for (const auto& path : plan.paths)
        if (!PathExists(path)) ++gone;
    if (gone == 0) {
        out.code = "delete_failed";
        out.message = "Nothing was deleted.";
        return out;
    }
    out.ok = true;
    out.data = json::Value::Object();
    out.data.set("ok", true);
    out.data.set("deleted", static_cast<int64_t>(gone));
    out.data.set("permanent", plan.permanent);
    if (!plan.permanent) out.data.set("undo_label", "AI 应用删除");
    return out;
}

// ---- allow rules, undo bookkeeping and online clients ----------------------

std::mutex g_state_mutex;
// The newest undoable step, per client: two connected apps must not undo
// each other's work.
std::map<std::wstring, std::pair<uint64_t, std::wstring>> g_last;
std::set<std::wstring> g_online;
// One call runs at a time; connections themselves are served in parallel.
std::mutex g_exec_mutex;

void NoteUndoGroup(const std::wstring& client, uint64_t group, std::wstring label) {
    if (!group) return;
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_last[client] = {group, std::move(label)};
}

std::wstring FoldPath(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'/') ch = L'\\';
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

bool FolderCovered(const std::wstring& rule_folder, const std::wstring& folder) {
    if (rule_folder.empty()) return true;
    const std::wstring rule = FoldPath(rule_folder);
    const std::wstring path = FoldPath(folder);
    if (path == rule) return true;
    return path.size() > rule.size() && path.compare(0, rule.size(), rule) == 0 &&
           (path[rule.size()] == L'\\' || rule.back() == L'\\');
}

bool RuleAllows(const app::ai::Settings& st, const std::wstring& client, const std::string& tool,
                const std::wstring& folder) {
    for (const auto& rule : st.allow)
        if (rule.client == client && rule.tool == tool && FolderCovered(rule.folder, folder))
            return true;
    return false;
}

void AddAllowRule(app::ai::Settings& st, const std::wstring& client, const std::string& tool,
                  const std::wstring& folder) {
    for (auto& rule : st.allow) {
        if (rule.client == client && rule.tool == tool && rule.folder == folder) return;
    }
    st.allow.push_back({client, tool, folder});
}

std::wstring ParentOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 2 && path.size() > 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, slash);
}

bool IsDirectory(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// ---- new tools -------------------------------------------------------------

ToolOutcome RunNavigate(AppState* s, const std::string& tool, const json::Value& args) {
    ToolOutcome out;
    std::wstring path = json::Wide(args.get_string("path"));
    if (path.empty()) {
        out.code = "no_path";
        out.message = "No path was given.";
        return out;
    }
    if (!app::ai::ResolveFolderPath(path, path)) {
        out.code = "bad_path";
        out.message = "That is not an absolute path or a known folder.";
        return out;
    }
    app::ai::BrokerUiCall call;
    call.kind = app::ai::BrokerUiCall::Kind::Navigate;
    call.path = path;
    call.reveal = tool == "reveal";
    call.new_tab = args.get_bool("new_tab", false);
    return CallUi(s, call);
}

ToolOutcome RunListTabs(AppState* s) {
    app::ai::BrokerUiCall call;
    call.kind = app::ai::BrokerUiCall::Kind::ListTabs;
    return CallUi(s, call);
}

ToolOutcome RunGetSettings(AppState* s) {
    app::ai::BrokerUiCall call;
    call.kind = app::ai::BrokerUiCall::Kind::GetSettings;
    return CallUi(s, call);
}

ToolOutcome RunSetSetting(AppState* s, const std::wstring& client, const json::Value& args) {
    ToolOutcome out;
    const std::wstring key = json::Wide(args.get_string("key"));
    const std::wstring value = json::Wide(args.get_string("value"));
    if (key.empty()) {
        out.code = "no_key";
        out.message = "No setting key was given.";
        return out;
    }
    app::ai::BrokerUiCall ask = ConfirmCard(
        L"修改设置？", L"「" + client + L"」请求把「" + key + L"」改为「" + value + L"」。",
        L"修改", 3, L"\xE713", {}, L"改完立即生效，可在设置里改回来。", false);
    if (!Ask(s, ask)) {
        out.code = "user_denied";
        out.message = "The user declined.";
        return out;
    }
    app::ai::BrokerUiCall call;
    call.kind = app::ai::BrokerUiCall::Kind::SetSetting;
    call.key = key;
    call.value = value;
    return CallUi(s, call);
}

ToolOutcome RunTransfer(AppState* s, const std::wstring& client, const json::Value& args, bool move) {
    ToolOutcome out;
    const json::Value* paths = args.find("paths");
    if (!paths || !paths->is_array() || paths->size() == 0) {
        out.code = "no_paths";
        out.message = "No paths were given.";
        return out;
    }
    std::wstring dest = json::Wide(args.get_string("dest"));
    if (dest.empty() || !app::ai::ResolveFolderPath(dest, dest) || !IsDirectory(dest)) {
        out.code = "bad_dest";
        out.message = "The destination is not an existing absolute folder.";
        return out;
    }
    std::vector<std::wstring> sources;
    for (const auto& item : paths->items()) {
        std::wstring path = json::Wide(item.as_string());
        if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            out.code = "missing_item";
            out.message = "One of the paths does not exist.";
            return out;
        }
        sources.push_back(std::move(path));
    }
    const std::string tool = move ? "move_files" : "copy_files";
    bool malformed = false;
    app::ai::Settings st = app::ai::LoadSettings(&malformed);
    const bool covered = !malformed && !move && RuleAllows(st, client, tool, dest);
    if (!covered) {
        std::vector<std::wstring> items;
        for (const auto& path : sources) {
            if (items.size() >= 10) break;
            const size_t slash = path.find_last_of(L"\\/");
            items.push_back(slash == std::wstring::npos ? path : path.substr(slash + 1));
        }
        app::ai::BrokerUiCall ask = ConfirmCard(
            move ? L"移动 " + std::to_wstring(sources.size()) + L" 项？"
                 : L"复制 " + std::to_wstring(sources.size()) + L" 项？",
            L"「" + client + L"」请求" + (move ? L"移动" : L"复制") + L"到：" + dest,
            move ? L"移动" : L"复制", 3, L"\xE8DE", std::move(items),
            L"确认后可在鹿铃里撤销。", false, !move);
        if (!Ask(s, ask)) {
            out.code = "user_denied";
            out.message = "The user declined.";
            return out;
        }
        if (ask.always && !malformed) {
            AddAllowRule(st, client, tool, dest);
            (void)app::ai::SaveSettings(st);
        }
    }
    ops::OpRequest req;
    req.type = move ? ops::OpType::Move : ops::OpType::Copy;
    req.sources = sources;
    req.dest_dir = dest;
    req.collision_policy = ops::CollisionPolicy::KeepBoth;
    req.undo_group = NewUndoGroup();
    req.undo_label = move ? L"AI 应用移动" : L"AI 应用复制";
    NoteUndoGroup(client, req.undo_group, req.undo_label);
    std::vector<uint64_t> tasks{s->ops.Submit(std::move(req))};
    bool all_ok = true;
    WaitForTasks(s, tasks, all_ok);
    out.ok = all_ok;
    out.data = json::Value::Object();
    out.data.set("ok", all_ok);
    out.data.set("count", static_cast<int64_t>(sources.size()));
    out.data.set("dest", json::Utf8(dest));
    out.data.set("undo_label", move ? "AI 应用移动" : "AI 应用复制");
    if (!all_ok) {
        out.code = "transfer_partial";
        out.message = "Some items did not go through; check the folder.";
    }
    return out;
}

ToolOutcome RunCreateFolder(AppState* s, const std::wstring& client, const json::Value& args) {
    ToolOutcome out;
    std::wstring parent = json::Wide(args.get_string("parent"));
    std::wstring name = json::Wide(args.get_string("name"));
    if (parent.empty() || !app::ai::ResolveFolderPath(parent, parent) || !IsDirectory(parent)) {
        out.code = "bad_parent";
        out.message = "The parent is not an existing absolute folder.";
        return out;
    }
    if (name.empty() || name == L"." || name == L".." || name.size() > 200 ||
        name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        out.code = "bad_name";
        out.message = "The folder name has a character a name cannot hold.";
        return out;
    }
    const std::wstring target = Join(parent, name);
    bool malformed = false;
    app::ai::Settings st = app::ai::LoadSettings(&malformed);
    const bool covered = !malformed && RuleAllows(st, client, "create_folder", parent);
    if (!covered) {
        app::ai::BrokerUiCall ask = ConfirmCard(
            L"新建文件夹？", L"「" + client + L"」请求在 " + parent + L" 里新建「" + name + L"」。",
            L"新建", 3, L"\xE8B7", {}, L"确认后可在鹿铃里撤销。", false, true);
        if (!Ask(s, ask)) {
            out.code = "user_denied";
            out.message = "The user declined.";
            return out;
        }
        if (ask.always && !malformed) {
            AddAllowRule(st, client, "create_folder", parent);
            (void)app::ai::SaveSettings(st);
        }
    }
    ops::OpRequest req;
    req.type = ops::OpType::CreateFolder;
    req.sources.push_back(target);
    req.undo_group = NewUndoGroup();
    req.undo_label = L"AI 应用新建文件夹";
    NoteUndoGroup(client, req.undo_group, req.undo_label);
    std::vector<uint64_t> tasks{s->ops.Submit(std::move(req))};
    bool all_ok = true;
    WaitForTasks(s, tasks, all_ok);
    out.ok = all_ok && IsDirectory(target);
    out.data = json::Value::Object();
    out.data.set("ok", out.ok);
    out.data.set("path", json::Utf8(target));
    if (!out.ok) {
        out.code = "create_failed";
        out.message = "The folder was not created.";
    }
    return out;
}

ToolOutcome RunUndo(AppState* s, const std::wstring& client) {
    ToolOutcome out;
    uint64_t group = 0;
    std::wstring label;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const auto it = g_last.find(client);
        if (it != g_last.end()) {
            group = it->second.first;
            label = it->second.second;
        }
    }
    if (!group) {
        out.code = "nothing_to_undo";
        out.message = "Luling has no AI file action to undo yet.";
        return out;
    }
    app::ai::BrokerUiCall ask = ConfirmCard(
        L"撤销上一步？", L"「" + client + L"」请求撤销：" + (label.empty() ? L"上一步" : label) + L"。",
        L"撤销", 3, L"\xE7A7", {}, L"只有刚做完的那一步能撤销。", false);
    if (!Ask(s, ask)) {
        out.code = "user_denied";
        out.message = "The user declined.";
        return out;
    }
    const ops::OpsManager::GroupUndo result = s->ops.UndoGroup(group);
    if (result == ops::OpsManager::GroupUndo::NotOnTop) {
        out.code = "not_newest";
        out.message = "That step is no longer the newest; undo it in Luling first.";
        return out;
    }
    if (result == ops::OpsManager::GroupUndo::Pending) {
        out.code = "still_running";
        out.message = "The step is still running; ask again when it finishes.";
        return out;
    }
    if (result == ops::OpsManager::GroupUndo::Unavailable) {
        out.code = "not_undoable";
        out.message = "That step cannot be undone.";
        return out;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (s->ops.Status().active && std::chrono::steady_clock::now() < deadline) Sleep(50);
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_last.erase(client);
    }
    out.ok = true;
    out.data = json::Value::Object();
    out.data.set("ok", true);
    out.data.set("undone", json::Utf8(label));
    return out;
}

ToolOutcome Execute(AppState* s, const std::wstring& client, const std::string& tool,
                    const json::Value& args) {
    if (tool == "get_state") {
        app::ai::BrokerUiCall call;
        call.kind = app::ai::BrokerUiCall::Kind::ReadState;
        call.tool = tool;
        call.args = args;
        return CallUi(s, call);
    }
    if (tool == "list_folder") {
        std::atomic<bool> cancelled{false};
        return app::ai::RunToolOnWorker(tool, args, cancelled);
    }
    if (tool == "reveal" || tool == "open_tab") return RunNavigate(s, tool, args);
    if (tool == "list_tabs") return RunListTabs(s);
    if (tool == "get_settings") return RunGetSettings(s);
    if (tool == "set_setting") return RunSetSetting(s, client, args);
    if (tool == "copy_files") return RunTransfer(s, client, args, false);
    if (tool == "move_files") return RunTransfer(s, client, args, true);
    if (tool == "create_folder") return RunCreateFolder(s, client, args);
    if (tool == "undo_last") return RunUndo(s, client);
    if (tool == "organize_files") return RunOrganize(s, client, args);
    if (tool == "rename_files") return RunRename(s, client, args);
    if (tool == "delete_files") return RunDelete(s, client, args);
    ToolOutcome out;
    out.code = "unknown_tool";
    out.message = "Luling has no such tool.";
    return out;
}

std::wstring SummaryFor(const std::string& tool, const ToolOutcome& out, const json::Value& args) {
    if (!out.ok) return json::Wide(out.message);
    if (tool == "get_state") {
        std::wstring folder = json::Wide(out.data.get_string("folder"));
        if (!folder.empty()) return folder;
    }
    if (tool == "list_folder") {
        const std::wstring path = json::Wide(args.get_string("path"));
        auto count_of = [&](const char* key) {
            int64_t n = 0;
            const json::Value* v = out.data.find(key);
            return v && v->get_int(n, 0, 1000000) ? n : int64_t{0};
        };
        return path + L" · " + std::to_wstring(count_of("folders") + count_of("files")) + L" 项";
    }
    auto number_of = [&](const char* key) {
        int64_t n = 0;
        const json::Value* v = out.data.find(key);
        return v && v->get_int(n, 0, 1000000) ? n : int64_t{-1};
    };
    if (const int64_t n = number_of("moved"); n >= 0) return L"移动 " + std::to_wstring(n) + L" 项";
    if (const int64_t n = number_of("renamed"); n >= 0) return L"重命名 " + std::to_wstring(n) + L" 项";
    if (const int64_t n = number_of("deleted"); n >= 0) return L"删除 " + std::to_wstring(n) + L" 项";
    return {};
}

json::Value ResponseFor(const json::Value& request, const std::wstring& client) {
    const std::string op = request.get_string("op");
    json::Value response = json::Value::Object();
    if (const json::Value* id = request.find("id")) {
        json::Value copy = *id;
        response.set("id", std::move(copy));
    }
    if (op == "hello") {
        response.set("ok", true);
        response.set("protocol", static_cast<int64_t>(ipc::ai::kProtocolVersion));
        response.set("host", "luling");
        return response;
    }
    if (op == "tools") {
        json::Value tools = app::ai::Tools();
        response.set("ok", true);
        response.set("tools", std::move(tools));
        return response;
    }
    if (op != "call") {
        response.set("ok", false);
        response.set("code", "bad_op");
        response.set("message", "Unknown request.");
        return response;
    }
    const std::string tool = request.get_string("tool");
    json::Value args = json::Value::Object();
    if (const json::Value* a = request.find("arguments"); a && a->is_object()) args = *a;
    const Permission perm = CheckPermission(client, tool);
    if (!perm.allowed) {
        response.set("ok", false);
        response.set("code", perm.code);
        response.set("message", perm.message);
        AppendActivity(client, tool, false, perm.code == "denied", json::Wide(perm.message));
        return response;
    }
    ToolOutcome out;
    {
        std::lock_guard<std::mutex> lock(g_exec_mutex);
        out = Execute(g_app, client, tool, args);
    }
    AppendActivity(client, tool, out.ok, out.code == "user_denied", SummaryFor(tool, out, args));
    if (!out.ok) {
        response.set("ok", false);
        response.set("code", out.code);
        response.set("message", out.message);
        return response;
    }
    response.set("ok", true);
    response.set("result", out.data);
    return response;
}

void HandleConnection(HANDLE pipe) {
    t_client_pipe = pipe;
    std::wstring client = L"AI 应用";
    std::string frame;
    while (ipc::ai::ReadFrame(pipe, frame)) {
        json::Value request;
        if (!json::Parse(frame, request) || !request.is_object()) {
            json::Value bad = json::Value::Object();
            bad.set("ok", false);
            bad.set("code", "bad_json");
            bad.set("message", "The request is not a JSON object.");
            (void)ipc::ai::WriteFrame(pipe, json::Serialize(bad));
            continue;
        }
        if (request.get_string("op") == "hello") {
            const std::wstring name = json::Wide(request.get_string("client"));
            if (!name.empty() && name.size() <= 64) {
                client = name;
                std::lock_guard<std::mutex> lock(g_state_mutex);
                g_online.insert(client);
            }
        }
        const json::Value response = ResponseFor(request, client);
        if (!ipc::ai::WriteFrame(pipe, json::Serialize(response))) break;
        if (request.get_string("op") == "bye") break;
    }
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_online.erase(client);
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

void ServeLoop() {
    g_running = true;
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(
            ipc::ai::kPipeName, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(1000);
            continue;
        }
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            continue;
        }
        // Each connection gets its own thread, so a second AI app (or the
        // first one reconnecting) is served while another is connected.
        std::thread(HandleConnection, pipe).detach();
    }
    g_running = false;
}

} // namespace

LRESULT AiBrokerHandleUiCall(AppState& s, app::ai::BrokerUiCall* call) {
    if (!call) return 0;
    // 多窗口: the broker's calls act on the window the user is in, not on
    // whichever window happened to host the pipe server. Forwarding here keeps
    // the registry read on the UI thread.
    if (AppState* recent = s.core.windows.MostRecent();
        recent && recent != &s && recent->hwnd) {
        return SendMessageW(recent->hwnd, kAiBrokerUiMessage, 0,
                            reinterpret_cast<LPARAM>(call));
    }
    using Kind = app::ai::BrokerUiCall::Kind;
    switch (call->kind) {
    case Kind::ReadState:
        call->outcome = app::ai::RunToolOnUi(s, call->tool, call->args);
        break;
    case Kind::Selection: {
        call->selection.clear();
        app::Tab* tab = ActiveTab(s);
        if (!tab) break;
        call->folder = tab->current_path;
        if (!tab->snapshot) break;
        if (tab->all_selected) {
            for (size_t i = 0; i < tab->snapshot->size() && i < 500; ++i)
                call->selection.push_back(Join(tab->current_path, (*tab->snapshot)[i].name));
            break;
        }
        for (int index : tab->selected)
            if (index >= 0 && index < static_cast<int>(tab->snapshot->size()))
                call->selection.push_back(Join(tab->current_path, (*tab->snapshot)[static_cast<size_t>(index)].name));
        if (call->selection.empty() && tab->selected_index >= 0 &&
            tab->selected_index < static_cast<int>(tab->snapshot->size()))
            call->selection.push_back(
                Join(tab->current_path, (*tab->snapshot)[static_cast<size_t>(tab->selected_index)].name));
        break;
    }
    case Kind::Folder: {
        app::Tab* tab = ActiveTab(s);
        if (tab) call->folder = tab->current_path;
        break;
    }
    case Kind::Navigate: {
        std::wstring target = call->path;
        if (call->reveal && !target.empty() && !IsDirectory(target)) target = ParentOf(target);
        if (target.empty()) {
            call->outcome.code = "bad_path";
            call->outcome.message = "There is no such folder.";
            break;
        }
        call->outcome.ok = true;
        call->outcome.data = json::Value::Object();
        call->outcome.data.set("ok", true);
        call->outcome.data.set("path", json::Utf8(target));
        if (call->reveal || !call->new_tab) NavigateTo(s, target);
        else NewTab(s, target);
        break;
    }
    case Kind::ListTabs: {
        call->outcome.ok = true;
        call->outcome.data = json::Value::Object();
        json::Value tabs = json::Value::Array();
        for (size_t i = 0; i < s.window_tabs.items.size(); ++i) {
            const auto& tab = s.window_tabs.items[i];
            if (!tab) continue;
            const app::Tab* folder = tab->ActiveFolder();
            const std::wstring path = folder ? folder->current_path : std::wstring();
            std::wstring title = tab->title;
            if (title.empty() && !path.empty()) {
                const size_t slash = path.find_last_of(L"\\/");
                title = slash == std::wstring::npos ? path : path.substr(slash + 1);
            }
            json::Value row = json::Value::Object();
            row.set("index", static_cast<int64_t>(i));
            row.set("path", json::Utf8(path));
            row.set("title", json::Utf8(title));
            row.set("active", i == s.window_tabs.active);
            tabs.push(std::move(row));
        }
        call->outcome.data.set("tabs", std::move(tabs));
        break;
    }
    case Kind::GetSettings: {
        call->outcome.ok = true;
        call->outcome.data = json::Value::Object();
        const char* theme = s.appPrefs.theme_mode == 1 ? "light" : s.appPrefs.theme_mode == 2 ? "dark" : "system";
        call->outcome.data.set("theme", theme);
        const int row = s.appPrefs.row_height;
        call->outcome.data.set("row_height", row <= 28 ? "compact" : row >= 40 ? "roomy" : "standard");
        call->outcome.data.set("language", json::Utf8(s.appPrefs.language));
        call->outcome.data.set("show_hidden", s.appPrefs.show_hidden_files);
        call->outcome.data.set("show_protected", s.appPrefs.show_protected_os_files);
        call->outcome.data.set("show_performance", s.appPrefs.show_status_performance);
        call->outcome.data.set("list_smart_date", s.appPrefs.list_smart_date);
        call->outcome.data.set("list_zebra_rows", s.appPrefs.list_zebra_rows);
        call->outcome.data.set("list_size_bar", s.appPrefs.list_size_bar);
        break;
    }
    case Kind::SetSetting: {
        const std::wstring key = call->key;
        const std::wstring value = call->value;
        auto true_value = [&] {
            return value == L"true" || value == L"1" || value == L"on" || value == L"yes";
        };
        bool applied = false;
        if (key == L"theme") {
            int mode = value == L"light" ? 1 : value == L"dark" ? 2 : 0;
            SetThemeMode(s, mode);
            applied = true;
        } else if (key == L"row_height") {
            const int index = value == L"compact" ? 0 : value == L"roomy" ? 2 : 1;
            s.settings.RowHeight(index);
            applied = true;
        } else if (key == L"language") {
            s.settings.Language(value);
            applied = true;
        } else {
            int toggle = -1;
            bool current = false;
            if (key == L"show_hidden") { toggle = 5; current = s.appPrefs.show_hidden_files; }
            else if (key == L"show_protected") { toggle = 16; current = s.appPrefs.show_protected_os_files; }
            else if (key == L"show_performance") { toggle = 4; current = s.appPrefs.show_status_performance; }
            else if (key == L"list_smart_date") { toggle = 17; current = s.appPrefs.list_smart_date; }
            else if (key == L"list_zebra_rows") { toggle = 18; current = s.appPrefs.list_zebra_rows; }
            else if (key == L"list_size_bar") { toggle = 19; current = s.appPrefs.list_size_bar; }
            if (toggle >= 0 && true_value() != current) s.settings.ToggleUi(toggle);
            applied = toggle >= 0;
        }
        call->outcome.ok = applied;
        call->outcome.data = json::Value::Object();
        call->outcome.data.set("ok", applied);
        call->outcome.data.set("key", json::Utf8(key));
        call->outcome.data.set("value", json::Utf8(value));
        if (!applied) {
            call->outcome.code = "unknown_setting";
            call->outcome.message = "Luling does not have that setting.";
        }
        break;
    }
    case Kind::Confirm: {
        ui::ConfirmDialogSpec spec;
        spec.title = call->title.empty() ? L"鹿铃 · AI 应用" : call->title;
        spec.message = call->message.empty() ? call->prompt : call->message;
        spec.confirm_text = call->confirm_text.empty() ? L"允许" : call->confirm_text;
        spec.cancel_text = call->cancel_text.empty() ? L"取消" : call->cancel_text;
        spec.items = call->items;
        spec.item_glyph = call->glyph;
        spec.note = call->note;
        spec.danger = call->danger;
        spec.tone = static_cast<ui::ConfirmTone>(std::clamp(call->tone, 0, 4));
        if (call->secondary_allow) spec.secondary_text = L"始终允许";
        spec.default_choice =
            call->default_confirm ? ui::ConfirmChoice::Confirm : ui::ConfirmChoice::Cancel;
        spec.cancel = &g_ui_cancelled;
        const ui::ConfirmChoice choice =
            ui::ShowConfirmDialogEx(s.hwnd, spec, s.darkMode, s.accentColor);
        call->always = choice == ui::ConfirmChoice::Secondary;
        call->confirmed = choice == ui::ConfirmChoice::Confirm || call->always;
        break;
    }
    }
    return 0;
}

void AiBrokerStart(AppState& s) {
    if (g_running) return;
    g_app = &s;
    if (g_thread.joinable()) g_thread.join();
    g_thread = std::thread([] { ServeLoop(); });
    g_thread.detach();
}

bool AiBrokerListening() { return g_running.load(); }

bool AiBrokerIsOnline(const std::wstring& client) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_online.count(client) != 0;
}

std::wstring AiHostPath() {
    const std::wstring dir = ModuleDir();
    if (dir.empty()) return {};
    const std::wstring path = Join(dir, L"Luling.AI.exe");
    return PathExists(path) ? path : std::wstring();
}

std::wstring AiActivityLogPath() {
    const std::wstring base = LocalAppDataDir();
    if (base.empty()) return {};
    EnsureAiDir();
    return Join(Join(Join(base, L"Luling"), L"ai"), L"activity.log");
}

std::vector<app::ai::ActivityEntry> AiReadActivity(size_t limit, size_t& more) {
    more = 0;
    std::vector<app::ai::ActivityEntry> entries;
    const std::wstring path = AiActivityLogPath();
    if (path.empty()) return entries;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return entries;
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 8 * 1024 * 1024) {
        text.assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        (void)ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
        text.resize(read);
    }
    CloseHandle(file);

    bool malformed = false;
    const app::ai::Settings st = app::ai::LoadSettings(&malformed);
    const int64_t oldest = NowEpoch() - static_cast<int64_t>(st.log_days > 0 ? st.log_days : 30) * 86400;
    size_t at = text.find_last_of('\n');
    if (at == std::string::npos) at = text.size(); else text.resize(at);
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        json::Value line;
        if (!json::Parse(*it, line) || !line.is_object()) continue;
        int64_t t = 0;
        const json::Value* stamp = line.find("t");
        if (!stamp || !stamp->get_int(t, 0, 0x7FFFFFFFFFFFFFFF) || t < oldest) continue;
        if (entries.size() >= limit) {
            ++more;
            continue;
        }
        app::ai::ActivityEntry entry;
        entry.time = TimeLabel(t);
        entry.client = json::Wide(line.get_string("client"));
        entry.tool = app::ai::ToolTitle(line.get_string("tool"));
        entry.summary = json::Wide(line.get_string("summary"));
        entry.failed = !line.get_bool("ok", true);
        entry.declined = line.get_bool("declined", false);
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::string AiMcpConfigJson() {
    json::Value root = json::Value::Object();
    json::Value servers = json::Value::Object();
    json::Value luling = json::Value::Object();
    const std::wstring host = AiHostPath();
    luling.set("command", json::Utf8(host));
    luling.set("args", json::Value::Array());
    servers.set("luling", std::move(luling));
    root.set("mcpServers", std::move(servers));
    return json::Serialize(root);
}

bool AiCopyMcpConfig(AppState& s) {
    const std::string text = AiMcpConfigJson();
    if (text.empty() || !s.hwnd) return false;
    const std::wstring wide = json::Wide(text);
    if (!OpenClipboard(s.hwnd)) return false;
    EmptyClipboard();
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    if (void* target = GlobalLock(memory)) {
        memcpy(target, wide.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    } else {
        GlobalFree(memory);
    }
    CloseClipboard();
    return true;
}

void AiOpenActivityLog(AppState& s) {
    const std::wstring path = AiActivityLogPath();
    if (path.empty()) return;
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    ShellExecuteW(s.hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace luling
