// ai_settings_edits.h - Settings > AI 助手 (page 12): the API key and address
// EDITs, and the 连接并读取模型 probe. The page's own state lives here so the
// view builder (ai_settings_ui.cpp) and the edit placement share it.
#pragma once

#include "ai_model_stream.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace luling {

struct AppState;

struct AiSettingsEdits {
    HWND key_edit = nullptr;
    HWND base_edit = nullptr;
    HFONT font = nullptr;
    float font_scale = 0.0f;
    int line_px = 0;
    bool key_open = false;
    bool base_open = false;
    bool key_revealed = false;
    bool key_focus_pending = false;
    bool base_focus_pending = false;
    // 换一个服务: back to the grid without giving up the stored service.
    bool grid_open = false;
    // 连接并读取模型.
    struct Job {
        uint64_t gen = 0;
        std::atomic<bool> done{false};
        app::ai::ModelProbe result;
    };
    std::shared_ptr<Job> job;
    uint64_t gen = 0;
    bool probing = false;
    bool probed = false;
    bool probe_ok = false;
    int status_code = 0;
    uint32_t ms = 0;
    size_t model_count = 0;
    std::wstring probe_url;
    uint64_t probed_at = 0;
};

// Places / hides the EDITs after a frame (the renderer keeps the last rects).
void AiSettingsEditsLayout(AppState& s);
void AiSettingsBeginKeyEdit(AppState& s);
void AiSettingsBeginBaseEdit(AppState& s);
void AiSettingsToggleReveal(AppState& s);
void AiSettingsProbeConnect(AppState& s);
void AiSettingsEditsDestroy(AppState& s);

} // namespace luling
