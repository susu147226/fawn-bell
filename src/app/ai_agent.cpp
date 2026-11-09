// ai_agent.cpp - 鹿铃助手's turn driver (ai_agent.h).
// Ported from the earlier implementation app/ai_assistant_agent.cpp: the same shape, with
// the read-only tools of ai_tools.h and no file-changing ones yet.
#include "ai_agent.h"

#include "ai_assistant.h"
#include "ai_assistant_conn.h"
#include "ai_delete.h"
#include "ai_model_key.h"
#include "ai_model_stream.h"
#include "ai_peek.h"
#include "ai_rename.h"
#include "ai_tools.h"
#include "ai_write.h"
#include "app_internal.h"
#include "../ops/ops_manager.h"
#include "../common/json_value.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace luling {

struct AiAgent {
    json::Value history = json::Value::Array();   // user / assistant / tool messages
    std::shared_ptr<app::ai::ModelStream> stream;
    uint64_t generation = 0;                      // bumped by stop: late events drop out
    std::string reasoning, content;
    uint32_t think_id = 0, text_id = 0;
    std::wstring last_prompt;
    bool busy = false;

    // The tool round: calls the model asked for, the one running now.
    std::vector<app::ai::ModelToolCall> calls;
    size_t call_index = 0;
    int rounds = 0;
    uint32_t steps_id = 0;
    struct Job {
        std::atomic<bool> done{false};
        std::atomic<bool> cancelled{false};
        app::ai::ToolOutcome out;
    };
    std::shared_ptr<Job> job;

    // organize_files through the app's operation queue (one undo group).
    struct OrganizeJob {
        std::shared_ptr<app::ai::OrganizePlan> plan;
        std::vector<uint64_t> tasks;
        uint64_t group = 0;
        size_t finished = 0;
        size_t failed = 0;
        uint32_t step_block = 0;
    };
    std::shared_ptr<OrganizeJob> organ;

    // rename_files: the card that waits, and the queue doing it.
    bool awaiting_rename = false;
    std::shared_ptr<app::ai::RenamePlan> rename;
    std::string rename_call_id;
    uint32_t rename_block = 0;
    struct RenameJob {
        std::vector<uint64_t> tasks;
        uint64_t group = 0;
        size_t finished = 0;
        size_t failed = 0;
        size_t total = 0;
        uint32_t step_block = 0;
    };
    std::shared_ptr<RenameJob> rename_job;

    // delete_files: the card that waits, and the queue doing it.
    bool awaiting_delete = false;
    std::shared_ptr<app::ai::DeletePlan> del;
    std::string delete_call_id;
    uint32_t delete_block = 0;
    struct DeleteJob {
        std::vector<uint64_t> tasks;
        uint64_t group = 0;
        size_t finished = 0;
        size_t failed = 0;
        size_t total = 0;
        uint32_t step_block = 0;
        bool permanent = false;
    };
    std::shared_ptr<DeleteJob> delete_job;

    // organize_files: the card that waits for the user.
    bool awaiting = false;
    std::shared_ptr<app::ai::OrganizePlan> plan;
    std::string plan_call_id;
    uint32_t plan_block = 0;
    uint32_t confirm_block = 0;
    uint64_t undo_group = 0;   // the ops group the result card's 撤销 uses
    uint32_t undo_block = 0;
    std::vector<std::wstring> undo_paths;   // what should be back after 撤销
    bool undo_wait = false;    // an undo is running
    uint64_t undo_started = 0;
};

namespace {

constexpr UINT_PTR kPumpTimer = 0x4147;   // 'AG'
constexpr UINT kPumpMs = 40;
constexpr int kMaxRounds = 8;
constexpr size_t kMaxToolText = 20000;

std::wstring Text(const wchar_t* s) { return s; }

std::wstring EffortName(int effort) {
    static constexpr const wchar_t* kNames[] = {L"关闭", L"低", L"中", L"高"};
    return kNames[std::clamp(effort, 0, 3)];
}

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

uint64_t Now() { return ui::motion::NowMs(); }

AiAgent& Agent(AppState& s) {
    auto& slot = s.aiAssistant.agent;
    if (!slot) slot = std::make_shared<AiAgent>();
    return *slot;
}

std::wstring FormatSize(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    wchar_t buf[32];
    if (u == 0 || v >= 100.0) swprintf_s(buf, L"%.0f %s", v, units[u]);
    else swprintf_s(buf, L"%.1f %s", v, units[u]);
    return buf;
}

void SetChip(AppState& s, ui::StatusAiState state, const wchar_t* text, float progress = -1.0f) {
    s.aiAssistant.chip = state;
    s.aiAssistant.chip_text = text ? text : L"";
    s.aiAssistant.chip_progress = progress;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (!dir.empty() && dir.back() == L'\\') return dir + name;
    return dir + L"\\" + name;
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The system prompt: the same voice as the app's, with the tools this version
// really has.
std::string SystemPrompt() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char date[32];
    sprintf_s(date, "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
    std::string p =
        "You are 鹿铃助手 (Luling Assistant), built into 鹿铃 (Luling), a file manager for Windows. "
        "Today is ";
    p += date;
    p += ".\n"
         "You help the user understand files. You can look, but this version cannot change files.\n"
         "Rules:\n"
         "- When the request is about this folder, the selection or what is open, call get_state first.\n"
         "- To answer anything about a folder's contents, call list_folder once. Folders map to drives "
         "(D:), known folders (Downloads, Desktop, Documents, Pictures, Videos, Music, or 下载 桌面 文档 "
         "图片 视频 音乐) or absolute paths; without a path it lists the folder open in the window.\n"
         "- Base every statement about files on tool results; never invent files, sizes or folders. "
         "The step card already shows paths and counts, so do not repeat long lists.\n"
         "- You cannot move, rename, delete, create or open files in this version. Say so plainly and "
         "tell the user to do it in the window.\n"
         "- Reply in the user's language and keep it brief. Format with short paragraphs, lists whose "
         "lines start with \"- \", and **bold**. No headings, tables, code blocks or links.";
    return p;
}

std::wstring Display(const std::string& utf8) {
    const std::wstring in = json::Wide(utf8);
    std::wstring out;
    out.reserve(in.size());
    size_t start = 0;
    while (start <= in.size()) {
        size_t end = in.find(L'\n', start);
        if (end == std::wstring::npos) end = in.size();
        std::wstring line = in.substr(start, end - start);
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == L' ' || line[lead] == L'\t')) ++lead;
        std::wstring body = line.substr(lead);
        if (!body.empty() && body[0] == L'#') {
            size_t h = 0;
            while (h < body.size() && body[h] == L'#') ++h;
            while (h < body.size() && body[h] == L' ') ++h;
            body = body.substr(h);
            if (!body.empty() && body.find(L"**") == std::wstring::npos) body = L"**" + body + L"**";
            line = body;
        } else if (body.size() >= 2 && (body[0] == L'*' || body[0] == L'+' || body[0] == L'-') &&
                   body[1] == L' ') {
            line = L"- " + body.substr(2);
        } else if (body.rfind(L"```", 0) == 0) {
            line.clear();
        }
        line.erase(std::remove(line.begin(), line.end(), L'`'), line.end());
        out += line;
        if (end >= in.size()) break;
        out += L'\n';
        start = end + 1;
    }
    while (!out.empty() && (out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    return out;
}

ui::AiBlockView& Add(AppState& s, ui::AiBlockKind kind) {
    AiAssistantState& a = s.aiAssistant;
    ui::AiBlockView block;
    block.kind = kind;
    block.id = a.next_block_id++;
    block.born_ms = Now();
    a.blocks.push_back(std::move(block));
    return a.blocks.back();
}

ui::AiBlockView* Find(AppState& s, uint32_t id) {
    if (!id) return nullptr;
    for (ui::AiBlockView& b : s.aiAssistant.blocks)
        if (b.id == id) return &b;
    return nullptr;
}

std::wstring ServiceName() {
    return AiAgentBaseUrl() == AiAgentDefaultBaseUrl() ? std::wstring(L"DeepSeek") : AiAgentHost();
}

// A group's tile: the kind of files it holds.
void GroupLook(const std::vector<std::wstring>& items, ui::AiIcon& icon, ui::AiTint& tint) {
    int image = 0, doc = 0, app = 0, video = 0, archive = 0;
    for (const std::wstring& name : items) {
        const size_t dot = name.find_last_of(L'.');
        std::wstring ext = dot == std::wstring::npos ? std::wstring() : name.substr(dot + 1);
        for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
        if (ext == L"jpg" || ext == L"jpeg" || ext == L"png" || ext == L"gif" || ext == L"webp" ||
            ext == L"bmp" || ext == L"heic")
            ++image;
        else if (ext == L"exe" || ext == L"msi" || ext == L"appx")
            ++app;
        else if (ext == L"mp4" || ext == L"mov" || ext == L"mkv" || ext == L"avi")
            ++video;
        else if (ext == L"zip" || ext == L"7z" || ext == L"rar" || ext == L"tar" || ext == L"gz")
            ++archive;
        else
            ++doc;
    }
    const int best = (std::max)((std::max)(image, doc), (std::max)(app, (std::max)(video, archive)));
    if (best == image) { icon = ui::AiIcon::Image; tint = ui::AiTint::Image; }
    else if (best == app) { icon = ui::AiIcon::Window; tint = ui::AiTint::App; }
    else if (best == video) { icon = ui::AiIcon::Video; tint = ui::AiTint::Video; }
    else if (best == archive) { icon = ui::AiIcon::File; tint = ui::AiTint::Archive; }
    else if (best == doc) { icon = ui::AiIcon::Document; tint = ui::AiTint::Document; }
    else { icon = ui::AiIcon::Folder; tint = ui::AiTint::Neutral; }
}

void FoldThinking(AppState& s, AiAgent& a) {
    if (!a.think_id) return;
    if (ui::AiBlockView* b = Find(s, a.think_id)) {
        if (a.reasoning.empty()) {
            // Nothing was said: the block goes rather than reading 已思考 0 秒.
            auto& blocks = s.aiAssistant.blocks;
            blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                                        [&](const ui::AiBlockView& v) { return v.id == a.think_id; }),
                         blocks.end());
        } else {
            const uint64_t secs = (std::max<uint64_t>)(1, (Now() - b->born_ms + 500) / 1000);
            b->running = false;
            b->streaming = false;
            b->label = Fill(Text(L"已思考 {n} 秒"), L"{n}", std::to_wstring(secs)) + L" · " +
                       EffortName(s.aiAssistant.effort);
            b->meta.clear();
            b->seconds.clear();
        }
    }
    a.think_id = 0;
}

