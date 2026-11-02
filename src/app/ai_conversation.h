// ai_conversation.h - The assistant's chat, kept between launches in
// %LOCALAPPDATA%\Luling\ai\conversation.json: the user's messages and the
// answers, in order (the cards in between are rebuilt by the turn, so only
// text survives). Last 40 entries. UI thread only.
#pragma once
#include <string>

namespace luling {
struct AppState;
namespace app::ai {

std::wstring ConversationPath();
// Restores the saved turns into AiAssistantState::blocks (once, after prefs).
void LoadConversation(AppState& s);
// Writes the current user/answer text; nothing before LoadConversation ran.
void SaveConversation(AppState& s);
void ClearConversation();

} // namespace app::ai
} // namespace luling
