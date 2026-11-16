#include "plugin_preview.h"

#include "../app/extension_manager.h"
#include "../plugin/plugin_client.h"

#include <algorithm>
#include <cwctype>
#include <dxgiformat.h>

namespace luling::ui {
namespace {

constexpr ULONGLONG kExtensionRefreshMs = 2000;
constexpr ULONGLONG kPreviewTimeoutMs = 15000;

std::wstring LowerExtensionOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return {};
    std::wstring extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return extension;
}

} // namespace

PluginPreview& PluginPreview::Instance() {
    static PluginPreview instance;
    return instance;
}

void PluginPreview::SetDeviceContext(ID2D1DeviceContext* dc) {
    if (dc_ == dc) return;
    dc_ = dc;
    // Bitmaps belong to the old device.
    for (auto& [path, entry] : entries_) entry.bitmap.Reset();
}

void PluginPreview::Reset() { entries_.clear(); }

void PluginPreview::RefreshExtensions() {
    last_refresh_ = GetTickCount64();
    loaded_ = true;
    supported_.clear();
    plugins_.clear();
    for (const app::InstalledExtension& ext : app::LoadInstalledExtensions()) {
        if (!ext.is_plugin() || !ext.enabled || !ext.granted || ext.previewers.empty())
            continue;
        plugins_.push_back({ext.id, ext.dir});
        for (const plugin::PluginPreviewerSpec& spec : ext.previewers)
            for (const std::wstring& extension : spec.extensions)
                supported_[extension] = true;
    }
}

void PluginPreview::EnsureExtensions() { RefreshExtensions(); }

bool PluginPreview::SupportsExtension(const std::wstring& extension) {
    if (extension.empty()) return false;
    const auto it = supported_.find(extension);
    if (it != supported_.end()) return it->second;
    const ULONGLONG now = GetTickCount64();
    if (!loaded_ || now - last_refresh_ > kExtensionRefreshMs) EnsureExtensions();
    const auto again = supported_.find(extension);
    return again != supported_.end() && again->second;
}

void PluginPreview::RequestPreview(const std::wstring& path) {
    app::PluginClient::Instance().Preview(path, plugin::kPreviewDefaultEdge, plugins_);
}

bool PluginPreview::Draw(ID2D1DeviceContext* dc, const D2D1_RECT_F& dest,
                         const std::wstring& path, bool* pending, std::wstring* error) {
    if (pending) *pending = false;
    if (!dc || path.empty()) return false;
    if (!SupportsExtension(LowerExtensionOf(path))) return false;
    auto it = entries_.find(path);
    if (it == entries_.end()) {
        // The details pane and Quick Look share this cache; keep it small.
        constexpr size_t kMaxEntries = 8;
        if (entries_.size() >= kMaxEntries) {
            auto oldest = entries_.begin();
            for (auto scan = entries_.begin(); scan != entries_.end(); ++scan)
                if (scan->second.requested < oldest->second.requested) oldest = scan;
            entries_.erase(oldest);
        }
        Entry entry;
        entry.requested = GetTickCount64();
        entries_.emplace(path, std::move(entry));
        RequestPreview(path);
        if (pending) *pending = true;
        return true;
    }
    if (it->second.state == Entry::State::Pending) {
        if (GetTickCount64() - it->second.requested > kPreviewTimeoutMs) {
            it->second.state = Entry::State::Failed;
            if (error) *error = L"plugin-preview-timeout";
            return false;
        }
        if (pending) *pending = true;
        return true;
    }
    if (it->second.state == Entry::State::Failed || !it->second.bitmap.Get()) {
        if (error) *error = it->second.error;
        return false;
    }
    const D2D1_SIZE_F size = it->second.bitmap->GetSize();
    if (size.width <= 0.0f || size.height <= 0.0f) return false;
    const float slot_w = dest.right - dest.left;
    const float slot_h = dest.bottom - dest.top;
    if (slot_w <= 1.0f || slot_h <= 1.0f) return true;
    const float scale = (std::min)(slot_w / size.width, slot_h / size.height);
    const float draw_w = size.width * scale;
    const float draw_h = size.height * scale;
    const D2D1_RECT_F target = D2D1::RectF(
        dest.left + (slot_w - draw_w) * 0.5f, dest.top + (slot_h - draw_h) * 0.5f,
        dest.left + (slot_w + draw_w) * 0.5f, dest.top + (slot_h + draw_h) * 0.5f);
    dc->DrawBitmap(it->second.bitmap.Get(), target, 1.0f,
                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    return true;
}

void PluginPreview::Store(std::wstring path, uint32_t status, uint32_t width, uint32_t height,
                          std::vector<uint8_t> bgra) {
    auto it = entries_.find(path);
    if (it == entries_.end()) return;
    if (status != plugin::PREVIEW_OK || width == 0 || height == 0 || !dc_ ||
        bgra.size() < static_cast<size_t>(width) * height * 4) {
        it->second.state = Entry::State::Failed;
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    const HRESULT hr = dc_->CreateBitmap(
        D2D1::SizeU(width, height), bgra.data(), width * 4,
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                 D2D1_ALPHA_MODE_PREMULTIPLIED)),
        &bitmap);
    if (FAILED(hr) || !bitmap.Get()) {
        it->second.state = Entry::State::Failed;
        return;
    }
    it->second.bitmap = std::move(bitmap);
    it->second.state = Entry::State::Ready;
}

} // namespace luling::ui