// The steps so far fold into one line once the model speaks again.
void FoldSteps(AppState& s, AiAgent& a) {
    if (ui::AiBlockView* b = Find(s, a.steps_id)) {
        if (!b->collapsed) {
            b->label = Fill(Text(L"用了 {n} 次工具"), L"{n}", std::to_wstring(b->steps.size()));
            b->collapsed = true;
            b->open = false;
        }
    }
    a.steps_id = 0;
}

ui::AiBlockView& StepsBlock(AppState& s, AiAgent& a) {
    if (ui::AiBlockView* b = Find(s, a.steps_id)) return *b;
    ui::AiBlockView& b = Add(s, ui::AiBlockKind::Steps);
    a.steps_id = b.id;
    return b;
}

void AddStep(AppState& s, AiAgent& a, ui::AiIcon icon, std::wstring title, std::wstring arg,
             std::wstring sub) {
    ui::AiBlockView& b = StepsBlock(s, a);
    ui::AiStepView step;
    step.icon = icon;
    step.title = std::move(title);
    step.arg = std::move(arg);
    step.sub = std::move(sub);
    step.state = ui::AiStepState::Running;
    step.born_ms = Now();
    b.steps.push_back(std::move(step));
    b.collapsed = false;
    b.open = true;
}

void EndStep(AppState& s, AiAgent& a, bool ok, std::wstring sub) {
    if (ui::AiBlockView* b = Find(s, a.steps_id)) {
        if (!b->steps.empty()) {
            ui::AiStepView& st = b->steps.back();
            st.state = ok ? ui::AiStepState::Done : ui::AiStepState::Failed;
            st.done_ms = Now();
            st.progress = -1.0f;
            st.sub = std::move(sub);
        }
    }
}

void FailCard(AppState& s, AiAgent& a, ui::AiErrTone tone, ui::AiIcon icon, std::wstring title,
              std::wstring sub);

// The failed turn's card: the service's own words in the details, the buttons
// that can fix it.
void Fail(AppState& s, AiAgent& a, int http, DWORD win32_error, const std::string& error_type,
          const std::string& error_message) {
    FoldThinking(s, a);
    FoldSteps(s, a);
    if (ui::AiBlockView* t = Find(s, a.text_id)) t->streaming = false;
    a.text_id = 0;
    a.stream.reset();
    a.job.reset();
    a.calls.clear();
    a.call_index = 0;
    a.busy = false;
    s.aiAssistant.busy = false;
    const std::wstring host = AiAgentHost();
    const std::wstring name = ServiceName();
    ui::AiBlockView& b = Add(s, ui::AiBlockKind::Error);
    b.tone = ui::AiErrTone::Err;
    b.icon = ui::AiIcon::AlertCircle;
    if (http == 401) {
        b.title = Text(L"API Key 无效或已过期");
        b.sub = Fill(Text(L"{name} 不认这个 Key。到「管理连接」换一个新的，再点重试。"), L"{name}", name);
        b.actions.push_back({ui::AiErrAct::ManageConn, ui::AiIcon::Window, Text(L"管理连接"), true});
        b.actions.push_back({ui::AiErrAct::Retry, ui::AiIcon::Retry, Text(L"重试"), false});
    } else if (http == 402) {
        b.title = Text(L"账户余额不足");
        b.sub = Fill(Text(L"{host} 返回余额不足。充值后点重试。"), L"{host}", host);
        b.actions.push_back({ui::AiErrAct::Retry, ui::AiIcon::Retry, Text(L"重试"), true});
    } else if (http == 429) {
        b.title = Text(L"请求太频繁");
        b.sub = Fill(Text(L"{name} 暂时限制了请求速度，等几秒再试。"), L"{name}", name);
        b.actions.push_back({ui::AiErrAct::Retry, ui::AiIcon::Retry, Text(L"重试"), true});
    } else if (http >= 500) {
        b.title = Fill(Fill(Text(L"{name} 服务暂时出错（{code}）"), L"{name}", name), L"{code}",
                       std::to_wstring(http));
        b.sub = Text(L"不是你的设置问题，稍后重试即可。");
        b.actions.push_back({ui::AiErrAct::Retry, ui::AiIcon::Retry, Text(L"重试"), true});
    } else if (http == 0) {
        b.title = Fill(Text(L"连不上 {host}"), L"{host}", host);
        b.sub = Fill(Text(L"检查网络或代理设置。（错误 {code}）"), L"{code}", std::to_wstring(win32_error));
        b.actions.push_back({ui::AiErrAct::Retry, ui::AiIcon::Retry, Text(L"重试"), true});
        b.actions.push_back({ui::AiErrAct::ManageConn, ui::AiIcon::Window, Text(L"管理连接"), false});
    } else {
        b.title = Fill(Fill(Text(L"{name} 返回了 {code}"), L"{name}", name), L"{code}",
                       std::to_wstring(http));
        b.sub = Text(L"稍后重试；如果一直这样，到「管理连接」核对接口地址。");
        b.actions.push_back({ui::AiErrAct::Retry, ui::AiIcon::Retry, Text(L"重试"), true});
        b.actions.push_back({ui::AiErrAct::ManageConn, ui::AiIcon::Window, Text(L"管理连接"), false});
    }
    if (!error_message.empty() || !error_type.empty()) {
        b.detail = json::Wide(error_message.empty() ? error_type : error_message);
    }
    // The chip: red needs you. With the panel open the card says it all.
    AiAssistantState& as = s.aiAssistant;
    if (as.open) SetChip(s, ui::StatusAiState::Idle, nullptr);
    else {
        SetChip(s, ui::StatusAiState::Error, L"出错");
        as.unread = true;
    }
    as.peek_failed = true;
    as.peek_tone = b.tone;
    as.peek_title = b.title;
    if (!as.open) AiPeekShow(s, 650);
    Redraw(s);
}

