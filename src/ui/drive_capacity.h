#pragma once
// How full a drive is, said the same way everywhere it shows: one level for
// the colour of the bar and its numbers, one whole percent, one wording.

#include "../common/localization.h"
#include "../common/text_format.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace luling::ui {

enum class CapacityLevel : std::uint8_t { Low, Normal, Warning, Critical };

constexpr std::uint64_t kCapacityUnknownFree = ~std::uint64_t{0};

// Whole percent for the label: never 0% while something is used, never 100%
// while something is left.
constexpr int CapacityPercent(float used) noexcept {
    if (!(used > 0.0f)) return 0;
    if (used >= 1.0f) return 100;
    const int rounded = static_cast<int>(used * 100.0f + 0.5f);
    return rounded < 1 ? 1 : rounded > 99 ? 99 : rounded;
}

// By the whole percent shown beside the bar, so "80%" is never still blue.
// Under 50% used: green. 50-79%: the accent. 80-94%: amber. 95% and up, or a
// drive that is at least 80% used with under 1 GB left: red.
constexpr CapacityLevel CapacityLevelFor(float used,
                                         std::uint64_t free_bytes = kCapacityUnknownFree) noexcept {
    constexpr std::uint64_t kCriticalFree = std::uint64_t{1} << 30;
    const int percent = CapacityPercent(used);
    if (percent >= 95 || (percent >= 80 && free_bytes < kCriticalFree)) return CapacityLevel::Critical;
    if (percent >= 80) return CapacityLevel::Warning;
    if (percent < 50) return CapacityLevel::Low;
    return CapacityLevel::Normal;
}

inline std::wstring CapacityPercentText(float used) {
    return std::to_wstring(CapacityPercent(used)) + L"%";
}

// One colour per drive for the sidebar's disk rows, so C: and D: stay apart at
// a glance. Keyed by letter, so a colour never moves when rows reorder; C
// starts the palette and wraps after the last slot.
constexpr size_t kDriveAccentCount = 12;
inline std::uint32_t DriveAccentRgb(wchar_t letter, bool dark) noexcept {
    static constexpr std::uint32_t kLight[kDriveAccentCount] = {
        0x2F6FDF, 0xE07B39, 0x2E9E6B, 0xB44E9E, 0x1597A8, 0xC9A227,
        0x6C5CE7, 0xC0392B, 0x3D8C7A, 0x9A6B4F, 0x5566A8, 0xA34B6B,
    };
    static constexpr std::uint32_t kDark[kDriveAccentCount] = {
        0x6EA8FF, 0xF0A05A, 0x54C992, 0xE07BBB, 0x45C2D0, 0xE5C04A,
        0x9C8CFF, 0xE06A5A, 0x63BBA6, 0xC79A7C, 0x8A9BD8, 0xD4799B,
    };
    int slot = 0;
    if (letter >= L'A' && letter <= L'Z') {
        slot = (static_cast<int>(letter) - 'C' + 26) % static_cast<int>(kDriveAccentCount);
    } else if (letter >= L'a' && letter <= L'z') {
        slot = (static_cast<int>(letter) - 'c' + 26) % static_cast<int>(kDriveAccentCount);
    }
    return dark ? kDark[slot] : kLight[slot];
}

inline void ReplaceCapacityToken(std::wstring& text, std::wstring_view token,
                                 const std::wstring& value, size_t* at = nullptr) {
    const size_t pos = text.find(token);
    if (pos == std::wstring::npos) return;
    text.replace(pos, token.size(), value);
    if (at) *at = pos;
}

// Sidebar drive rows keep a compact suffix: "76.3G 可用", or "仅剩 14.7G"
// once the drive is red.
inline std::wstring DriveSidebarDetail(std::uint64_t free_bytes, std::uint64_t total_bytes) {
    if (!total_bytes) return {};
    if (free_bytes > total_bytes) free_bytes = total_bytes;
    const float used = static_cast<float>(static_cast<double>(total_bytes - free_bytes) /
                                          static_cast<double>(total_bytes));
    std::wstring text = l10n::Get(CapacityLevelFor(used, free_bytes) == CapacityLevel::Critical
                                      ? l10n::StringId::DriveSideOnlyLeft
                                      : l10n::StringId::DriveSideFree);
    ReplaceCapacityToken(text, L"{free}",
                         format::ByteSize(free_bytes, false, format::ByteSizeStyle::Compact));
    return text;
}

// This PC style line: "75.2 GB 可用 · 共 476.6 GB" ("仅剩 14.7 GB · 共 325.6 GB"
// once red). The free size is drawn semibold: [bold_start, bold_start + bold_length).
struct DriveSpaceText {
    std::wstring text;
    size_t bold_start = 0;
    size_t bold_length = 0;
};

inline DriveSpaceText DriveTileSpaceText(std::uint64_t free_bytes, std::uint64_t total_bytes) {
    DriveSpaceText out;
    if (!total_bytes) return out;
    if (free_bytes > total_bytes) free_bytes = total_bytes;
    const float used = static_cast<float>(static_cast<double>(total_bytes - free_bytes) /
                                          static_cast<double>(total_bytes));
    out.text = l10n::Get(CapacityLevelFor(used, free_bytes) == CapacityLevel::Critical
                             ? l10n::StringId::DriveOnlyLeft
                             : l10n::StringId::DriveFreeOfTotal);
    // {total} first: replacing {free} first would shift where {total} is.
    const size_t free_at = out.text.find(L"{free}");
    const size_t total_at = out.text.find(L"{total}");
    const std::wstring free_text = format::ByteSize(free_bytes);
    const std::wstring total_text = format::ByteSize(total_bytes);
    if (total_at != std::wstring::npos && free_at != std::wstring::npos && total_at < free_at) {
        ReplaceCapacityToken(out.text, L"{total}", total_text);
        ReplaceCapacityToken(out.text, L"{free}", free_text, &out.bold_start);
    } else {
        ReplaceCapacityToken(out.text, L"{free}", free_text, &out.bold_start);
        ReplaceCapacityToken(out.text, L"{total}", total_text);
    }
    out.bold_length = free_at == std::wstring::npos ? 0 : free_text.size();
    return out;
}

} // namespace luling::ui
