#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include "../ui/window_material.h"

namespace pulse {
// What the quick search window hands to the main window when the user asks to
// continue there (Shift+Enter or the footer link). folder is empty when the
// search covers every indexed location.
struct GlobalSearchHandoff {
    std::wstring query;
    bool content = false;
    std::wstring folder;
};
using GlobalSearchHandoffHandler = std::function<void(const GlobalSearchHandoff&)>;

class GlobalSearchWindow {
public:
    GlobalSearchWindow();
    ~GlobalSearchWindow();
    GlobalSearchWindow(const GlobalSearchWindow&) = delete;
    GlobalSearchWindow& operator=(const GlobalSearchWindow&) = delete;
    bool Show(HWND owner, bool dark, float scale, const std::wstring& current_folder, bool search_pinyin = true);
    void SetAppearance(bool dark, ui::WindowEffect effect, const std::wstring& background_image, D2D1_COLOR_F accent);
    // Called on the UI thread after the popup has hidden itself.
    void SetHandoffHandler(GlobalSearchHandoffHandler handler);
    void Hide();
    void Shutdown();
    bool Visible() const;
private:
    friend struct GlobalSearchWindowTestPeer;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