// A failure of our own making (too many rounds): no service error to quote.
void FailNote(AppState& s, AiAgent& a, const wchar_t* title, const wchar_t* sub) {
    FoldThinking(s, a);
    FoldSteps(s, a);
    if (ui::AiBlockView* t = Find(s, a.text_id)) t->streaming = false;
    a.text_id = 0;
    a.stream.reset();
    a.job.reset();
    a.calls.clear();
    a.call_index = 0;
    a.busy = false;
    s.aiAssistant.busy = false;
    ui::AiBlockView& b = Add(s, ui::AiBlockKind::Error);
    b.tone = ui::AiErrTone::Warn;
    b.icon = ui::AiIcon::AlertCircle;
    b.title = title;
    b.sub = sub;
    b.actions.push_back({ui::AiErrAct::Retry, ui::AiIcon::Retry, Text(L"重试"), true});
    SetChip(s, ui::StatusAiState::Warn, L"没完成");
    Redraw(s);
}

// A stored key is needed before anything is sent.
bool NoKey(AppState& s) {
    std::wstring key;
    const bool has = app::ai::LoadModelKey(key) && !key.empty();
    SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
    if (has) return false;
    ui::AiBlockView& b = Add(s, ui::AiBlockKind::Error);
    b.tone = ui::AiErrTone::Accent;
    b.icon = ui::AiIcon::Spark;
    b.title = Text(L"还没有连接模型服务");
    b.sub = Text(L"填入 API Key 之后我就能回答问题了。");
    b.actions.push_back({ui::AiErrAct::ManageConn, ui::AiIcon::Window, Text(L"管理连接"), true});
    Redraw(s);
    return true;
}

std::string Body(AppState& s, AiAgent& a) {
    json::Value body = json::Value::Object();
    body.set("model", json::Utf8(AiAssistantModelName(s)));
    json::Value messages = json::Value::Array();
    json::Value sys = json::Value::Object();
    sys.set("role", "system");
    sys.set("content", SystemPrompt());
    messages.push(std::move(sys));
    for (const json::Value& m : a.history.items()) messages.push(m);
    body.set("messages", std::move(messages));
    body.set("stream", true);
    json::Value so = json::Value::Object();
    so.set("include_usage", true);
    body.set("stream_options", std::move(so));
    body.set("tools", app::ai::Tools());
    const int effort = std::clamp(s.aiAssistant.effort, 0, 3);
    json::Value thinking = json::Value::Object();
    thinking.set("type", effort == 0 ? "disabled" : "enabled");
    body.set("thinking", std::move(thinking));
    if (effort > 0) {
        static constexpr const char* kEffort[] = {"", "low", "high", "max"};
        body.set("reasoning_effort", kEffort[effort]);
    }
    return json::Serialize(body);
}

// The files selected in the active tab, as absolute paths.
std::vector<std::wstring> SelectionPaths(AppState& s) {
    std::vector<std::wstring> selection;
    app::Tab* tab = ActiveTab(s);
    if (!tab || !tab->snapshot) return selection;
    if (tab->all_selected) {
        for (size_t i = 0; i < tab->snapshot->size() && i < 500; ++i)
            selection.push_back(Join(tab->current_path, (*tab->snapshot)[i].name));
        return selection;
    }
    for (const int index : tab->selected) {
        if (index >= 0 && index < static_cast<int>(tab->snapshot->size()))
            selection.push_back(Join(tab->current_path, (*tab->snapshot)[static_cast<size_t>(index)].name));
    }
    if (selection.empty() && tab->selected_index >= 0 &&
        tab->selected_index < static_cast<int>(tab->snapshot->size()))
        selection.push_back(Join(tab->current_path, (*tab->snapshot)[static_cast<size_t>(tab->selected_index)].name));
    return selection;
}

void SendRound(AppState& s);
void StartNextTool(AppState& s);
void FinishTool(AppState& s, app::ai::ToolOutcome out);

void CALLBACK PumpTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s) {
        KillTimer(hwnd, id);
        return;
    }
    AiAgentPump(*s);
}

void ArmPump(AppState& s, bool on) {
    if (!s.hwnd) return;
    if (on) SetTimer(s.hwnd, kPumpTimer, kPumpMs, PumpTimer);
    else KillTimer(s.hwnd, kPumpTimer);
}

// The plan card and the amber card that waits for the user.
void ShowOrganizeConfirm(AppState& s, std::shared_ptr<app::ai::OrganizePlan> plan,
                         const std::string& call_id) {
    AiAgent& a = Agent(s);
    FoldSteps(s, a);   // the plan stands on its own
    ui::AiBlockView& b = Add(s, ui::AiBlockKind::Plan);
    b.label = Text(L"整理方案");
    b.meta = std::to_wstring(plan->groups.size()) + L" 个文件夹 · " + std::to_wstring(plan->total) + L" 项";
    for (const app::ai::OrganizeGroup& g : plan->groups) {
        ui::AiGroupView v;
        GroupLook(g.items, v.icon, v.tint);
        v.name = g.name;
        v.count = std::to_wstring(g.items.size()) + L" 个";
        for (size_t i = 0; i < g.items.size() && i < 40; ++i) {
            if (!v.list.empty()) v.list += L"、";
            v.list += g.items[i];
        }
        if (!g.exists) v.badge = Text(L"新建");
        v.in_ms = Now();
        b.groups.push_back(std::move(v));
    }
    a.plan_block = b.id;
    a.plan = std::move(plan);
    a.plan_call_id = call_id;
    a.awaiting = true;
    SetChip(s, ui::StatusAiState::Waiting, L"等你确认");
    ui::AiBlockView& cf = Add(s, ui::AiBlockKind::Confirm);
    cf.title = L"把 " + std::to_wstring(a.plan->total) + L" 项放进 " +
               std::to_wstring(a.plan->groups.size()) + L" 个文件夹";
    cf.sub = L"都在「" + a.plan->label + L"」里，不会覆盖同名文件。";
    cf.hint = Text(L"确认后可以在结果卡上点「撤销」。");
    cf.hint_icon = ui::AiIcon::Undo;
    cf.deny = Text(L"不整理");
    cf.accept = Text(L"开始整理");
    a.confirm_block = cf.id;
    Redraw(s);
}

// The rule split into chips: {name} / {n:2} / {ext} read as tokens.
std::vector<ui::AiRuleToken> RuleTokens(const std::wstring& rule) {
    std::vector<ui::AiRuleToken> out;
    std::wstring text;
    auto flush = [&] {
        if (text.empty()) return;
        ui::AiRuleToken t;
        t.text = text;
        out.push_back(std::move(t));
        text.clear();
    };
    for (size_t i = 0; i < rule.size();) {
        if (rule[i] == L'{') {
            const size_t close = rule.find(L'}', i);
            if (close != std::wstring::npos) {
                flush();
                ui::AiRuleToken t;
                t.text = rule.substr(i, close - i + 1);
                t.token = true;
                t.word = true;
                out.push_back(std::move(t));
                i = close + 1;
                continue;
            }
        }
        text.push_back(rule[i++]);
    }
    flush();
    return out;
}

// The rename card (old → new rows, rule, checks) and the amber card.
void ShowRenameConfirm(AppState& s, std::shared_ptr<app::ai::RenamePlan> plan,
                       const std::string& call_id) {
    AiAgent& a = Agent(s);
    FoldSteps(s, a);
    ui::AiBlockView& b = Add(s, ui::AiBlockKind::Rename);
    b.label = Text(L"重命名方案");
    b.meta = std::to_wstring(plan->old_paths.size()) + L" 个文件";
    b.rule_label = Text(L"规则");
    b.rule = RuleTokens(plan->rule);
    const size_t kRows = 5;
    for (size_t i = 0; i < plan->rows.size() && i < kRows; ++i) {
        ui::AiRenameRow row;
        row.old_name = plan->rows[i].first;
        ui::AiRuleToken t;
        t.text = plan->rows[i].second;
        row.parts.push_back(std::move(t));
        row.sample = i < 3 || i == 4;
        row.in_ms = Now();
        b.rows.push_back(std::move(row));
    }
    if (plan->rows.size() > kRows) {
        b.more = L"还有 " + std::to_wstring(plan->rows.size() - kRows) + L" 个";
        b.less = Text(L"收起");
    }
    if (plan->clashed)
        b.checks.push_back({std::to_wstring(plan->clashed) + L" 个结果重名，已跳过", true});
    else
        b.checks.push_back({Text(L"不会覆盖已有文件"), false});
    if (plan->skipped > plan->clashed)
        b.checks.push_back({std::to_wstring(plan->skipped - plan->clashed) + L" 个名字不变", false});
    b.sub = Text(L"确认后可以撤销");
    a.rename_block = b.id;
    a.rename = std::move(plan);
    a.rename_call_id = call_id;
    a.awaiting_rename = true;
    SetChip(s, ui::StatusAiState::Waiting, L"等你确认");
    ui::AiBlockView& cf = Add(s, ui::AiBlockKind::Confirm);
    cf.title = L"重命名 " + std::to_wstring(a.rename->old_paths.size()) + L" 个文件";
    cf.sub = L"都在「" + a.rename->label + L"」里，扩展名不变。";
    cf.hint = Text(L"确认后可以在结果卡上点「撤销」。");
    cf.hint_icon = ui::AiIcon::Undo;
    cf.deny = Text(L"不改了");
    cf.accept = Text(L"重命名");
    a.confirm_block = cf.id;
    Redraw(s);
}

