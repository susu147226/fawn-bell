// ai_settings_ui.cpp - Settings > AI 助手 (page 12) data and clicks.
#include "ai_settings_ui.h"

#include "ai_assistant.h"
#include "ai_broker.h"
#include "ai_model_key.h"
#include "ai_model_list.h"
#include "ai_providers.h"
#include "ai_settings.h"
#include "ai_settings_edits.h"
#include "app_internal.h"
#include "../common/json_value.h"
#include "../ui/ai_model_view.h"
#include "../ui/ai_settings_view.h"

#include <windows.h>

#include <memory>
#include <string>

namespace luling {
namespace {

struct GroupDef {
    const char* key;
    const wchar_t* icon;
    const wchar_t* title;
    const wchar_t* count;
    const wchar_t* desc;
    ui::AiGroupTag tag;
};

// Luling's tools, one group each (Pulse's five action groups do not exist
// here yet; the tool set is what the AI apps can use).
constexpr GroupDef kGroups[] = {
    {"view", L"\xE721", L"查看", L"get_state · list_folder",
     L"看当前窗口和文件夹：只读，不改任何东西。", ui::AiGroupTag::DefaultOn},
    {"organize", L"\xE8B7", L"整理", L"organize_files",
     L"把文件按类型移进子文件夹；方案要你在鹿铃里点确认。", ui::AiGroupTag::Asks},
    {"rename", L"\xE8AC", L"重命名", L"rename_files",
     L"按规则改名；新旧名字都由你看过再确认。", ui::AiGroupTag::Asks},
    {"delete", L"\xE74D", L"删除", L"delete_files",
     L"移到回收站或永久删除；永久删除要点两次，系统目录一律拒绝。", ui::AiGroupTag::AsksOutside},
    {"files", L"\xE8DE", L"文件", L"copy_files · move_files · create_folder",
     L"复制、移动和新建文件夹；执行前都由你在鹿铃里确认。", ui::AiGroupTag::Asks},
    {"settings", L"\xE713", L"设置", L"set_setting",
     L"按你的要求改鹿铃设置；每次都会先问你。", ui::AiGroupTag::Asks},
};
constexpr int kGroupCount = static_cast<int>(std::size(kGroups));

} // namespace

std::shared_ptr<const ui::AiPageView> BuildAiPage(AppState& s) {
    bool malformed = false;
    const app::ai::Settings st = app::ai::LoadSettings(&malformed);
    auto v = std::make_shared<ui::AiPageView>();
    v->enabled = st.mcp_enabled;
    v->listening = AiBrokerListening();
    v->broken = malformed;
    v->can_uninstall = false;
    v->can_copy = !AiHostPath().empty();
    v->status = st.mcp_enabled
        ? (v->can_copy
               ? (v->listening ? std::wstring(L"已开启 · AI 宿主正在运行")
                               : std::wstring(L"已开启 · 启动 Luling.AI.exe 的应用可以连接"))
               : std::wstring(L"已开启 · 找不到 Luling.AI.exe（AI 增强包没有装齐）"))
        : std::wstring(L"关闭：外部 AI 应用无法连接。");
    int group = 0;
    for (const GroupDef& def : kGroups) {
        ui::AiGroupRowView g;
        g.group = group++;
        g.icon = def.icon;
        g.title = def.title;
        g.count = def.count;
        g.desc = def.desc;
        g.tag = def.tag;
        g.on = app::ai::McpAllows(st, def.key);
        v->groups.push_back(std::move(g));
    }
    for (const app::ai::ClientRecord& c : st.clients) {
        ui::AiClientRowView row;
        row.name = c.name;
        row.grant = c.grant == app::ai::ClientGrant::Allowed ? ui::AiGrantView::Allowed
                  : c.grant == app::ai::ClientGrant::Denied ? ui::AiGrantView::Denied
                                                            : ui::AiGrantView::Ask;
        row.connected = AiBrokerIsOnline(c.name);
        v->clients.push_back(std::move(row));
    }
    for (const app::ai::AllowRule& r : st.allow) {
        ui::AiRuleRowView row;
        row.client = r.client;
        row.tool = r.tool;
        row.tool_label = std::wstring(r.tool.begin(), r.tool.end());
        row.folder = r.folder;
        v->rules.push_back(std::move(row));
    }
    size_t more = 0;
    for (const app::ai::ActivityEntry& e : AiReadActivity(ui::kAiPageCalls, more)) {
        ui::AiCallRowView row;
        row.time = e.time;
        row.client = e.client;
        row.tool_label = e.tool;
        row.summary = e.summary;
        row.outcome = e.declined ? L"已拒绝" : e.failed ? L"失败" : L"完成";
        row.tone = e.declined ? ui::AiCallTone::Declined
                  : e.failed ? ui::AiCallTone::Failed
                             : ui::AiCallTone::Done;
        v->calls.push_back(std::move(row));
    }
    v->more_calls = more;
    (void)s;
    return v;
}

void HandleAiSettings(AppState& s, int hit) {
    bool malformed = false;
    app::ai::Settings st = app::ai::LoadSettings(&malformed);
    if (malformed) return;   // never overwrite a file we could not read
    bool changed = false;
    if (hit == ui::kAiToggle) {
        st.mcp_enabled = !st.mcp_enabled;
        changed = true;
    } else if (hit >= ui::kAiGroupBase && hit < ui::kAiGroupBase + kGroupCount) {
        const int index = hit - ui::kAiGroupBase;
        const char* key = kGroups[index].key;
        app::ai::SetMcpAllow(st, key, !app::ai::McpAllows(st, key));
        changed = true;
    } else if (hit >= ui::kAiClientAllowBase && hit < ui::kAiClientAllowBase + 99) {
        const size_t index = static_cast<size_t>(hit - ui::kAiClientAllowBase);
        if (index < st.clients.size()) {
            st.clients[index].grant = app::ai::ClientGrant::Allowed;
            changed = true;
        }
    } else if (hit >= ui::kAiClientDenyBase && hit < ui::kAiClientDenyBase + 99) {
        const size_t index = static_cast<size_t>(hit - ui::kAiClientDenyBase);
        if (index < st.clients.size()) {
            st.clients[index].grant = app::ai::ClientGrant::Denied;
            changed = true;
        }
    } else if (hit >= ui::kAiClientForgetBase && hit < ui::kAiClientForgetBase + 99) {
        const size_t index = static_cast<size_t>(hit - ui::kAiClientForgetBase);
        if (index < st.clients.size()) {
            st.clients.erase(st.clients.begin() + static_cast<ptrdiff_t>(index));
            changed = true;
        }
    } else if (hit == ui::kAiCopyConfig) {
        if (!AiHostPath().empty()) (void)AiCopyMcpConfig(s);
    } else if (hit == ui::kAiOpenLog) {
        AiOpenActivityLog(s);
    } else if (hit >= ui::kAiRuleRemoveBase && hit < ui::kAiRuleRemoveBase + 99) {
        const size_t index = static_cast<size_t>(hit - ui::kAiRuleRemoveBase);
        if (index < st.allow.size()) {
            st.allow.erase(st.allow.begin() + static_cast<ptrdiff_t>(index));
            changed = true;
        }
    }
    if (changed) (void)app::ai::SaveSettings(st);
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

namespace {

const app::ai::ProviderInfo* FindProviderById(std::string_view id) {
    for (const app::ai::ProviderInfo& p : app::ai::Providers())
        if (p.id == id) return &p;
    return nullptr;
}

// "https://platform.deepseek.com/api_keys" -> "platform.deepseek.com".
std::wstring ConsoleHost(std::wstring_view url) {
    const size_t scheme = url.find(L"://");
    std::wstring_view rest = scheme == std::wstring_view::npos ? url : url.substr(scheme + 3);
    const size_t slash = rest.find_first_of(L"/?#");
    std::wstring host(rest.substr(0, slash));
    if (host.size() > 4 && host.starts_with(L"www.")) host.erase(0, 4);
    return host;
}

// The address the page shows and the key applies to.
std::wstring ServiceBaseUrl(const app::ai::Settings& st) {
    std::wstring base = json::Wide(st.assistant.base_url);
    if (base.empty()) base = std::wstring(app::ai::DefaultProvider().base_url);
    return app::ai::NormalizeBaseUrl(std::move(base));
}

const app::ai::ProviderInfo* DisplayedProvider(int tile, bool more) {
    int n = 0;
    for (const app::ai::ProviderInfo& p : app::ai::Providers()) {
        if (p.in_more != more) continue;
        if (n++ == tile) return &p;
    }
    return nullptr;
}

} // namespace

std::shared_ptr<const ui::AiModelPageView> BuildAiModelPage(AppState& s) {
    bool malformed = false;
    const app::ai::Settings st = app::ai::LoadSettings(&malformed);
    const AiSettingsEdits* ui = s.aiAssistant.settings_edits.get();
    auto v = std::make_shared<ui::AiModelPageView>();
    v->broken = malformed;

    std::wstring key;
    const bool has_key = app::ai::LoadModelKey(key) && !key.empty();
    if (has_key) v->key_mask = app::ai::MaskModelKey(key);
    if (!key.empty()) SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));

