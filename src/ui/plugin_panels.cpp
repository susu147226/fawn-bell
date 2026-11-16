#include "plugin_panels.h"

#include "../app/extension_manager.h"
#include "../plugin/plugin_client.h"

namespace luling::ui {
namespace {

constexpr ULONGLONG kExtensionRefreshMs = 2000;
constexpr ULONGLONG kPanelRefreshMs = 30000;
constexpr ULONGLONG kRequestThrottleMs = 5000;
constexpr size_t kMaxPanels = 2;

} // namespace

PluginPanels& PluginPanels::Instance() {
    static PluginPanels instance;
    return instance;
}

void PluginPanels::Invalidate() {
    std::lock_guard lock(mutex_);
    dirty_ = true;
}

void PluginPanels::EnsureLoaded() const {
    const ULONGLONG now = GetTickCount64();
    if (loaded_ && !dirty_ && now - last_load_ < kExtensionRefreshMs) return;
    dirty_ = false;
    loaded_ = true;
    last_load_ = now;
    std::vector<Entry> next;
    std::vector<plugin::PluginEntry> plugins;
    for (const app::InstalledExtension& ext : app::LoadInstalledExtensions()) {
        if (!ext.is_plugin() || !ext.enabled || !ext.granted || ext.panels.empty()) continue;
        plugins.push_back({ext.id, ext.dir});
        for (const plugin::PluginPanelSpec& spec : ext.panels) {
            if (next.size() >= kMaxPanels) break;
            Entry entry;
            entry.plugin_id = ext.id;
            entry.panel_id = spec.id;
            entry.title = spec.title.empty() ? ext.name : spec.title;
            // Keep rows for a panel that is still declared.
            for (const Entry& old : panels_) {
                if (old.panel_id == entry.panel_id && old.plugin_id == entry.plugin_id) {
                    entry.rows = old.rows;
                    entry.loaded_at = old.loaded_at;
                    entry.requested_at = old.requested_at;
                    break;
                }
            }
            next.push_back(std::move(entry));
        }
        if (next.size() >= kMaxPanels) break;
    }
    panels_ = std::move(next);
    plugins_ = std::move(plugins);
}

std::vector<PluginPanelView> PluginPanels::Panels() {
    std::lock_guard lock(mutex_);
    EnsureLoaded();
    std::vector<PluginPanelView> out;
    out.reserve(panels_.size());
    for (const Entry& entry : panels_) {
        PluginPanelView view;
        view.plugin_id = entry.plugin_id;
        view.panel_id = entry.panel_id;
        view.title = entry.title;
        view.rows = entry.rows;
        out.push_back(std::move(view));
    }
    return out;
}

void PluginPanels::RequestAll() {
    std::lock_guard lock(mutex_);
    EnsureLoaded();
    const ULONGLONG now = GetTickCount64();
    for (Entry& entry : panels_) {
        const bool missing = entry.loaded_at == 0;
        const bool stale = !missing && now - entry.loaded_at > kPanelRefreshMs;
        if (!missing && !stale) continue;
        if (entry.requested_at != 0 && now - entry.requested_at < kRequestThrottleMs) continue;
        if (app::PluginClient::Instance().Panel(entry.panel_id, plugins_))
            entry.requested_at = now;
    }
}

void PluginPanels::Store(const std::wstring& panel_id, std::vector<PluginPanelRow> rows) {
    std::lock_guard lock(mutex_);
    for (Entry& entry : panels_) {
        if (entry.panel_id != panel_id) continue;
        entry.rows = std::move(rows);
        entry.loaded_at = GetTickCount64();
        return;
    }
}

void PluginPanels::RunAction(const std::wstring& panel_id, const std::wstring& action_id) {
    std::lock_guard lock(mutex_);
    EnsureLoaded();
    app::PluginClient::Instance().PanelAction(panel_id, action_id, plugins_);
}

} // namespace luling::ui