// The delete card: amber for the Recycle Bin, red and two-click for good.
void ShowDeleteConfirm(AppState& s, std::shared_ptr<app::ai::DeletePlan> plan,
                       const std::string& call_id) {
    AiAgent& a = Agent(s);
    FoldSteps(s, a);
    a.delete_block = 0;
    a.del = std::move(plan);
    a.delete_call_id = call_id;
    a.awaiting_delete = true;
    SetChip(s, ui::StatusAiState::Waiting, L"等你确认");
    ui::AiBlockView& cf = Add(s, ui::AiBlockKind::Confirm);
    const std::wstring count = std::to_wstring(a.del->paths.size());
    if (a.del->permanent) {
        cf.danger = true;
        cf.cf_icon = ui::AiIcon::Trash;
        cf.tag = Text(L"删除类");
        cf.title = L"永久删除 " + count + L" 项";
        cf.sub = Text(L"不会进回收站，之后找不回来。");
        cf.hint = Text(L"点两次才会删除。也可以先移到回收站。");
        cf.hint_icon = ui::AiIcon::AlertCircle;
        cf.deny = Text(L"取消");
        cf.accept = Text(L"永久删除");
        cf.arm = Text(L"再点一次，永久删除");
    } else {
        cf.title = L"把 " + count + L" 项移到回收站";
        cf.sub = L"都在「" + a.del->label + L"」里；回收站里还能找回。";
        cf.hint = Text(L"确认后可以在结果卡上点「撤销」。");
        cf.hint_icon = ui::AiIcon::Undo;
        cf.deny = Text(L"取消");
        cf.accept = Text(L"移到回收站");
    }
    a.confirm_block = cf.id;
    Redraw(s);
}

// Runs the next call the model asked for, or asks the model again once all
// of them are done.
void StartNextTool(AppState& s) {
    AiAgent& a = Agent(s);
    if (a.job) return;
    if (a.call_index >= a.calls.size()) {
        a.calls.clear();
        a.call_index = 0;
        SendRound(s);
        return;
    }
    const app::ai::ModelToolCall& call = a.calls[a.call_index];
    json::Value args = json::Value::Object();
    if (!call.arguments.empty() && !json::Parse(call.arguments, args)) args = json::Value::Object();

    if (call.name == "rename_files") {
        const std::vector<std::wstring> selection = SelectionPaths(s);
        auto plan = std::make_shared<app::ai::RenamePlan>();
        std::string code, message;
        if (!app::ai::ParseRenamePlan(args, selection, *plan, code, message)) {
            app::ai::ToolOutcome out;
            out.ok = false;
            out.code = code;
            out.message = message;
            auto bad = std::make_shared<AiAgent::Job>();
            bad->out = std::move(out);
            bad->done.store(true);
            a.job = bad;
            return;
        }
        ShowRenameConfirm(s, std::move(plan), call.id);
        return;
    }
    if (call.name == "delete_files") {
        auto plan = std::make_shared<app::ai::DeletePlan>();
        std::string code, message;
        if (!app::ai::ParseDeletePlan(args, SelectionPaths(s), *plan, code, message)) {
            app::ai::ToolOutcome out;
            out.ok = false;
            out.code = code;
            out.message = message;
            auto bad = std::make_shared<AiAgent::Job>();
            bad->out = std::move(out);
            bad->done.store(true);
            a.job = bad;
            return;
        }
        ShowDeleteConfirm(s, std::move(plan), call.id);
        return;
    }
    if (call.name == "organize_files") {
        // Without a folder the open one is meant, as with list_folder.
        if (!args.find("folder")) {
            app::Tab* tab = ActiveTab(s);
            if (tab && !tab->current_path.empty()) args.set("folder", json::Utf8(tab->current_path));
        }
        auto plan = std::make_shared<app::ai::OrganizePlan>();
        std::string code, message;
        if (!app::ai::ParseOrganizePlan(args, *plan, code, message)) {
            app::ai::ToolOutcome out;
            out.ok = false;
            out.code = code;
            out.message = message;
            auto bad = std::make_shared<AiAgent::Job>();
            bad->out = std::move(out);
            bad->done.store(true);
            a.job = bad;   // the pump completes the call
            return;
        }
        ShowOrganizeConfirm(s, std::move(plan), call.id);
        return;
    }

    const std::wstring title = app::ai::ToolTitle(call.name);
    auto job = std::make_shared<AiAgent::Job>();
    if (call.name == "get_state") {
        AddStep(s, a, ui::AiIcon::Window, title, {}, Text(L"正在读取鹿铃当前的文件夹和选中项…"));
        job->out = app::ai::RunToolOnUi(s, call.name, args);
        job->done.store(true);
        a.job = job;
        SetChip(s, ui::StatusAiState::Busy, L"读取中");
        ArmPump(s, true);
        return;   // the pump picks it up on the next tick
    }

    std::wstring arg;
    if (call.name == "list_folder") {
        // Without a path the open folder is the one meant.
        if (!args.find("path")) {
            app::Tab* tab = ActiveTab(s);
            if (tab && !tab->current_path.empty()) args.set("path", json::Utf8(tab->current_path));
        }
        arg = json::Wide(args.get_string("path"));
        if (const size_t cut = arg.find_last_of(L"\\/"); cut != std::wstring::npos && cut + 1 < arg.size())
            arg = arg.substr(cut + 1);
    }
    AddStep(s, a, call.name == "list_folder" ? ui::AiIcon::Rows : ui::AiIcon::Search, title, arg,
            Text(L"正在读取…"));
    SetChip(s, ui::StatusAiState::Busy, L"读取中");

    const std::string name = call.name;
    a.job = job;
    ArmPump(s, true);
    std::thread([job, name, args] {
        job->out = app::ai::RunToolOnWorker(name, args, job->cancelled);
        job->done.store(true);
    }).detach();
    Redraw(s);
}