    const std::wstring stored = json::Wide(st.assistant.base_url);
    const app::ai::ProviderInfo* prov = nullptr;
    if (!st.assistant.provider.empty()) prov = FindProviderById(st.assistant.provider);
    if (!prov && !stored.empty()) prov = app::ai::ProviderByBase(stored);
    if (!prov && stored.empty()) prov = has_key ? &app::ai::DefaultProvider() : nullptr;
    if (!prov && !stored.empty()) prov = FindProviderById("custom");

    const bool chosen = prov && !(ui && ui->grid_open);
    if (chosen) {
        v->chosen = true;
        v->provider_id = std::string(prov->id);
        v->provider_name = std::wstring(prov->name);
        v->provider_vendor = std::wstring(prov->vendor);
        v->needs_key = prov->needs_key;
        if (!prov->console_url.empty()) {
            v->console_label = ConsoleHost(prov->console_url);
            v->has_console = true;
        }
        std::wstring base;
        if (!stored.empty()) base = app::ai::NormalizeBaseUrl(stored);
        else if (prov->id != "custom") base = std::wstring(prov->base_url);
        const bool is_default = !prov->base_url.empty() && app::ai::SameBaseUrl(base, std::wstring(prov->base_url));
        v->base_text = base.empty() ? std::wstring(L"还没有填写地址")
                                    : (is_default ? base + L"（默认）" : base);
        v->model_label = std::wstring(st.assistant.model.begin(), st.assistant.model.end());
        for (const std::string& id : app::ai::ListedModels(base))
            v->model_choices.emplace_back(id.begin(), id.end());
        if (v->model_choices.empty())
            v->model_note = has_key ? L"还没有读到模型列表，点上面的连接" : L"先填 API Key 再连接";
        v->key_editing = ui && ui->key_open;
        v->key_revealed = ui && ui->key_revealed;
        v->base_editing = ui && ui->base_open;
        v->connecting = ui && ui->probing;
        v->can_connect = has_key;
        if (ui && ui->probing) {
            v->connect_status = L"正在连接 " + app::ai::ModelHost(base) + L"…";
        } else if (ui && ui->probed) {
            v->connect_ok = ui->probe_ok;
            if (ui->probe_ok) {
                v->connect_status = L"连接正常 · " + std::to_wstring(ui->ms) + L" 毫秒 · 读到 " +
                                    std::to_wstring(ui->model_count) + L" 个模型";
            } else if (ui->status_code == 401) {
                v->connect_status = L"密钥无效（401）";
                v->connect_detail = L"检查是否复制完整，或重新创建一个。";
            } else if (ui->status_code == 0) {
                v->connect_status = L"连不上 " + app::ai::ModelHost(base);
            } else {
                v->connect_status = L"连接失败（" + std::to_wstring(ui->status_code) + L"）";
            }
        }
    } else {
        if (ui && ui->probing) v->badge = L"正在连接…";
        else if (ui && ui->probed) v->badge = ui->probe_ok ? L"已连接" : L"连接失败";
        else v->badge = has_key ? L"已连接" : L"未连接";
    }

