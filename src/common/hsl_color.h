#pragma once
// hsl_color.h - small HSL <-> RGB helpers for the custom accent picker.
// Hue is 0..359, saturation and lightness 0..100, RGB is 0xRRGGBB.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace luling::hsl {

inline int Clamp01(double v) {
    return static_cast<int>(std::clamp(v, 0.0, 100.0) + 0.5);
}

inline uint32_t RgbFromHsl(int hue, int sat, int light) {
    const double h = static_cast<double>(((hue % 360) + 360) % 360) / 360.0;
    const double s = std::clamp(sat, 0, 100) / 100.0;
    const double l = std::clamp(light, 0, 100) / 100.0;
    const auto channel = [&](double t) {
        t = t - std::floor(t);
        const double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
        const double p = 2.0 * l - q;
        if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
        if (t < 1.0 / 2.0) return q;
        if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
        return p;
    };
    const auto to_byte = [](double v) {
        return static_cast<uint32_t>(std::clamp(v * 255.0, 0.0, 255.0) + 0.5);
    };
    const uint32_t r = to_byte(channel(h + 1.0 / 3.0));
    const uint32_t g = to_byte(channel(h));
    const uint32_t b = to_byte(channel(h - 1.0 / 3.0));
    return (r << 16) | (g << 8) | b;
}

inline void HslFromRgb(uint32_t rgb, int& hue, int& sat, int& light) {
    const double r = static_cast<double>((rgb >> 16) & 0xFF) / 255.0;
    const double g = static_cast<double>((rgb >> 8) & 0xFF) / 255.0;
    const double b = static_cast<double>(rgb & 0xFF) / 255.0;
    const double max_v = std::max({r, g, b});
    const double min_v = std::min({r, g, b});
    const double l = (max_v + min_v) / 2.0;
    double h = 0.0;
    double s = 0.0;
    const double delta = max_v - min_v;
    if (delta > 1e-6) {
        s = l > 0.5 ? delta / (2.0 - max_v - min_v) : delta / (max_v + min_v);
        if (max_v == r) h = (g - b) / delta + (g < b ? 6.0 : 0.0);
        else if (max_v == g) h = (b - r) / delta + 2.0;
        else h = (r - g) / delta + 4.0;
        h /= 6.0;
    }
    hue = static_cast<int>(std::lround(h * 360.0)) % 360;
    if (hue < 0) hue += 360;
    sat = Clamp01(s * 100.0);
    light = Clamp01(l * 100.0);
}

} // namespace luling::hsl