void FinishTool(AppState& s, app::ai::ToolOutcome out) {
    AiAgent& a = Agent(s);
    if (a.call_index >= a.calls.size()) return;
    const app::ai::ModelToolCall& call = a.calls[a.call_index];
    // The tool message: the result on success, one line on failure.
    json::Value msg = json::Value::Object();
    msg.set("role", "tool");
    msg.set("tool_call_id", call.id);
    std::string text;
    if (out.ok) {
        text = json::Serialize(out.data);
    } else {
        json::Value err = json::Value::Object();
        err.set("ok", false);
        json::Value e = json::Value::Object();
        e.set("code", out.code);
        e.set("message", out.message);
        err.set("error", std::move(e));
        text = json::Serialize(err);
    }
    if (text.size() > kMaxToolText) {
        size_t cut = kMaxToolText;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        text.resize(cut);
        text += "... [truncated]";
    }
    msg.set("content", std::move(text));
    a.history.push(std::move(msg));

    std::wstring sub;
    if (out.ok) {
        const json::Value* moved = out.data.find("moved");
        const json::Value* files = out.data.find("files");
        if (moved && moved->is_number()) {
            const size_t n = static_cast<size_t>(moved->as_number());
            const json::Value* created = out.data.find("created");
            const size_t c = created ? static_cast<size_t>(created->as_number()) : 0;
            sub = Fill(Fill(Text(L"{n} 项已移动 · 新建 {c} 个文件夹"), L"{n}", std::to_wstring(n)), L"{c}",
                       std::to_wstring(c));
        } else if (files && files->is_number()) {
            const size_t f = static_cast<size_t>(files->as_number());
            const json::Value* dirs = out.data.find("folders");
            const size_t d = dirs ? static_cast<size_t>(dirs->as_number()) : 0;
            const json::Value* total = out.data.find("total_size");
            const uint64_t bytes = total ? static_cast<uint64_t>(total->as_number()) : 0;
            sub = d ? Fill(Fill(Fill(Text(L"{n} 个文件 · {d} 个文件夹 · {size}"), L"{n}", std::to_wstring(f)),
                                L"{d}", std::to_wstring(d)), L"{size}", FormatSize(bytes))
                    : Fill(Fill(Text(L"{n} 个文件 · {size}"), L"{n}", std::to_wstring(f)), L"{size}",
                           FormatSize(bytes));
        } else {
            const std::wstring folder = json::Wide(out.data.get_string("folder_name"));
            const json::Value* items = out.data.find("items");
            const size_t count = items ? static_cast<size_t>(items->as_number()) : 0;
            const json::Value* picked = out.data.find("selected_count");
            const size_t chosen = picked ? static_cast<size_t>(picked->as_number()) : 0;
            if (folder.empty()) {
                sub = Text(L"没有打开的文件夹");
            } else if (chosen) {
                sub = Fill(Fill(Fill(Text(L"{folder} · {n} 项 · 选中 {k} 项"), L"{folder}", folder), L"{n}",
                                std::to_wstring(count)), L"{k}", std::to_wstring(chosen));
            } else {
                sub = Fill(Fill(Text(L"{folder} · {n} 项 · 未选中"), L"{folder}", folder), L"{n}",
                           std::to_wstring(count));
            }
        }
    } else {
        sub = Fill(Text(L"没有成功：{reason}"), L"{reason}", json::Wide(out.message));
    }
    EndStep(s, a, out.ok, std::move(sub));
    a.job.reset();
    ++a.call_index;
    Redraw(s);
    StartNextTool(s);
}

// Every queued task finished: the result card (with 撤销), then the call
// completes.
void FinishOrganizeQueue(AppState& s) {
    AiAgent& a = Agent(s);
    std::shared_ptr<AiAgent::OrganizeJob> job = a.organ;
    a.organ.reset();
    // How many items actually left the folder.
    size_t moved = 0;
    for (const app::ai::OrganizeGroup& g : job->plan->groups)
        for (const std::wstring& name : g.items)
            if (!PathExists(Join(job->plan->folder, name))) ++moved;
    const size_t total = job->plan->total;
    const bool ok = moved > 0;
    EndStep(s, a, ok, std::to_wstring(moved) + L" 项已移动");
    app::ai::ToolOutcome out;
    out.ok = ok;
    if (ok) {
        out.data = json::Value::Object();
        out.data.set("ok", true);
        out.data.set("moved", static_cast<int64_t>(moved));
        out.data.set("created", static_cast<int64_t>(job->plan->created));
        if (moved < total) out.data.set("skipped", static_cast<int64_t>(total - moved));
        ui::AiBlockView& r = Add(s, ui::AiBlockKind::Result);
        r.title = Fill(Text(L"已整理 {n} 项"), L"{n}", std::to_wstring(moved));
        r.sub = Fill(Fill(Text(L"放进 {g} 个文件夹 · 新建 {c} 个"), L"{g}",
                          std::to_wstring(job->plan->groups.size())),
                     L"{c}", std::to_wstring(job->plan->created));
        r.deny = Text(L"撤销");
        if (job->group) {
            a.undo_group = job->group;   // the same group the app's undo stack holds
            a.undo_block = r.id;
        }
    } else {
        out.code = "move_failed";
        out.message = "The moves did not go through; nothing moved.";
    }
    a.plan.reset();
    FinishTool(s, std::move(out));
}

// The batch rename finished: count what changed, then the result card.
void FinishRenameQueue(AppState& s) {
    AiAgent& a = Agent(s);
    std::shared_ptr<AiAgent::RenameJob> job = a.rename_job;
    a.rename_job.reset();
    size_t renamed = 0;
    if (a.rename) {
        for (const std::wstring& old_path : a.rename->old_paths)
            if (!PathExists(old_path)) ++renamed;
    }
    const bool ok = renamed > 0;
    EndStep(s, a, ok, std::to_wstring(renamed) + L" 个已重命名");
    app::ai::ToolOutcome out;
    out.ok = ok;
    if (ok) {
        out.data = json::Value::Object();
        out.data.set("ok", true);
        out.data.set("renamed", static_cast<int64_t>(renamed));
        out.data.set("skipped", static_cast<int64_t>(job->total - renamed));
        ui::AiBlockView& r = Add(s, ui::AiBlockKind::Result);
        r.title = Fill(Text(L"已重命名 {n} 个文件"), L"{n}", std::to_wstring(renamed));
        r.sub = a.rename ? (L"规则：" + a.rename->rule) : std::wstring();
        r.deny = Text(L"撤销");
        if (job->group) {
            a.undo_group = job->group;
            a.undo_block = r.id;
        }
    } else {
        out.code = "rename_failed";
        out.message = "The renames did not go through; the names are unchanged.";
    }
    a.rename.reset();
    FinishTool(s, std::move(out));
}

// The deletes finished: count what is gone, then the result card.
void FinishDeleteQueue(AppState& s) {
    AiAgent& a = Agent(s);
    std::shared_ptr<AiAgent::DeleteJob> job = a.delete_job;
    a.delete_job.reset();
    size_t gone = 0, total = 0;
    if (a.del) {
        for (const std::wstring& path : a.del->paths) {
            ++total;
            if (!PathExists(path)) ++gone;
        }
    }
    const bool ok = gone > 0;
    EndStep(s, a, ok, std::to_wstring(gone) + L" 项已删除");
    app::ai::ToolOutcome out;
    out.ok = ok;
    if (ok) {
        out.data = json::Value::Object();
        out.data.set("ok", true);
        out.data.set("deleted", static_cast<int64_t>(gone));
        out.data.set("permanent", job->permanent);
        if (gone < total) out.data.set("kept", static_cast<int64_t>(total - gone));
        ui::AiBlockView& r = Add(s, ui::AiBlockKind::Result);
        r.title = Fill(job->permanent ? Text(L"已永久删除 {n} 项") : Text(L"已移到回收站 {n} 项"), L"{n}",
                       std::to_wstring(gone));
        r.sub = job->permanent ? Text(L"这些项目已经不在磁盘上了。") : Text(L"回收站里还能找回。");
        if (!job->permanent && job->group) {
            r.deny = Text(L"撤销");
            a.undo_group = job->group;
            a.undo_block = r.id;
            a.undo_paths = a.del ? a.del->paths : std::vector<std::wstring>{};
        }
    } else {
        out.code = "delete_failed";
        out.message = "Nothing was deleted.";
    }
    a.del.reset();
    FinishTool(s, std::move(out));
}

void FailCut(AppState& s, AiAgent& a) {
    // The socket closed without a finish: keep what arrived, say it broke.
    FoldThinking(s, a);
    FoldSteps(s, a);
    if (ui::AiBlockView* t = Find(s, a.text_id)) t->streaming = false;
    a.text_id = 0;
    a.stream.reset();
    a.job.reset();
    a.calls.clear();
    a.call_index = 0;
    a.busy = false;
    s.aiAssistant.busy = false;
    ui::AiBlockView& b = Add(s, ui::AiBlockKind::Error);
    b.tone = ui::AiErrTone::Warn;
    b.icon = ui::AiIcon::AlertCircle;
    b.title = Text(L"回答中途断开了");
    b.sub = Text(L"已收到的内容保留");
    b.actions.push_back({ui::AiErrAct::Retry, ui::AiIcon::Retry, Text(L"重试"), true});
    SetChip(s, ui::StatusAiState::Warn, L"没完成");
}