    int index = 0;
    for (const app::ai::ProviderInfo& p : app::ai::Providers()) {
        ui::AiProviderTile t;
        t.id = std::string(p.id);
        t.name = std::wstring(p.name);
        t.vendor = std::wstring(p.vendor);
        t.mark = t.name.substr(0, 1);
        t.color = p.color;
        t.index = index++;
        t.selected = prov && prov->id == p.id;
        if (p.in_more) v->more.push_back(std::move(t));
        else v->providers.push_back(std::move(t));
    }

    v->pack_installed = !AiHostPath().empty();
    v->pack_state = v->pack_installed ? L"已安装" : L"未安装";
    return v;
}

void HandleAiModelSettings(AppState& s, int hit) {
    auto& slot = s.aiAssistant.settings_edits;
    if (!slot) slot = std::make_shared<AiSettingsEdits>();
    AiSettingsEdits& u = *slot;
    bool malformed = false;
    app::ai::Settings st = app::ai::LoadSettings(&malformed);
    if (malformed) return;   // never overwrite a file we could not read

    const app::ai::ProviderInfo* picked = nullptr;
    if (hit >= ui::kAiModelTileBase && hit < ui::kAiModelTileBase + 99)
        picked = DisplayedProvider(hit - ui::kAiModelTileBase, false);
    else if (hit >= ui::kAiModelMoreBase && hit < ui::kAiModelMoreBase + 99)
        picked = DisplayedProvider(hit - ui::kAiModelMoreBase, true);
    if (picked) {
        st.assistant.provider = std::string(picked->id);
        if (picked->id == "custom") {
            st.assistant.base_url.clear();
        } else {
            const std::wstring base(picked->base_url);
            st.assistant.base_url =
                app::ai::SameBaseUrl(base, std::wstring(app::ai::DefaultProvider().base_url))
                    ? std::string() : json::Utf8(base);
        }
        u.grid_open = false;
        u.probing = false;
        u.probed = false;
        ++u.gen;
        u.job.reset();
        if (app::ai::SaveSettings(st)) AiAgentKeyChanged();
        if (picked->id == "custom") AiSettingsBeginBaseEdit(s);
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }

    if (hit >= ui::kAiModelChoiceBase && hit < ui::kAiModelChoiceBase + 99) {
        const size_t index = static_cast<size_t>(hit - ui::kAiModelChoiceBase);
        const auto& models = app::ai::ListedModels(ServiceBaseUrl(st));
        if (index < models.size()) {
            st.assistant.model = models[index];
            if (app::ai::SaveSettings(st)) AiAgentKeyChanged();
        }
        if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
        return;
    }

    switch (hit) {
    case ui::kAiModelSwitchService:
        u.grid_open = true;
        break;
    case ui::kAiModelKeyField:
        AiSettingsBeginKeyEdit(s);
        return;
    case ui::kAiModelKeyEye:
        AiSettingsToggleReveal(s);
        return;
    case ui::kAiModelBaseField:
        AiSettingsBeginBaseEdit(s);
        return;
    case ui::kAiModelConnect:
        AiSettingsProbeConnect(s);
        return;
    case ui::kAiModelOpenPack:
        s.settings.SelectPage(14);
        break;
    default:
        break;
    }
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace luling
