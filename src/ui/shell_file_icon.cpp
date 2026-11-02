#include "shell_file_icon.h"

#include <windows.h>
#include <shellapi.h>

#include <cwctype>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace luling::ui {
namespace {

common::MenuIconRef IconToPixels(HICON icon) {
    if (!icon) return {};
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return {};
    auto release = [&] {
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
    };
    BITMAP bitmap{};
    if (!info.hbmColor || !GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) ||
        bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0 ||
        bitmap.bmWidth > static_cast<int>(common::kMaxMenuIconEdge) ||
        bitmap.bmHeight > static_cast<int>(common::kMaxMenuIconEdge)) {
        release();
        return {};
    }
    const int width = bitmap.bmWidth;
    const int height = bitmap.bmHeight;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    HDC dc = GetDC(nullptr);
    const int lines = dc ? GetDIBits(dc, info.hbmColor, 0, height, pixels.data(), &bmi,
                                     DIB_RGB_COLORS) : 0;
    if (dc) ReleaseDC(nullptr, dc);
    release();
    if (lines != height) return {};
    // Old icons carry no alpha channel at all; treat them as opaque.
    bool any_alpha = false;
    for (size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0) { any_alpha = true; break; }
    }
    if (!any_alpha) {
        for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    }
    // Menu bitmaps are premultiplied BGRA.
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const uint32_t alpha = pixels[i + 3];
        if (alpha == 255) continue;
        for (int c = 0; c < 3; ++c)
            pixels[i + c] = static_cast<uint8_t>(pixels[i + c] * alpha / 255);
    }
    auto out = std::make_shared<common::MenuIconPixels>();
    out->width = static_cast<uint32_t>(width);
    out->height = static_cast<uint32_t>(height);
    out->bgra = std::move(pixels);
    return out->Valid() ? common::MenuIconRef(std::move(out)) : nullptr;
}

} // namespace

common::MenuIconRef FileTypeIcon(const std::wstring& extension, bool directory) {
    std::wstring key = directory ? L"<dir>" : extension;
    if (key.empty()) key = L"<file>";
    for (wchar_t& c : key)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    static std::mutex mutex;
    static std::unordered_map<std::wstring, common::MenuIconRef> cache;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = cache.find(key);
        if (found != cache.end()) return found->second;
    }
    const std::wstring query = directory ? L"folder" : (L"file" + extension);
    SHFILEINFOW info{};
    const DWORD attributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    common::MenuIconRef pixels;
    if (SHGetFileInfoW(query.c_str(), attributes, &info, sizeof(info),
                       SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)) {
        pixels = IconToPixels(info.hIcon);
        if (info.hIcon) DestroyIcon(info.hIcon);
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() >= 256) cache.clear();
    cache[key] = pixels;
    return pixels;
}

} // namespace luling::ui