void SendRound(AppState& s) {
    AiAgent& a = Agent(s);
    AiAssistantState& as = s.aiAssistant;
    if (++a.rounds > kMaxRounds) {
        FailNote(s, a, L"这次用到的步骤太多了", L"换个说法，或者把要求分开说。");
        return;
    }
    std::wstring key;
    if (!app::ai::LoadModelKey(key)) {
        Fail(s, a, 0, ERROR_NOT_FOUND, "no_key", "");
        return;
    }
    a.reasoning.clear();
    a.content.clear();
    a.text_id = 0;
    if (as.effort > 0) {
        ui::AiBlockView& b = Add(s, ui::AiBlockKind::Thinking);
        b.running = true;
        b.label = Text(L"思考中");
        b.meta = L"· " + EffortName(as.effort);
        a.think_id = b.id;
    }
    app::ai::ModelEndpoint endpoint;
    endpoint.base_url = AiAgentBaseUrl();
    endpoint.key = std::move(key);
    a.stream = app::ai::ModelStream::Start(std::move(endpoint), Body(s, a));
    ArmPump(s, true);
    Redraw(s);
}

void EndTurn(AppState& s) {
    AiAgent& a = Agent(s);
    AiAssistantState& as = s.aiAssistant;
    a.stream.reset();
    a.job.reset();
    a.calls.clear();
    a.call_index = 0;
    a.rounds = 0;
    a.busy = false;
    as.busy = false;
    SetChip(s, ui::StatusAiState::Done, L"完成");
    if (!as.open) {
        as.unread = true;
        AiPeekShow(s, 650);
    }
    ArmPump(s, false);
    Redraw(s);
}

} // namespace

bool AiAgentBusy(const AppState& s) { return s.aiAssistant.busy; }

void AiAgentConfirm(AppState& s, bool accept) {
    AiAgent& a = Agent(s);
    if (a.awaiting_delete && a.del) {
        if (!accept) {
            a.awaiting_delete = false;
            if (ui::AiBlockView* cf = Find(s, a.confirm_block)) {
                cf->done = true;
                cf->declined = true;
                cf->title = Text(L"没有删除，什么都没动");
            }
            a.confirm_block = 0;
            app::ai::ToolOutcome out;
            out.ok = false;
            out.code = "user_denied";
            out.message = "The user declined the delete; nothing was removed. Do not ask again unless they say so.";
            a.del.reset();
            FinishTool(s, std::move(out));
            return;
        }
        // A permanent delete takes two clicks.
        if (a.del->permanent) {
            ui::AiBlockView* cf = Find(s, a.confirm_block);
            if (cf && !cf->armed) {
                cf->armed = true;
                if (!cf->arm.empty()) cf->accept = cf->arm;
                Redraw(s);
                return;
            }
        }
        a.awaiting_delete = false;
        if (ui::AiBlockView* cf = Find(s, a.confirm_block)) {
            cf->done = true;
            cf->title = L"已确认，删除 " + std::to_wstring(a.del->paths.size()) + L" 项";
        }
        a.confirm_block = 0;
        const bool permanent = a.del->permanent;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        uint64_t group = 0;
        if (!permanent) group = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        ops::OpRequest req;
        req.type = permanent ? ops::OpType::RealDelete : ops::OpType::RecycleDelete;
        req.sources = a.del->paths;
        if (group) {
            req.undo_group = group;
            req.undo_label = Text(L"鹿铃助手删除");
        }
        auto job = std::make_shared<AiAgent::DeleteJob>();
        job->group = group;
        job->total = a.del->paths.size();
        job->permanent = permanent;
        job->tasks.push_back(s.ops.Submit(std::move(req)));
        AddStep(s, a, ui::AiIcon::Trash, Text(L"删除文件"), a.del->label,
                permanent ? Text(L"正在永久删除…") : Text(L"正在移到回收站…"));
        job->step_block = a.steps_id;
        a.delete_job = job;
        SetChip(s, ui::StatusAiState::Busy, permanent ? L"正在永久删除" : L"正在删除", 0.0f);
        ArmPump(s, true);
        Redraw(s);
        return;
    }
    if (a.awaiting_rename && a.rename) {
        a.awaiting_rename = false;
        if (ui::AiBlockView* cf = Find(s, a.confirm_block)) {
            cf->done = true;
            cf->declined = !accept;
            cf->title = accept ? (L"已确认，重命名 " + std::to_wstring(a.rename->old_paths.size()) + L" 个文件")
                               : std::wstring(L"没有改名，什么都没动");
        }
        if (ui::AiBlockView* rb = Find(s, a.rename_block)) rb->collapsed = true;
        a.confirm_block = 0;
        if (!accept) {
            app::ai::ToolOutcome out;
            out.ok = false;
            out.code = "user_denied";
            out.message =
                "The user declined the rename; nothing changed. Do not propose the same rule again; ask "
                "what they would like instead.";
            a.rename.reset();
            FinishTool(s, std::move(out));
            return;
        }
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const uint64_t group = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        ops::OpRequest req;
        req.type = ops::OpType::BatchRename;
        req.sources = a.rename->old_paths;
        req.new_names = a.rename->new_names;
        req.undo_group = group;
        req.undo_label = Text(L"鹿铃助手重命名");
        auto job = std::make_shared<AiAgent::RenameJob>();
        job->group = group;
        job->total = a.rename->old_paths.size();
        job->tasks.push_back(s.ops.Submit(std::move(req)));
        AddStep(s, a, ui::AiIcon::Rename, Text(L"重命名文件"), a.rename->label, Text(L"正在重命名…"));
        job->step_block = a.steps_id;
        a.rename_job = job;
        SetChip(s, ui::StatusAiState::Busy, L"正在重命名", 0.0f);
        ArmPump(s, true);
        Redraw(s);
        return;
    }
    if (!a.awaiting || !a.plan) return;
    a.awaiting = false;
    if (ui::AiBlockView* cf = Find(s, a.confirm_block)) {
        cf->done = true;
        cf->declined = !accept;
        cf->title = accept ? (L"已确认，整理 " + std::to_wstring(a.plan->total) + L" 项")
                           : std::wstring(L"没有整理，什么都没动");
    }
    if (ui::AiBlockView* pb = Find(s, a.plan_block)) pb->collapsed = true;
    a.confirm_block = 0;
    if (!accept) {
        app::ai::ToolOutcome out;
        out.ok = false;
        out.code = "user_denied";
        out.message =
            "The user declined this plan; nothing moved. Do not propose the same plan again; ask what "
            "they would like instead.";
        a.plan.reset();
        FinishTool(s, std::move(out));
        return;
    }
    std::shared_ptr<app::ai::OrganizePlan> plan = a.plan;
    // One undo group for the whole plan: the app's own Ctrl+Z takes it back
    // (the same shape 合并到文件夹 uses).
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const uint64_t group = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const std::wstring label = Text(L"鹿铃助手整理");
    auto job = std::make_shared<AiAgent::OrganizeJob>();
    job->plan = plan;
    job->group = group;
    for (const app::ai::OrganizeGroup& g : plan->groups) {
        if (g.exists) continue;
        ops::OpRequest req;
        req.type = ops::OpType::CreateFolder;
        req.sources.push_back(Join(plan->folder, g.name));
        req.undo_group = group;
        req.undo_label = label;
        job->tasks.push_back(s.ops.Submit(std::move(req)));
    }
    for (const app::ai::OrganizeGroup& g : plan->groups) {
        ops::OpRequest req;
        req.type = ops::OpType::Move;
        for (const std::wstring& name : g.items) req.sources.push_back(Join(plan->folder, name));
        req.dest_dir = Join(plan->folder, g.name);
        req.collision_policy = ops::CollisionPolicy::KeepBoth;
        req.undo_group = group;
        req.undo_label = label;
        job->tasks.push_back(s.ops.Submit(std::move(req)));
    }
    AddStep(s, a, ui::AiIcon::Move, Text(L"整理文件"), plan->label, Text(L"正在移动…"));
    job->step_block = a.steps_id;
    a.organ = job;
    SetChip(s, ui::StatusAiState::Busy, L"正在整理", 0.0f);
    ArmPump(s, true);
    Redraw(s);
}

