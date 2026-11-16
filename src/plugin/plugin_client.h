#pragma once
// plugin_client.h — UI-process side of Luling.Plugin.exe.
//
// One background worker owns the host process: it starts the exe on demand,
// keeps the pipe, reloads plugins when the installed list changes, and posts
// each finished command back to the window. The UI thread never blocks on a
// plugin, and a host that stops answering is killed and restarted next time.

#include "plugin_protocol.h"

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace luling::app {

struct PluginCommandResult {
    std::wstring plugin_name;
    std::wstring command_title;
    uint32_t status = 0;  // plugin::PluginInvokeStatus
    std::wstring message;
};

struct PluginPreviewResult {
    std::wstring path;
    uint32_t status = 0;  // plugin::PluginPreviewStatus
    uint32_t width = 0;
    uint32_t height = 0;
    std::wstring message;
    std::vector<uint8_t> bgra;  // premultiplied, width * height * 4
};

struct PluginColumnsResult {
    int index = -1;  // 0 or 1: which plugin column slot
    uint32_t status = 0;
    std::vector<std::wstring> paths;
    std::vector<std::wstring> values;  // parallel to paths
};

struct PluginPanelItem {
    std::wstring glyph;
    std::wstring title;
    std::wstring subtitle;
    std::wstring action;
};

struct PluginPanelResult {
    std::wstring panel_id;
    uint32_t status = 0;
    std::vector<PluginPanelItem> items;
};

struct PluginPanelActionResult {
    std::wstring panel_id;
    std::wstring action_id;
    uint32_t status = 0;
    std::wstring message;
};

class PluginClient {
public:
    static PluginClient& Instance();

    // `notify_window` receives WM_PLUGIN_COMMAND_DONE with an owning
    // PluginCommandResult* in lParam. Call Stop() before the window is gone.
    void Start(HWND notify_window);
    void Stop();

    // Queues one command. `plugins` is the current installed list; the host
    // reloads when it differs from the list it already holds.
    void Invoke(const std::wstring& plugin_id, const std::wstring& command_id,
                const std::wstring& plugin_name, const std::wstring& command_title,
                const std::vector<std::wstring>& paths,
                std::vector<plugin::PluginEntry> plugins);

    // Asks the host for a plugin-rendered preview. The answer arrives as
    // WM_PLUGIN_PREVIEW_DONE with an owning PluginPreviewResult* in lParam.
    void Preview(const std::wstring& path, uint32_t max_px,
                 std::vector<plugin::PluginEntry> plugins);

    // Asks the host for one plugin column's values. The answer arrives as
    // WM_PLUGIN_COLUMNS_DONE with an owning PluginColumnsResult* in lParam.
    void Columns(int index, const std::wstring& plugin_id, const std::wstring& column_id,
                 std::vector<std::wstring> paths,
                 std::vector<plugin::PluginEntry> plugins);

    // Sidebar panel contents; the answer arrives as WM_PLUGIN_PANEL_DONE with
    // an owning PluginPanelResult* in lParam. False when the client is not
    // running, so the caller can retry after Start().
    bool Panel(const std::wstring& panel_id, std::vector<plugin::PluginEntry> plugins);
    // Runs one panel row; the answer arrives as WM_PLUGIN_PANEL_ACTION_DONE.
    bool PanelAction(const std::wstring& panel_id, const std::wstring& action_id,
                     std::vector<plugin::PluginEntry> plugins);

private:
    PluginClient() = default;
    ~PluginClient();
    PluginClient(const PluginClient&) = delete;
    PluginClient& operator=(const PluginClient&) = delete;

    struct Task {
        enum class Kind { Command, Preview, Columns, Panel, PanelAction };
        Kind kind = Kind::Command;
        std::wstring plugin_id;
        std::wstring command_id;
        std::wstring plugin_name;
        std::wstring command_title;
        std::vector<std::wstring> paths;
        std::wstring preview_path;
        std::wstring column_id;
        std::wstring panel_id;
        std::wstring action_id;
        int column_index = -1;
        uint32_t preview_edge = 0;
        std::vector<plugin::PluginEntry> plugins;
        uint32_t request_id = 0;
    };

    void Run();
    void Deliver(PluginCommandResult result);
    void DeliverPreview(PluginPreviewResult result);
    void DeliverColumns(PluginColumnsResult result);
    void DeliverPanel(PluginPanelResult result);
    void DeliverPanelAction(PluginPanelActionResult result);
    bool EnsureHost();
    void StopChild();
    bool HelloIfNeeded(const std::vector<plugin::PluginEntry>& plugins);
    uint32_t Execute(Task& task, std::wstring& message);
    void ExecutePreview(Task& task, PluginPreviewResult& result);
    void ExecuteColumns(Task& task, PluginColumnsResult& result);
    void ExecutePanel(Task& task, PluginPanelResult& result);
    void ExecutePanelAction(Task& task, PluginPanelActionResult& result);

    HWND notify_ = nullptr;
    bool running_ = false;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Task> queue_;
    uint32_t next_request_id_ = 1;

    // Worker-thread only.
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION child_{};
    uint32_t token_ = 0;
    bool hello_done_ = false;
    std::wstring hello_signature_;
};

} // namespace luling::app
