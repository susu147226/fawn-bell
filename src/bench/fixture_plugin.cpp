// A minimal plugin used by luling_plugin_host_test. It implements only what
// the host contract requires and reports what it was given, so the test can
// check the whole pipe round trip.

#include "../plugin/plugin_api.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

extern "C" __declspec(dllexport) uint32_t luling_plugin_abi(void) {
    return LULING_PLUGIN_ABI_VERSION;
}

extern "C" __declspec(dllexport) int luling_plugin_init(const LulingPluginHost* host) {
    if (!host || host->abi_version != LULING_PLUGIN_ABI_VERSION || !host->plugin_dir) return 1;
    return 0;
}

extern "C" __declspec(dllexport) int luling_plugin_invoke(const wchar_t* command_id,
                                                          const wchar_t* const* paths,
                                                          uint32_t count, wchar_t* message,
                                                          uint32_t message_cap) {
    if (!command_id) return 2;
    if (wcscmp(command_id, L"echo") == 0) {
        const wchar_t* first = count != 0 && paths ? paths[0] : L"";
        if (message && message_cap)
            swprintf_s(message, static_cast<size_t>(message_cap), L"count=%u first=%ls", count,
                       first);
        return 0;
    }
    if (wcscmp(command_id, L"fail") == 0) {
        if (message && message_cap)
            _snwprintf_s(message, message_cap, _TRUNCATE, L"failed on purpose");
        return 1;
    }
    return 3;
}

// A recognisable test pattern for .fix files: a checkerboard with an X.
extern "C" __declspec(dllexport) int luling_plugin_render_preview(
    const wchar_t* path, uint32_t max_px, uint8_t* bgra, uint32_t bgra_cap,
    uint32_t* out_width, uint32_t* out_height, wchar_t* message, uint32_t message_cap) {
    if (!path || !bgra || !out_width || !out_height) return 2;
    const wchar_t* dot = wcsrchr(path, L'.');
    if (!dot || _wcsicmp(dot, L".fix") != 0) return 1;
    const uint32_t edge = max_px < 256 ? max_px : 256;
    if (bgra_cap < edge * edge * 4) {
        if (message && message_cap)
            _snwprintf_s(message, message_cap, _TRUNCATE, L"buffer too small");
        return 3;
    }
    for (uint32_t y = 0; y < edge; ++y) {
        for (uint32_t x = 0; x < edge; ++x) {
            const bool check = ((x / 32) + (y / 32)) % 2 == 0;
            const bool cross = x == y || x + y == edge - 1;
            const uint8_t base = check ? 0x40 : 0x20;
            uint8_t* pixel = bgra + (static_cast<size_t>(y) * edge + x) * 4;
            pixel[0] = cross ? 0x40 : base;  // B
            pixel[1] = cross ? 0xC0 : base;  // G
            pixel[2] = cross ? 0x40 : base;  // R
            pixel[3] = 0xFF;                 // A (opaque, so premultiplied == straight)
        }
    }
    *out_width = edge;
    *out_height = edge;
    return 0;
}

// One value per path: the leaf name's length, joined by U+001F.
extern "C" __declspec(dllexport) int luling_plugin_column_values(
    const wchar_t* column_id, const wchar_t* const* paths, uint32_t count, wchar_t* values,
    uint32_t values_cap) {
    if (!column_id || !values) return 2;
    if (wcscmp(column_id, L"name-length") != 0) return 1;
    std::wstring out;
    for (uint32_t i = 0; i < count; ++i) {
        if (i) out.push_back(L'\x1F');
        const wchar_t* path = paths && paths[i] ? paths[i] : L"";
        const wchar_t* leaf = wcsrchr(path, L'\\');
        leaf = leaf ? leaf + 1 : path;
        out += std::to_wstring(wcslen(leaf));
    }
    if (out.size() + 1 > values_cap) return 3;
    std::memcpy(values, out.c_str(), (out.size() + 1) * sizeof(wchar_t));
    return 0;
}

// Three fixed rows, each a record of glyph, title, subtitle, action.
extern "C" __declspec(dllexport) int luling_plugin_panel_items(
    const wchar_t* panel_id, uint32_t max_items, wchar_t* items, uint32_t items_cap) {
    if (!panel_id || !items) return 2;
    if (wcscmp(panel_id, L"fixture-panel") != 0) return 1;
    const wchar_t* rows[] = {
        L"\xE7B8\x1F示例条目一\x1F点击执行动作 one\x1Fone",
        L"\xE73E\x1F示例条目二\x1F点击执行动作 two\x1Ftwo",
        L"\xE783\x1F示例条目三\x1F点击执行动作 three\x1Fthree",
    };
    std::wstring out;
    const uint32_t count = max_items < 3 ? max_items : 3;
    for (uint32_t i = 0; i < count; ++i) {
        if (i) out.push_back(L'\x1E');
        out += rows[i];
    }
    if (out.size() + 1 > items_cap) return 3;
    std::memcpy(items, out.c_str(), (out.size() + 1) * sizeof(wchar_t));
    return 0;
}

extern "C" __declspec(dllexport) int luling_plugin_panel_action(
    const wchar_t* panel_id, const wchar_t* action_id, wchar_t* message, uint32_t message_cap) {
    if (!panel_id || !action_id) return 2;
    if (wcscmp(panel_id, L"fixture-panel") != 0) return 1;
    if (wcscmp(action_id, L"one") == 0 || wcscmp(action_id, L"two") == 0 ||
        wcscmp(action_id, L"three") == 0)
        return 0;
    if (message && message_cap)
        _snwprintf_s(message, message_cap, _TRUNCATE, L"unknown action");
    return 4;
}
