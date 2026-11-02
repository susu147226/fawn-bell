// ai_broker.h - The MCP broker inside luling.exe: a named-pipe server that
// applies ai.json's permissions, asks the user before anything changes, runs
// the five tools, and writes the activity log. Luling.AI.exe is the stdio
// side of the same protocol (src/ai_host).
#pragma once

#include "../common/json_value.h"
#include "ai_tools.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace luling {
struct AppState;

namespace app::ai {

struct ActivityEntry {
    std::wstring time;      // "14:32"
    std::wstring client;
    std::wstring tool;      // localized title
    std::wstring summary;
    bool failed = false;
    bool declined = false;
};

// Work the broker needs the UI thread for: reading the open window, the
// selection, or asking a confirmation question.
struct BrokerUiCall {
    enum class Kind { ReadState, Selection, Confirm, Folder, Navigate, ListTabs, GetSettings, SetSetting };
    Kind kind = Kind::ReadState;
    std::string tool;
    json::Value args;
    // Confirm: a Luling-style card (ui/confirm_dialog.h).
    std::wstring prompt;          // kept for simple questions
    std::wstring title, message, confirm_text, cancel_text, note, glyph;
    std::vector<std::wstring> items;
    int tone = 0;                 // ui::ConfirmTone: 0 auto, 1 info, 2 question, 3 warning, 4 danger
    bool danger = false;
    bool default_confirm = false; // Enter confirms; destructive cards leave it false
    bool secondary_allow = false; // show 始终允许 as the middle button
    bool always = false;          // the user picked 始终允许
    bool confirmed = false;
    // Navigation / settings work.
    std::wstring path;
    bool new_tab = true;
    bool reveal = false;
    std::wstring key, value;
    ToolOutcome outcome;
    std::vector<std::wstring> selection;
    std::wstring folder;
};

} // namespace app::ai

// Window message the broker uses to run a BrokerUiCall on the UI thread.
inline constexpr UINT kAiBrokerUiMessage = WM_APP + 0x53;
LRESULT AiBrokerHandleUiCall(AppState& s, app::ai::BrokerUiCall* call);

void AiBrokerStart(AppState& s);   // idempotent; the thread lives until exit
bool AiBrokerListening();
// True while a host connection from that client is open.
bool AiBrokerIsOnline(const std::wstring& client);

// Luling.AI.exe beside luling.exe; empty when the pack is missing.
std::wstring AiHostPath();
std::wstring AiActivityLogPath();

// Settings > AI 与 MCP: the newest entries first, plus how many older ones
// were not returned.
std::vector<app::ai::ActivityEntry> AiReadActivity(size_t limit, size_t& more);
// The MCP configuration to paste into Claude/other clients.
std::string AiMcpConfigJson();
bool AiCopyMcpConfig(AppState& s);
void AiOpenActivityLog(AppState& s);

} // namespace luling
