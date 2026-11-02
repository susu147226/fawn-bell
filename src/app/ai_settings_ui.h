// ai_settings_ui.h - Settings > AI 助手 (page 12): the page's view model and
// its clicks. Ported from Pulse 1.0.58 app/ai_settings_ui.cpp, with Luling's
// own tool groups; the broker itself is still being ported.
#pragma once

#include <memory>

namespace luling {
struct AppState;
namespace ui {
struct AiPageView;
struct AiModelPageView;
}
} // namespace luling

namespace luling {

// 增强包 (page 11): the AI pack's MCP settings.
std::shared_ptr<const ui::AiPageView> BuildAiPage(AppState& s);
// HitTestResult::SettingsAi index (ui::AiHit).
void HandleAiSettings(AppState& s, int hit);

// AI 助手 (page 12): the model service page.
std::shared_ptr<const ui::AiModelPageView> BuildAiModelPage(AppState& s);
// HitTestResult::SettingsAi index (ui::AiModelHit).
void HandleAiModelSettings(AppState& s, int hit);

} // namespace luling
