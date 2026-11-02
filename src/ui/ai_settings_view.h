// ai_settings_view.h - what Settings > AI 助手 draws, ported from Pulse 1.0.58
// ui/ai_settings_view.h: the MCP switch, the action groups, the apps that
// connected, the always-allow rules and this run's calls.
#pragma once

#include <d2d1.h>

#include <cstdint>
#include <string>
#include <vector>

namespace luling::ui {

// HitTestResult::SettingsAi indices.
enum AiHit : int {
    kAiToggle = 0,            // 允许 AI 应用连接
    kAiCopyConfig = 1,        // 复制配置
    kAiOpenLog = 2,           // 打开活动记录
    kAiUninstall = 3,         // 卸载 AI 增强包
    kAiClientAllowBase = 100,
    kAiClientDenyBase = 200,
    kAiClientForgetBase = 300,
    kAiRuleRemoveBase = 400,
    kAiGroupBase = 500,
    kAiRowLimit = 99,
};

enum class AiGrantView : uint8_t { Ask, Allowed, Session, Denied };

struct AiClientRowView {
    std::wstring name;
    AiGrantView grant = AiGrantView::Ask;
    bool connected = false;
};

struct AiRuleRowView {
    std::wstring client;
    std::string tool;
    std::wstring tool_label;
    std::wstring folder;
};

enum class AiCallTone : uint8_t { Done, Running, Declined, Failed };

struct AiCallRowView {
    std::wstring time;
    std::wstring client;
    std::wstring tool_label;
    std::wstring outcome;
    std::wstring summary;
    AiCallTone tone = AiCallTone::Done;
};

enum class AiGroupTag : uint8_t { None, DefaultOn, Asks, AsksOutside };

struct AiGroupRowView {
    int group = 0;
    std::wstring icon;
    std::wstring title, count, desc;
    AiGroupTag tag = AiGroupTag::None;
    bool on = false;
    bool locked = false;
};

struct AiPageView {
    bool enabled = false;
    bool listening = false;
    bool broken = false;
    bool can_uninstall = false;
    bool can_copy = false;   // Luling.AI.exe is present
    std::wstring status;
    std::vector<AiClientRowView> clients;
    std::vector<AiRuleRowView> rules;
    std::vector<AiGroupRowView> groups;
    std::vector<AiCallRowView> calls;
    size_t more_calls = 0;
};

inline constexpr size_t kAiPageCalls = 12;

struct AiClientRowLayout {
    D2D1_RECT_F row{}, allow{}, deny{}, forget{};
};

struct AiPageLayout {
    D2D1_RECT_F main_card{}, enable_row{}, toggle{}, setup_row{}, copy{}, broken{};
    D2D1_RECT_F groups_section{}, groups_card{}, groups_foot{};
    std::vector<D2D1_RECT_F> groups, group_toggles;
    D2D1_RECT_F apps_section{}, apps_card{}, apps_empty{};
    std::vector<AiClientRowLayout> clients;
    D2D1_RECT_F rules_section{}, rules_card{}, rules_empty{};
    std::vector<D2D1_RECT_F> rules, rule_remove;
    D2D1_RECT_F recent_section{}, open_log{}, recent_card{}, recent_empty{}, more_calls{};
    std::vector<D2D1_RECT_F> calls;
    D2D1_RECT_F pack_section{}, uninstall_row{}, uninstall{};
};

} // namespace luling::ui
