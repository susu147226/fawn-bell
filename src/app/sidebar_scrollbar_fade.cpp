#include "sidebar_scrollbar_fade.h"
#include <algorithm>

namespace pulse::app {

namespace {

float Approach(float value, float target, uint64_t dt, uint64_t up_ms, uint64_t down_ms) noexcept {
    if (value == target) return value;
    if (value < target) {
        const float step = static_cast<float>(dt) / static_cast<float>(up_ms);
        return std::min(target, value + step);
    }
    const float step = static_cast<float>(dt) / static_cast<float>(down_ms);
    return std::max(target, value - step);
}

} // namespace

void ScrollbarFade::Reveal(uint64_t now) noexcept {
    visible_until_ = std::max(visible_until_, now + kHoldMs);
}

void ScrollbarFade::SetHot(bool hot, uint64_t now) noexcept {
    if (hot_ && !hot) visible_until_ = std::max(visible_until_, now + kHoldMs);
    hot_ = hot;
}

float ScrollbarFade::TargetOpacity(uint64_t now) const noexcept {
    return (hot_ || now < visible_until_) ? 1.0f : 0.0f;
}

bool ScrollbarFade::Tick(uint64_t now) noexcept {
    // The first tick after a long idle steps by at most one fade.
    const uint64_t dt = last_tick_ == 0 || now < last_tick_
        ? 0 : std::min<uint64_t>(now - last_tick_, kFadeOutMs);
    last_tick_ = now;
    const float opacity = opacity_ == 0.0f && TargetOpacity(now) > 0.0f && dt == 0
        ? 1.0f  // no time base yet: show at once rather than stall
        : Approach(opacity_, TargetOpacity(now), dt, kFadeInMs, kFadeOutMs);
    const float expand = Approach(expand_, TargetExpand(), dt, kExpandMs, kExpandMs);
    const bool changed = opacity != opacity_ || expand != expand_;
    opacity_ = opacity;
    expand_ = expand;
    return changed;
}

bool ScrollbarFade::Moving(uint64_t now) const noexcept {
    if (opacity_ != TargetOpacity(now) || expand_ != TargetExpand()) return true;
    // Holding: tick fast just before the fade so it starts smoothly.
    constexpr uint64_t kLead = 150;
    return opacity_ > 0.0f && !hot_ && visible_until_ <= now + kLead;
}

} // namespace pulse::app
