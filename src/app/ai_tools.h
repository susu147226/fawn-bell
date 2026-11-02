// ai_tools.h - The tools 鹿铃助手 may call, and the two read-only ones this
// version ships: get_state (the window's folder and selection) and
// list_folder (one folder's items). Ported in shape from Pulse 1.0.58
// app/ai_tools*; the file-changing tools arrive with the actions layer.
#pragma once

#include "../common/json_value.h"

#include <atomic>
#include <string>
#include <windows.h>

namespace luling {
struct AppState;

namespace app::ai {

struct ToolOutcome {
    bool ok = false;
    json::Value data;            // the result object handed to the model
    std::string code;            // failure: a short machine name
    std::string message;         // failure: one line for the model
};

// The "tools" array sent with every request.
const json::Value& Tools();
// An absolute path, a drive (D:) or a known folder name ("Downloads", "下载").
bool ResolveFolderPath(const std::wstring& raw, std::wstring& out);
// The step card's words for a call ("读取文件夹", "查看当前窗口").
std::wstring ToolTitle(const std::string& name);

// get_state reads AppState, so it runs on the UI thread.
bool ToolNeedsUiThread(const std::string& name);
ToolOutcome RunToolOnUi(AppState& s, const std::string& name, const json::Value& args);
// list_folder touches only its arguments; run it on a worker thread.
ToolOutcome RunToolOnWorker(const std::string& name, const json::Value& args,
                            const std::atomic<bool>& cancelled);

} // namespace app::ai
} // namespace luling
