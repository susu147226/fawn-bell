#pragma once
// plugin_panels.h — sidebar sections contributed by plugins.
//
// A panel is declarative: the plugin returns rows (glyph, title, subtitle,
// action) and Luling draws them with the existing sidebar rows. Clicking a row
// calls back into the plugin. Panels refresh on a slow timer so a plugin can
// keep its rows current without holding the UI.

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "../plugin/plugin_protocol.h"

namespace luling::ui {

struct PluginPanelRow {
    std::wstring glyph;
    std::wstring title;
    std::wstring subtitle;
    std::wstring action;
};

struct PluginPanelView {
    std::wstring plugin_id;
    std::wstring panel_id;
    std::wstring title;
    std::vector<PluginPanelRow> rows;
};

class PluginPanels {
public:
    static PluginPanels& Instance();

    void Invalidate();

    // Up to two panels declared by approved plugins, in declaration order.
    std::vector<PluginPanelView> Panels();

    // Refreshes panels whose rows are missing or stale. Safe to call often.
    void RequestAll();

    // From WM_PLUGIN_PANEL_DONE.
    void Store(const std::wstring& panel_id, std::vector<PluginPanelRow> rows);

    // Runs a row's action in the host.
    void RunAction(const std::wstring& panel_id, const std::wstring& action_id);

private:
    struct Entry {
        std::wstring plugin_id;
        std::wstring panel_id;
        std::wstring title;
        std::vector<PluginPanelRow> rows;
        ULONGLONG loaded_at = 0;
        ULONGLONG requested_at = 0;
    };

    void EnsureLoaded() const;

    mutable std::mutex mutex_;
    mutable std::vector<Entry> panels_;
    mutable std::vector<plugin::PluginEntry> plugins_;
    mutable ULONGLONG last_load_ = 0;
    mutable bool loaded_ = false;
    mutable bool dirty_ = true;
};

} // namespace luling::ui
