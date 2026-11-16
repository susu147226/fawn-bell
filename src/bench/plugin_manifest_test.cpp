#include "../plugin/plugin_manifest.h"

#include <cstdio>
#include <string>

using namespace luling::plugin;

namespace {

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

const char* kValidManifest = R"({
  "schema": 2, "id": "hello", "name": "示例插件", "version": "1.0.0",
  "api_version": 1, "main": "plugin.dll",
  "capabilities": ["ui.command"],
  "contributes": {
    "commands": [
      { "id": "hello", "title": "示例：打开所在文件夹" },
      { "id": "copy", "title": "示例：复制路径" }
    ]
  }
})";

} // namespace

int main() {
    const PluginManifest valid = ParsePluginManifest(kValidManifest, L"hello");
    Check(valid.ok(), "valid manifest accepted");
    Check(valid.id == L"hello" && valid.name == L"示例插件", "identity parsed");
    Check(valid.version == L"1.0.0" && valid.api_version == 1, "version and api level parsed");
    Check(valid.main == L"plugin.dll", "module name parsed");
    Check(valid.capabilities.size() == 1 && valid.capabilities[0] == L"ui.command",
          "capabilities parsed");
    Check(valid.commands.size() == 2 && valid.commands[1].id == L"copy", "commands parsed");
    Check(IsLoadablePlugin(valid), "valid manifest is loadable");

    const PluginManifest no_schema =
        ParsePluginManifest(R"({"id":"x","version":"1","api_version":1})", L"x");
    Check(no_schema.error == L"unsupported-schema", "schema is required");

    const PluginManifest wrong_api = ParsePluginManifest(
        R"({"schema":2,"id":"x","version":"1","api_version":9})", L"x");
    Check(wrong_api.error == L"unsupported-api", "api_version must be 1");

    const PluginManifest unknown_cap = ParsePluginManifest(
        R"({"schema":2,"id":"x","version":"1","api_version":1,"capabilities":["fs.write"]})",
        L"x");
    Check(unknown_cap.error == L"unknown-capability", "unknown capability is rejected");

    const PluginManifest missing_cap = ParsePluginManifest(
        R"({"schema":2,"id":"x","version":"1","api_version":1,
            "contributes":{"commands":[{"id":"a","title":"A"}]}})",
        L"x");
    Check(missing_cap.error == L"missing-command-capability",
          "a command needs the ui.command capability");

    const PluginManifest duplicate = ParsePluginManifest(
        R"({"schema":2,"id":"x","version":"1","api_version":1,
            "capabilities":["ui.command"],
            "contributes":{"commands":[{"id":"a","title":"A"},{"id":"a","title":"B"}]}})",
        L"x");
    Check(duplicate.error == L"duplicate-command", "duplicate command ids are rejected");

    const PluginManifest traversal = ParsePluginManifest(
        R"({"schema":2,"id":"x","version":"1","api_version":1,"main":"..\\evil.dll"})", L"x");
    Check(traversal.error == L"invalid-main", "the module must live in the plugin folder");

    const PluginManifest empty_version = ParsePluginManifest(
        R"({"schema":2,"id":"x","api_version":1})", L"x");
    Check(empty_version.error == L"missing-version", "version is required");

    const PluginManifest unnamed = ParsePluginManifest(
        R"({"schema":2,"version":"1","api_version":1})", L"folder-id");
    Check(unnamed.ok() && unnamed.id == L"folder-id" && unnamed.name == L"folder-id",
          "a missing id falls back to the folder name");

    const PluginManifest with_previewer = ParsePluginManifest(
        R"({"schema":2,"id":"x","version":"1","api_version":1,
            "capabilities":["ui.preview"],
            "contributes":{"previewers":[{"extensions":[".FIX","fix"],"priority":7}]}})",
        L"x");
    Check(with_previewer.ok(), "a previewer manifest is accepted");
    Check(with_previewer.previewers.size() == 1 &&
              with_previewer.previewers[0].priority == 7 &&
              with_previewer.previewers[0].extensions.size() == 2 &&
              with_previewer.previewers[0].extensions[0] == L"fix",
          "previewer extensions are normalised");

    const PluginManifest preview_without_capability = ParsePluginManifest(
        R"({"schema":2,"id":"x","version":"1","api_version":1,
            "contributes":{"previewers":[{"extensions":["fix"]}]}})",
        L"x");
    Check(preview_without_capability.error == L"missing-preview-capability",
          "a previewer needs the ui.preview capability");

    const PluginManifest bad_extension = ParsePluginManifest(
        R"({"schema":2,"id":"x","version":"1","api_version":1,
            "capabilities":["ui.preview"],
            "contributes":{"previewers":[{"extensions":["..\\evil"]}]}})",
        L"x");
    Check(bad_extension.error == L"invalid-preview-extension",
          "previewer extensions must be plain suffixes");

    int plugin_index = -1;
    int command_index = -1;
    Check(PluginCommandId(0, 0) == kPluginCommandBase, "first plugin command id");
    Check(ParsePluginCommandId(kPluginCommandBase + 32 + 3, plugin_index, command_index) &&
              plugin_index == 1 && command_index == 3,
          "plugin command ids round-trip");
    Check(!ParsePluginCommandId(kPluginCommandBase - 1, plugin_index, command_index),
          "ids below the plugin range are not plugin commands");
    Check(!ParsePluginCommandId(kPluginCommandBase + kPluginMaxCount * kPluginCommandsPerPlugin,
                                plugin_index, command_index),
          "ids above the plugin range are not plugin commands");

    Check(SanitizePluginId(L"hello") == L"hello", "plain ids survive sanitizing");
    Check(SanitizePluginId(L"../evil") != L"../evil", "separators are stripped");
    const PluginManifest bad_id = ParsePluginManifest(
        R"({"schema":2,"id":"../evil","version":"1","api_version":1})", L"x");
    Check(bad_id.error == L"invalid-id", "ids with separators are rejected");
    Check(SanitizePluginId(L"..").empty(), "dot-dot is not an id");

    if (failures == 0) {
        std::printf("[done] PASS\n");
        return 0;
    }
    std::printf("[done] FAIL (%d)\n", failures);
    return 1;
}