void AiAgentUndoResult(AppState& s, int block) {
    AiAgent& a = Agent(s);
    if (!a.undo_group) return;
    // The card reports its block index; the stored value is the block's id.
    if (block < 0 || block >= static_cast<int>(s.aiAssistant.blocks.size())) return;
    if (s.aiAssistant.blocks[static_cast<size_t>(block)].id != a.undo_block) return;
    using GroupUndo = ops::OpsManager::GroupUndo;
    const GroupUndo result = s.ops.UndoGroup(a.undo_group);
    ui::AiBlockView* card = Find(s, a.undo_block);
    if (result == GroupUndo::Started || result == GroupUndo::Pending) {
        if (card) card->title = Text(L"正在撤销…");
        a.undo_wait = true;
        a.undo_started = GetTickCount64();
        SetChip(s, ui::StatusAiState::Busy, L"正在撤销");
        ArmPump(s, true);
        Redraw(s);
        return;
    }
    if (card) {
        card->done = true;
        card->title = result == GroupUndo::NotOnTop
                          ? Text(L"不能撤销：之后还有别的操作")
                          : Text(L"无法撤销");
    }
    a.undo_group = 0;
    a.undo_block = 0;
    Redraw(s);
}

void AiAgentSend(AppState& s, const std::wstring& prompt) {
    if (prompt.empty()) return;
    AiAssistantState& as = s.aiAssistant;
    AiAgent& a = Agent(s);
    if (as.busy) return;

    Add(s, ui::AiBlockKind::User).text = prompt;
    a.last_prompt = prompt;
    AiPeekHide(s);
    as.turn_start = Now();
    as.peek_text.clear();
    as.peek_title.clear();
    as.peek_failed = false;
    as.unread = false;

    if (NoKey(s)) return;

    json::Value user = json::Value::Object();
    user.set("role", "user");
    user.set("content", json::Utf8(prompt));
    a.history.push(std::move(user));

    a.rounds = 0;
    a.steps_id = 0;
    a.busy = true;
    as.busy = true;
    SetChip(s, ui::StatusAiState::Busy, L"思考中");
    SendRound(s);
}

void AiAgentPump(AppState& s) {
    AiAssistantState& as = s.aiAssistant;
    if (!as.agent) return;
    AiAgent& a = *as.agent;

    // The app's operation queue is doing the moves: poll until every task of
    // ours has a final status.
    if (a.organ) {
        size_t finished = 0, failed = 0;
        for (const uint64_t id : a.organ->tasks) {
            if (const std::optional<ops::OpStatus> st = s.ops.FinalStatus(id)) {
                ++finished;
                failed += st->failed_items;
            }
        }
        if (finished != a.organ->finished || failed != a.organ->failed) {
            a.organ->finished = finished;
            a.organ->failed = failed;
            const float p = a.organ->tasks.empty()
                ? 1.0f
                : static_cast<float>(finished) / static_cast<float>(a.organ->tasks.size());
            if (ui::AiBlockView* b = Find(s, a.organ->step_block)) {
                if (!b->steps.empty()) {
                    b->steps.back().progress = p;
                    b->steps.back().sub = L"第 " + std::to_wstring(finished) + L" / " +
                                          std::to_wstring(a.organ->tasks.size()) + L" 步";
                }
            }
            SetChip(s, ui::StatusAiState::Busy, L"正在整理", p);
            Redraw(s);
        }
        if (finished == a.organ->tasks.size()) FinishOrganizeQueue(s);
        return;
    }
    if (a.delete_job) {
        size_t finished = 0, failed = 0;
        for (const uint64_t id : a.delete_job->tasks) {
            if (const std::optional<ops::OpStatus> st = s.ops.FinalStatus(id)) {
                ++finished;
                failed += st->failed_items;
            }
        }
        if (finished != a.delete_job->finished || failed != a.delete_job->failed) {
            a.delete_job->finished = finished;
            a.delete_job->failed = failed;
            const float p = a.delete_job->tasks.empty()
                ? 1.0f
                : static_cast<float>(finished) / static_cast<float>(a.delete_job->tasks.size());
            if (ui::AiBlockView* b = Find(s, a.delete_job->step_block)) {
                if (!b->steps.empty()) b->steps.back().progress = p;
            }
            SetChip(s, ui::StatusAiState::Busy, L"正在删除", p);
            Redraw(s);
        }
        if (finished == a.delete_job->tasks.size()) FinishDeleteQueue(s);
        return;
    }
    if (a.rename_job) {
        size_t finished = 0, failed = 0;
        for (const uint64_t id : a.rename_job->tasks) {
            if (const std::optional<ops::OpStatus> st = s.ops.FinalStatus(id)) {
                ++finished;
                failed += st->failed_items;
            }
        }
        if (finished != a.rename_job->finished || failed != a.rename_job->failed) {
            a.rename_job->finished = finished;
            a.rename_job->failed = failed;
            const float p = a.rename_job->tasks.empty()
                ? 1.0f
                : static_cast<float>(finished) / static_cast<float>(a.rename_job->tasks.size());
            if (ui::AiBlockView* b = Find(s, a.rename_job->step_block)) {
                if (!b->steps.empty()) b->steps.back().progress = p;
            }
            SetChip(s, ui::StatusAiState::Busy, L"正在重命名", p);
            Redraw(s);
        }
        if (finished == a.rename_job->tasks.size()) FinishRenameQueue(s);
        return;
    }
    // An undo of the result card runs through the same queue.
    if (a.undo_wait) {
        if (!s.ops.Status().active && GetTickCount64() - a.undo_started > 300) {
            if (ui::AiBlockView* r = Find(s, a.undo_block)) {
                r->done = true;
                size_t back = 0;
                for (const std::wstring& path : a.undo_paths)
                    if (PathExists(path)) ++back;
                if (a.undo_paths.empty() || back == a.undo_paths.size()) {
                    r->title = Text(L"已撤销，文件回到原处");
                } else if (back == 0) {
                    r->title = Text(L"没能撤销，回收站里找不到这些条目");
                } else {
                    r->title = L"撤销了一部分：" + std::to_wstring(back) + L" / " +
                               std::to_wstring(a.undo_paths.size());
                }
            }
            a.undo_paths.clear();
            a.undo_wait = false;
            a.undo_group = 0;
            a.undo_block = 0;
            SetChip(s, ui::StatusAiState::Idle, nullptr);
            Redraw(s);
        }
        return;
    }
    // A tool running on its worker: take it as soon as it is done.
    if (a.job) {
        if (a.job->done.load(std::memory_order_acquire)) {
            app::ai::ToolOutcome out = std::move(a.job->out);
            FinishTool(s, std::move(out));
        }
        return;
    }
    if (!a.stream) {
        ArmPump(s, false);
        return;
    }
    std::vector<app::ai::ModelEvent> events;
    a.stream->Drain(events);
    for (app::ai::ModelEvent& ev : events) {
        switch (ev.kind) {
        case app::ai::ModelEvent::Kind::Reasoning:
            a.reasoning += ev.text;
            if (ui::AiBlockView* b = Find(s, a.think_id)) b->text = json::Wide(a.reasoning);
            break;
        case app::ai::ModelEvent::Kind::Content: {
            FoldThinking(s, a);
            FoldSteps(s, a);
            a.content += ev.text;
            ui::AiBlockView* t = Find(s, a.text_id);
            if (!t) {
                ui::AiBlockView& nt = Add(s, ui::AiBlockKind::Text);
                nt.streaming = true;
                a.text_id = nt.id;
                t = &nt;
            }
            t->text = Display(a.content);
            break;
        }
        case app::ai::ModelEvent::Kind::Finished: {
            FoldThinking(s, a);
            if (ui::AiBlockView* t = Find(s, a.text_id)) {
                t->streaming = false;
                t->text = Display(a.content);
            }
            a.text_id = 0;
            json::Value msg = json::Value::Object();
            msg.set("role", "assistant");
            msg.set("content", ev.content);
            if (!ev.reasoning.empty()) msg.set("reasoning_content", ev.reasoning);
            if (!ev.tool_calls.empty()) {
                json::Value calls = json::Value::Array();
                for (const auto& tc : ev.tool_calls) {
                    json::Value call = json::Value::Object();
                    call.set("id", tc.id);
                    call.set("type", "function");
                    json::Value fn = json::Value::Object();
                    fn.set("name", tc.name);
                    fn.set("arguments", tc.arguments);
                    call.set("function", std::move(fn));
                    calls.push(std::move(call));
                }
                msg.set("tool_calls", std::move(calls));
            }
            a.history.push(std::move(msg));
            a.stream.reset();
            if (ev.tool_calls.empty()) {
                EndTurn(s);
            } else {
                a.calls = std::move(ev.tool_calls);
                a.call_index = 0;
                StartNextTool(s);
            }
            Redraw(s);
            return;
        }
        case app::ai::ModelEvent::Kind::Failed:
            Fail(s, a, ev.http_status, ev.win32_error, ev.error_type, ev.error_message);
            ArmPump(s, false);
            return;
        }
    }
    if (a.stream && a.stream->Ended()) {
        FailCut(s, a);
        ArmPump(s, false);
    }
    Redraw(s);
}

