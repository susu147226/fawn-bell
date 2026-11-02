// ai_providers.h - The model services Settings > AI 助手 offers, in the order
// Pulse 1.0.58 lists them. The addresses and key pages are Pulse's own
// (pulse.exe strings). A service picked here sets ai.json assistant.base_url;
// the key stays in ai_model_key.h's DPAPI file.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace luling::app::ai {

struct ProviderInfo {
    std::string_view id;
    std::wstring_view name;
    std::wstring_view vendor;
    std::wstring_view base_url;      // empty: 自定义 (the user types one)
    std::wstring_view console_url;   // where its API key is created; may be empty
    std::wstring_view note;          // 需要代理 / 本机 · 无需 Key ...; may be empty
    uint32_t color = 0;              // the tile's mark colour
    bool in_more = false;            // the 更多 chips under the grid
    bool needs_key = true;
};

const std::vector<ProviderInfo>& Providers();
// The service a stored base_url belongs to; nullptr when it is a custom
// address (or the field is empty, which means DeepSeek's default).
const ProviderInfo* ProviderByBase(const std::wstring& base_url);
const ProviderInfo& DefaultProvider();

// One form for comparisons and for storing: trimmed, no trailing slash, with
// https:// added when a typed address has no scheme.
std::wstring NormalizeBaseUrl(std::wstring url);
bool SameBaseUrl(const std::wstring& a, const std::wstring& b);

} // namespace luling::app::ai
