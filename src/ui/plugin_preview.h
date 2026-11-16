#pragma once
// plugin_preview.h — details-pane previews drawn by a plugin.
//
// The details panel asks Draw() before the built-in decoder cache: a plugin
// previewer that claims the file's extension either draws its bitmap here or,
// while the host is still rendering, keeps the pane on its loading state. A
// failed or timed-out plugin falls back to the built-in preview.

#include <d2d1.h>
#include <d2d1_1.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../plugin/plugin_protocol.h"

namespace luling::ui {

class PluginPreview {
public:
    static PluginPreview& Instance();

    // Called when the D2D device changes; a lost device drops every bitmap.
    void SetDeviceContext(ID2D1DeviceContext* dc);
    void Reset();

    // True when a plugin previewer claims this path. `pending` tells the
    // caller to show its loading state instead of the built-in preview.
    bool Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest, const std::wstring& path,
              bool* pending, std::wstring* error);
    // From WM_PLUGIN_PREVIEW_DONE. A result for a file that is no longer
    // selected is dropped.
    void Store(std::wstring path, uint32_t status, uint32_t width, uint32_t height,
               std::vector<uint8_t> bgra);

private:
    struct Entry {
        enum class State { Pending, Ready, Failed };
        State state = State::Pending;
        std::wstring error;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        ULONGLONG requested = 0;
    };

    bool SupportsExtension(const std::wstring& extension);
    void RefreshExtensions();
    void EnsureExtensions();
    void RequestPreview(const std::wstring& path);

    ID2D1DeviceContext* dc_ = nullptr;
    // Shared by the details pane and Quick Look; a few files at a time.
    std::unordered_map<std::wstring, Entry> entries_;
    std::unordered_map<std::wstring, bool> supported_;
    std::vector<plugin::PluginEntry> plugins_;
    ULONGLONG last_refresh_ = 0;
    bool loaded_ = false;
};

} // namespace luling::ui