void AiAgentStop(AppState& s) {
    AiAssistantState& as = s.aiAssistant;
    if (!as.agent) return;
    AiAgent& a = *as.agent;
    if (!a.busy) return;
    ++a.generation;   // late callbacks drop out
    if (a.stream) a.stream->Cancel();
    a.stream.reset();
    if (a.job) {
        a.job->cancelled.store(true);
        a.job.reset();
    }
    a.calls.clear();
    a.call_index = 0;
    if (a.organ) {
        // The moves already queued keep running in the app's own queue (and
        // stay undoable there); this turn just stops following them.
        if (ui::AiBlockView* b = Find(s, a.organ->step_block)) {
            if (!b->steps.empty()) {
                b->steps.back().state = ui::AiStepState::Failed;
                b->steps.back().sub = Text(L"已停止，剩下的操作还在队列里");
            }
        }
        a.organ.reset();
    }
    if (a.awaiting_delete) {
        if (ui::AiBlockView* cf = Find(s, a.confirm_block)) {
            cf->done = true;
            cf->declined = true;
            cf->title = L"已停止，什么都没动";
        }
        a.awaiting_delete = false;
        a.confirm_block = 0;
        a.del.reset();
    }
    if (a.delete_job) {
        if (ui::AiBlockView* b = Find(s, a.delete_job->step_block)) {
            if (!b->steps.empty()) {
                b->steps.back().state = ui::AiStepState::Failed;
                b->steps.back().sub = Text(L"已停止，删除还在队列里");
            }
        }
        a.delete_job.reset();
    }
    if (a.awaiting_rename) {
        if (ui::AiBlockView* cf = Find(s, a.confirm_block)) {
            cf->done = true;
            cf->declined = true;
            cf->title = L"已停止，什么都没动";
        }
        a.awaiting_rename = false;
        a.confirm_block = 0;
        a.rename.reset();
    }
    if (a.rename_job) {
        if (ui::AiBlockView* b = Find(s, a.rename_job->step_block)) {
            if (!b->steps.empty()) {
                b->steps.back().state = ui::AiStepState::Failed;
                b->steps.back().sub = Text(L"已停止，重命名还在队列里");
            }
        }
        a.rename_job.reset();
    }
    if (a.awaiting) {
        if (ui::AiBlockView* cf = Find(s, a.confirm_block)) {
            cf->done = true;
            cf->declined = true;
            cf->title = L"已停止，什么都没动";
        }
        a.awaiting = false;
        a.confirm_block = 0;
        a.plan.reset();
    }
    FoldThinking(s, a);
    if (ui::AiBlockView* t = Find(s, a.text_id)) t->streaming = false;
    a.text_id = 0;
    a.busy = false;
    as.busy = false;
    SetChip(s, ui::StatusAiState::Idle, nullptr);
    ArmPump(s, false);
    Redraw(s);
}

void AiAgentReset(AppState& s) {
    AiAssistantState& as = s.aiAssistant;
    if (as.agent) {
        ++as.agent->generation;
        if (as.agent->stream) as.agent->stream->Cancel();
        as.agent->stream.reset();
        if (as.agent->job) {
            as.agent->job->cancelled.store(true);
            as.agent->job.reset();
        }
        as.agent->history = json::Value::Array();
        as.agent->busy = false;
        as.agent->last_prompt.clear();
        as.agent->think_id = as.agent->text_id = 0;
        as.agent->calls.clear();
        as.agent->call_index = 0;
        as.agent->rounds = 0;
        as.agent->steps_id = 0;
        as.agent->awaiting = false;
        as.agent->confirm_block = 0;
        as.agent->plan.reset();
        as.agent->organ.reset();
        as.agent->rename.reset();
        as.agent->rename_job.reset();
        as.agent->del.reset();
        as.agent->delete_job.reset();
        as.agent->awaiting_delete = false;
        as.agent->delete_block = 0;
        as.agent->awaiting_rename = false;
        as.agent->rename_block = 0;
        as.agent->undo_group = 0;
        as.agent->undo_block = 0;
        as.agent->undo_wait = false;
        as.agent->undo_paths.clear();
    }
    as.busy = false;
    SetChip(s, ui::StatusAiState::Idle, nullptr);
    as.unread = false;
    AiPeekHide(s);
    ArmPump(s, false);
}

void AiAgentRetry(AppState& s) {
    AiAgent& a = Agent(s);
    if (a.last_prompt.empty()) return;
    const std::wstring prompt = a.last_prompt;
    // The failed turn's card goes; the user's question stays where it was.
    auto& blocks = s.aiAssistant.blocks;
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                                [](const ui::AiBlockView& b) { return b.kind == ui::AiBlockKind::Error; }),
                 blocks.end());
    // The question is already in the history: resend without adding it again.
    if (a.history.is_array() && a.history.size() > 0) {
        json::Value trimmed = json::Value::Array();
        const auto& items = a.history.items();
        size_t keep = items.size();
        if (keep && items.back().get_string("role") == "user") --keep;
        for (size_t i = 0; i < keep; ++i) trimmed.push(items[i]);
        a.history = std::move(trimmed);
    }
    json::Value user = json::Value::Object();
    user.set("role", "user");
    user.set("content", json::Utf8(prompt));
    a.history.push(std::move(user));

    std::wstring key;
    if (!app::ai::LoadModelKey(key)) {
        NoKey(s);
        return;
    }
    a.reasoning.clear();
    a.content.clear();
    a.think_id = 0;
    a.text_id = 0;
    a.busy = true;
    a.rounds = 0;
    a.calls.clear();
    a.call_index = 0;
    a.steps_id = 0;
    s.aiAssistant.turn_start = Now();
    s.aiAssistant.peek_text.clear();
    s.aiAssistant.peek_title.clear();
    s.aiAssistant.peek_failed = false;
    s.aiAssistant.busy = true;
    s.aiAssistant.unread = false;
    SetChip(s, ui::StatusAiState::Busy, L"思考中");
    SendRound(s);
}

void AiAgentErrorAction(AppState& s, int block, int button) {
    AiAssistantState& as = s.aiAssistant;
    if (block < 0 || block >= static_cast<int>(as.blocks.size())) return;
    ui::AiBlockView& b = as.blocks[static_cast<size_t>(block)];
    if (button < 0 || button >= static_cast<int>(b.actions.size())) return;
    switch (b.actions[static_cast<size_t>(button)].act) {
    case ui::AiErrAct::Retry:
        b.gone_ms = Now();   // fades out this frame; AiAgentRetry drops it
        AiAgentRetry(s);
        break;
    case ui::AiErrAct::ManageConn:
        as.menu_open = true;
        AiAssistantOpen(s);
        AiConnOpen(s);
        Redraw(s);
        break;
    default:
        break;
    }
}

void AiAgentDestroy(AppState& s) {
    if (s.aiAssistant.agent) {
        ++s.aiAssistant.agent->generation;
        if (s.aiAssistant.agent->stream) s.aiAssistant.agent->stream->Cancel();
        s.aiAssistant.agent->stream.reset();
        if (s.aiAssistant.agent->job) {
            s.aiAssistant.agent->job->cancelled.store(true);
            s.aiAssistant.agent->job.reset();
        }
        s.aiAssistant.agent->organ.reset();
        s.aiAssistant.agent->rename.reset();
        s.aiAssistant.agent->rename_job.reset();
        s.aiAssistant.agent->del.reset();
        s.aiAssistant.agent->delete_job.reset();
    }
    ArmPump(s, false);
}

} // namespace luling
