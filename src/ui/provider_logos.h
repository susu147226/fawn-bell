// provider_logos.h - The model services' marks (Pulse 1.0.58).
#pragma once

#include <string_view>

namespace luling::ui {

// 24x24 SVG text with fill="currentColor"; nullptr for 自定义 (no mark).
const char* ProviderLogoSvg(std::string_view provider_id);

} // namespace luling::ui
