#pragma once
// plugin_api.h — the C ABI a plugin DLL implements.
//
// The plugin runs inside Luling.Plugin.exe, not in the UI process. The host
// checks `luling_plugin_abi` before calling anything else; a mismatch unloads
// the DLL without running plugin code.
//
//   uint32_t luling_plugin_abi(void);                  required
//   int      luling_plugin_init(const LulingPluginHost* host);   optional
//   void     luling_plugin_shutdown(void);                       optional
//   int      luling_plugin_invoke(const wchar_t* command_id,
//                                 const wchar_t* const* paths, uint32_t count,
//                                 wchar_t* message, uint32_t message_cap);
//   int      luling_plugin_render_preview(const wchar_t* path, uint32_t max_px,
//                                         uint8_t* bgra, uint32_t bgra_cap,
//                                         uint32_t* out_width, uint32_t* out_height,
//                                         wchar_t* message, uint32_t message_cap);
//   int      luling_plugin_column_values(const wchar_t* column_id,
//                                        const wchar_t* const* paths, uint32_t count,
//                                        wchar_t* values, uint32_t values_cap);
//   int      luling_plugin_panel_items(const wchar_t* panel_id, uint32_t max_items,
//                                      wchar_t* items, uint32_t items_cap);
//   int      luling_plugin_panel_action(const wchar_t* panel_id,
//                                       const wchar_t* action_id,
//                                       wchar_t* message, uint32_t message_cap);
//
// Return 0 from init/invoke for success. A non-zero return is reported to the
// user with `message` (UTF-16, NUL-terminated when message_cap > 0).
//
// render_preview returns 0 after writing out_width * out_height * 4 bytes of
// premultiplied BGRA (top-down) into `bgra`, 1 for "this plugin does not
// handle this file", and any other value for a failure. Neither edge may
// exceed max_px.
//
// column_values writes one string per path, joined by U+001F (unit separator)
// and NUL-terminated. Return 0 on success and 1 when the column id is unknown.
//
// panel_items writes at most max_items records separated by U+001E: each record
// is glyph U+001F title U+001F subtitle U+001F action. Return 0 on success and
// 1 when the panel id is unknown.

#include <cstdint>

#define LULING_PLUGIN_ABI_VERSION 1u

#ifdef __cplusplus
#define LULING_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define LULING_PLUGIN_EXPORT __declspec(dllexport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LulingPluginHost {
    uint32_t struct_size;   // sizeof(LulingPluginHost)
    uint32_t abi_version;   // LULING_PLUGIN_ABI_VERSION
    const wchar_t* plugin_dir;
    // level: 0 info, 1 warning, 2 error; goes to the host's runtime log.
    void (__cdecl* log)(int level, const wchar_t* message);
} LulingPluginHost;

#ifdef __cplusplus
}
#endif
