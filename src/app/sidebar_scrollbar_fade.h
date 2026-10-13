#pragma once
#include <cstdint>

namespace pulse::app {

// The sidebar scrollbar stays out of sight until it is useful (#44): a thin
// thumb shows while the sidebar scrolls and fades out a moment later; the
// pointer on the bar (or dragging it) keeps it visible and expands it to the
// full thumb with its track. Time based, so a slow UI tick only coarsens it.
class ScrollbarFade {
public:
    static constexpr uint64_t kHoldMs = 1000;    // visible after the last scroll or hover
    static constexpr uint64_t kFadeInMs = 100;
    static constexpr uint64_t kFadeOutMs = 400;
    static constexpr uint64_t kExpandMs = 120;

    // The sidebar scrolled.
    void Reveal(uint64_t now) noexcept;
    // Pointer on the bar, or the bar being dragged.
    void SetHot(bool hot, uint64_t now) noexcept;
    // Advances to `now`; true when opacity or expansion changed.
    bool Tick(uint64_t now) noexcept;
    // True while a frame-rate tick matters: a fade or expansion is running or
    // about to start.
    bool Moving(uint64_t now) const noexcept;

    float Opacity() const noexcept { return opacity_; }
    float Expand() const noexcept { return expand_; }

private:
    float TargetOpacity(uint64_t now) const noexcept;
    float TargetExpand() const noexcept { return hot_ ? 1.0f : 0.0f; }

    float opacity_ = 0.0f;
    float expand_ = 0.0f;
    bool hot_ = false;
    uint64_t visible_until_ = 0;
    uint64_t last_tick_ = 0;
};

} // namespace pulse::app
