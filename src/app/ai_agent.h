// ai_agent.h - 鹿铃助手's turn driver: one streamed answer from an
// OpenAI-format service (ai_model_stream.h), turned into conversation blocks.
// Ported from the earlier implementation app/ai_assistant_agent.cpp without the tool loop:
// this version answers in text only, so the model is not offered tools.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace luling {
struct AppState;
struct AiAgent;   // the conversation state (ai_agent.cpp)

// Starts a turn for `prompt`: the user's block, then the streamed answer.
void AiAgentSend(AppState& s, const std::wstring& prompt);
// Cancels the running turn; what arrived stays.
void AiAgentStop(AppState& s);
// Forgets the conversation (新建会话).
void AiAgentReset(AppState& s);
// Drains the stream into blocks; the frame timer calls it while a turn runs.
void AiAgentPump(AppState& s);
bool AiAgentBusy(const AppState& s);
// Repeats the last prompt after a failure.
void AiAgentRetry(AppState& s);
// An error card's button (AiErrAct) for the block at `index`.
void AiAgentErrorAction(AppState& s, int block, int button);
// The confirmation card's answer (organize_files): accept runs the moves.
void AiAgentConfirm(AppState& s, bool accept);
// 撤销 on a result card: puts the moved files back.
void AiAgentUndoResult(AppState& s, int block);
void AiAgentDestroy(AppState& s);

} // namespace luling
