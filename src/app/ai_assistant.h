// ai_assistant.h - 鹿铃助手: panel state, composer EDIT, pointer routing and
// the view model ui::AiPanelView draws. Ported from Pulse 1.0.58
// (app/ai_assistant.h); the chat driver is not wired yet.
#pragma once
#include "../ui/ai_panel_view.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace luling {
struct AppState;
struct AiConnUi;  // 管理连接 card: fields, test, key (ai_assistant_conn.h)
struct AiSettingsEdits;  // Settings > AI 助手 page 12: key/address edits, probe
struct AiAgent;   // the turn driver (ai_agent.cpp)
namespace ui {
struct WindowViewModel;
}

struct AiAssistantState {
    bool open = false;
    bool floating = false;
    bool prefs_loaded = false;
    bool conversation_loaded = false;  // conversation.json read (ai_conversation.h)
    int model = 0;                 // index into the model list
    int effort = 2;                // 0 off .. 3 high
    bool menu_open = false;
    uint64_t menu_closed_at = 0;
    std::wstring context_removed_for;
    // Pointer.
    ui::AiPanelHit hover;
    ui::AiPanelHit pressed;
    POINT hover_point{};
    bool captured = false;
    // Floating card drag (header): start point and the offset when it began.
    bool float_drag = false;
    POINT drag_start{};
    float drag_dx = 0.0f, drag_dy = 0.0f;
    // Composer EDIT.
    HWND edit = nullptr;
    HFONT font = nullptr;
    float font_scale = 0.0f;
    HBRUSH brush = nullptr;
    COLORREF brush_color = 0;
    COLORREF edit_back = CLR_INVALID;
    COLORREF edit_fore = CLR_INVALID;
    std::wstring edit_cue;
    bool edit_visible = false;
    bool focus_pending = false;
    bool focused = false;
    int lines = 1;
    int text_len = 0;
    int line_px = 0;
    // Conversation (text blocks for now).
    std::vector<ui::AiBlockView> blocks;
    uint32_t next_block_id = 1;
    bool busy = false;
    // Connection / models.
    bool connected = false;
    std::wstring connection_text;
    std::vector<ui::AiModelView> models;
    std::wstring model_id = L"deepseek-flash";
    std::wstring model_label = L"deepseek-flash";
    std::wstring effort_label;
    std::shared_ptr<AiConnUi> conn;
    std::shared_ptr<AiSettingsEdits> settings_edits;
    std::shared_ptr<AiAgent> agent;
    // The status bar chip while a turn runs (ui::StatusAiState).
    ui::StatusAiState chip = ui::StatusAiState::Idle;
    std::wstring chip_text;
    float chip_progress = -1.0f;
    bool unread = false;
    bool chip_pressed = false;
    // The peek bubble over the chip (ai_peek.h).
    uint64_t turn_start = 0;      // ui::motion clock of the turn
    uint64_t peek_at = 0;         // shows from (0: none)
    uint64_t peek_until = 0;
    uint64_t peek_turn = 0;       // the turn it was shown for
    std::wstring peek_text;
    std::wstring peek_title;
    bool peek_failed = false;
    ui::AiErrTone peek_tone = ui::AiErrTone::Err;
};

void AiAssistantToggle(AppState& s);
void AiAssistantHotkey(AppState& s);
void AiAssistantOpen(AppState& s, bool animate = true);
void AiAssistantClose(AppState& s, bool animate = true);
void FillAiAssistantVm(AppState& s, ui::WindowViewModel& vm);
void AiAssistantLayout(AppState& s);
bool AiAssistantMouseMove(AppState& s, int x, int y);
bool AiAssistantMouseDown(AppState& s, int x, int y);
bool AiAssistantMouseUp(AppState& s, int x, int y);
bool AiAssistantWheel(AppState& s, int x, int y, int delta);
bool AiAssistantSetCursor(AppState& s, int x, int y);
bool AiAssistantEscape(AppState& s);
void AiAssistantMouseLeave(AppState& s);
void AiAssistantDestroy(AppState& s);
HBRUSH AiAssistantCtlColor(AppState& s, HDC hdc, HWND edit);


// The model service a request would use (the chat driver, once ported, owns
// the rest of the request). For now: the key and the address live in ai.json
// and model_key.bin, which the 管理连接 card writes.
bool AiAgentHasKey();
void AiAgentKeyChanged();               // after the key or the address was saved or removed
std::wstring AiAgentBaseUrl();          // ai.json assistant.base_url, else the default
std::wstring AiAgentDefaultBaseUrl();   // https://api.deepseek.com
std::wstring AiAgentHost();             // the host of AiAgentBaseUrl()
std::wstring AiAssistantModelName(const AppState& s);  // the model, as the service names it

} // namespace luling
