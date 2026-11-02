// ai_peek.h - The bubble over the status bar's AI chip (.peek): shown when
// the panel is closed and the assistant finished, or failed. Ported from
// Pulse 1.0.58 (ai_assistant_chat.cpp AiChatPeek / AiChatFillPeek).
#pragma once

#include "../ui/ai_panel_view.h"

#include <cstdint>

namespace luling {
struct AppState;

// Shows the bubble `delay` ms from now, once per turn.
void AiPeekShow(AppState& s, uint32_t delay);
void AiPeekHide(AppState& s);
// FillVm: vm.ai_panel.peek.
void AiPeekFill(AppState& s, ui::AiPeekView& peek);
// A click on the bubble; false when the part is not the bubble's.
bool AiPeekActivate(AppState& s, ui::AiPanelPart part);
void AiPeekDestroy(AppState& s);

} // namespace luling
