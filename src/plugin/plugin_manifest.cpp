#include "plugin_manifest.h"

#include "../common/json_value.h"

#include <algorithm>
#include <cwctype>

namespace luling::plugin {
namespace {

const std::vector<std::wstring>& CapabilityTable() {
    static const std::vector<std::wstring> table = {
        L"ui.command",
        L"ui.preview",
        L"ui.column",
        L"ui.panel",
    };
    return table;
}

std::wstring Trim(const std::wstring& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && iswspace(value[begin])) ++begin;
    while (end > begin && iswspace(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

bool ReadManifestId(const json::Value& root, const std::wstring& folder_name,
                    std::wstring& out) {
    out = Trim(json::Wide(root.get_string("id")));
    if (out.empty()) out = SanitizePluginId(folder_name);
    return !out.empty() && SanitizePluginId(out) == out;
}

bool ReadStringArray(const json::Value& root, const char* key, std::vector<std::wstring>& out) {
    const json::Value* list = root.find(key);
    if (!list) return true;
    if (!list->is_array()) return false;
    for (const auto& item : list->items()) {
        if (!item.is_string()) return false;
        out.push_back(json::Wide(item.as_string()));
    }
    return true;
}

} // namespace

const std::vector<std::wstring>& KnownCapabilities() { return CapabilityTable(); }

bool IsKnownCapability(const std::wstring& capability) {
    const auto& table = CapabilityTable();
    return std::find(table.begin(), table.end(), capability) != table.end();
}

PluginManifest ParsePluginManifest(const std::string& utf8_json, const std::wstring& folder_name) {
    PluginManifest manifest;
    json::Value root;
    if (!json::Parse(utf8_json, root, nullptr) || !root.is_object()) {
        manifest.error = L"invalid-json";
        return manifest;
    }
    int64_t schema = 0;
    const json::Value* schema_value = root.find("schema");
    if (!schema_value || !schema_value->get_int(schema) || schema != 2) {
        manifest.error = L"unsupported-schema";
        return manifest;
    }
    manifest.schema = 2;
    if (!ReadManifestId(root, folder_name, manifest.id)) {
        manifest.error = L"invalid-id";
        return manifest;
    }
    manifest.name = Trim(json::Wide(root.get_string("name")));
    if (manifest.name.empty()) manifest.name = manifest.id;
    manifest.version = Trim(json::Wide(root.get_string("version")));
    if (manifest.version.empty()) {
        manifest.error = L"missing-version";
        return manifest;
    }
    int64_t api_version = 0;
    const json::Value* api_value = root.find("api_version");
    if (!api_value || !api_value->get_int(api_version) || api_version != 1) {
        manifest.error = L"unsupported-api";
        return manifest;
    }
    manifest.api_version = 1;
    const std::wstring main = Trim(json::Wide(root.get_string("main")));
    if (!main.empty()) manifest.main = main;
    if (manifest.main.empty() || manifest.main.find_first_of(L"\\/") != std::wstring::npos ||
        manifest.main == L"." || manifest.main == L"..") {
        manifest.error = L"invalid-main";
        return manifest;
    }
    if (!ReadStringArray(root, "capabilities", manifest.capabilities)) {
        manifest.error = L"invalid-capabilities";
        return manifest;
    }
    std::sort(manifest.capabilities.begin(), manifest.capabilities.end());
    manifest.capabilities.erase(
        std::unique(manifest.capabilities.begin(), manifest.capabilities.end()),
        manifest.capabilities.end());
    for (const std::wstring& capability : manifest.capabilities) {
        if (!IsKnownCapability(capability)) {
            manifest.error = L"unknown-capability";
            return manifest;
        }
    }
    const json::Value* contributes = root.find("contributes");
    if (contributes) {
        if (!contributes->is_object()) {
            manifest.error = L"invalid-contributes";
            return manifest;
        }
        const json::Value* commands = contributes->find("commands");
        if (commands) {
            if (!commands->is_array()) {
                manifest.error = L"invalid-commands";
                return manifest;
            }
            for (const auto& item : commands->items()) {
                if (!item.is_object()) {
                    manifest.error = L"invalid-commands";
                    return manifest;
                }
                PluginCommandSpec spec;
                spec.id = Trim(json::Wide(item.get_string("id")));
                spec.title = Trim(json::Wide(item.get_string("title")));
                spec.glyph = Trim(json::Wide(item.get_string("glyph")));
                if (spec.id.empty() || spec.title.empty()) {
                    manifest.error = L"invalid-commands";
                    return manifest;
                }
                if (std::any_of(manifest.commands.begin(), manifest.commands.end(),
                                [&](const PluginCommandSpec& other) {
                                    return other.id == spec.id;
                                })) {
                    manifest.error = L"duplicate-command";
                    return manifest;
                }
                if (manifest.commands.size() >= static_cast<size_t>(kPluginMaxCommands)) {
                    manifest.error = L"too-many-commands";
                    return manifest;
                }
                manifest.commands.push_back(std::move(spec));
            }
        }
        const json::Value* previewers = contributes->find("previewers");
        if (previewers) {
            if (!previewers->is_array()) {
                manifest.error = L"invalid-previewers";
                return manifest;
            }
            for (const auto& item : previewers->items()) {
                if (!item.is_object()) {
                    manifest.error = L"invalid-previewers";
                    return manifest;
                }
                PluginPreviewerSpec spec;
                const json::Value* extensions = item.find("extensions");
                if (!extensions || !extensions->is_array() || extensions->size() == 0) {
                    manifest.error = L"invalid-previewers";
                    return manifest;
                }
                for (const auto& value : extensions->items()) {
                    if (!value.is_string()) {
                        manifest.error = L"invalid-previewers";
                        return manifest;
                    }
                    std::wstring extension = Trim(json::Wide(value.as_string()));
                    if (!extension.empty() && extension.front() == L'.') extension.erase(0, 1);
                    std::transform(extension.begin(), extension.end(), extension.begin(),
                                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                    if (extension.empty() ||
                        extension.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789_+-") !=
                            std::wstring::npos) {
                        manifest.error = L"invalid-preview-extension";
                        return manifest;
                    }
                    spec.extensions.push_back(std::move(extension));
                }
                int64_t priority = 0;
                if (const json::Value* value = item.find("priority"); value &&
                    value->get_int(priority))
                    spec.priority = static_cast<int>(priority);
                manifest.previewers.push_back(std::move(spec));
            }
            if (!manifest.previewers.empty() &&
                !std::binary_search(manifest.capabilities.begin(), manifest.capabilities.end(),
                                    std::wstring(L"ui.preview"))) {
                manifest.error = L"missing-preview-capability";
                return manifest;
            }
        }
        const json::Value* columns = contributes->find("columns");
        if (columns) {
            if (!columns->is_array() || columns->size() > 4) {
                manifest.error = L"invalid-columns";
                return manifest;
            }
            for (const auto& item : columns->items()) {
                if (!item.is_object()) {
                    manifest.error = L"invalid-columns";
                    return manifest;
                }
                PluginColumnSpec spec;
                spec.id = Trim(json::Wide(item.get_string("id")));
                spec.title = Trim(json::Wide(item.get_string("title")));
                if (spec.id.empty() || spec.title.empty()) {
                    manifest.error = L"invalid-columns";
                    return manifest;
                }
                int64_t width = 0;
                if (const json::Value* value = item.find("width");
                    value && value->get_int(width))
                    spec.width = static_cast<int>(std::clamp<int64_t>(width, 40, 600));
                manifest.columns.push_back(std::move(spec));
            }
            if (!manifest.columns.empty() &&
                !std::binary_search(manifest.capabilities.begin(), manifest.capabilities.end(),
                                    std::wstring(L"ui.column"))) {
                manifest.error = L"missing-column-capability";
                return manifest;
            }
        }
        const json::Value* panels = contributes->find("panels");
        if (panels) {
            if (!panels->is_array() || panels->size() > 4) {
                manifest.error = L"invalid-panels";
                return manifest;
            }
            for (const auto& item : panels->items()) {
                if (!item.is_object()) {
                    manifest.error = L"invalid-panels";
                    return manifest;
                }
                PluginPanelSpec spec;
                spec.id = Trim(json::Wide(item.get_string("id")));
                spec.title = Trim(json::Wide(item.get_string("title")));
                if (spec.id.empty() || spec.title.empty()) {
                    manifest.error = L"invalid-panels";
                    return manifest;
                }
                if (std::any_of(manifest.panels.begin(), manifest.panels.end(),
                                [&](const PluginPanelSpec& other) {
                                    return other.id == spec.id;
                                })) {
                    manifest.error = L"duplicate-panel";
                    return manifest;
                }
                manifest.panels.push_back(std::move(spec));
            }
            if (!manifest.panels.empty() &&
                !std::binary_search(manifest.capabilities.begin(), manifest.capabilities.end(),
                                    std::wstring(L"ui.panel"))) {
                manifest.error = L"missing-panel-capability";
                return manifest;
            }
        }
    }
    if (!manifest.commands.empty() &&
        !std::binary_search(manifest.capabilities.begin(), manifest.capabilities.end(),
                            std::wstring(L"ui.command"))) {
        manifest.error = L"missing-command-capability";
        return manifest;
    }
    return manifest;
}

bool IsLoadablePlugin(const PluginManifest& manifest) {
    return manifest.ok() && manifest.schema == 2 && manifest.api_version == 1;
}

std::wstring SanitizePluginId(const std::wstring& id) {
    std::wstring out;
    out.reserve(id.size());
    for (wchar_t c : id) {
        if (iswalnum(c) || c == L'.' || c == L'_' || c == L'-') out += c;
    }
    if (out == L"." || out == L".." || out.empty()) return {};
    return out;
}

int PluginCommandId(int plugin_index, int command_index) {
    return kPluginCommandBase + plugin_index * kPluginCommandsPerPlugin + command_index;
}

bool ParsePluginCommandId(int command, int& plugin_index, int& command_index) {
    const int maximum = kPluginCommandBase + kPluginMaxCount * kPluginCommandsPerPlugin;
    if (command < kPluginCommandBase || command >= maximum) return false;
    const int offset = command - kPluginCommandBase;
    plugin_index = offset / kPluginCommandsPerPlugin;
    command_index = offset % kPluginCommandsPerPlugin;
    return true;
}

} // namespace luling::plugin
