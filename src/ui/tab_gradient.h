#pragma once
// Tab gradient presets. The active tab is filled with a vertical linear
// gradient that fades from the top colour (60% alpha) to the bottom colour
// (0%), so the plate reads as lit from above without a hard edge.

#include <cstdint>

namespace luling::ui {

struct TabGradientStop {
    float pos;
    uint32_t rgb;
};

struct TabGradientPreset {
    const wchar_t* name_zh;
    const wchar_t* name_en;
    TabGradientStop stops[7];
    int stop_count;
};

inline constexpr TabGradientPreset kTabGradients[] = {
    {L"日程橙红", L"Schedule coral", {{0.0f, 0xEE6FA0}, {1.0f, 0xF2544C}}, 2},
    {L"海洋蓝绿", L"Ocean teal", {{0.0f, 0x2F9DE8}, {1.0f, 0x2BD9C8}}, 2},
    {L"极光紫蓝", L"Aurora violet", {{0.0f, 0x7F6BDC}, {1.0f, 0x4E7FE0}}, 2},
    {L"清晨薄雾", L"Morning mist", {{0.0f, 0xDCE6F5}, {1.0f, 0xC9E2F2}}, 2},
    {L"翠绿森林", L"Forest green", {{0.0f, 0x3DBB6E}, {1.0f, 0x2E9C5C}}, 2},
    {L"玫瑰金", L"Rose gold", {{0.0f, 0xF2A03D}, {1.0f, 0xF8C24A}}, 2},
    {L"深夜晚蓝", L"Midnight blue", {{0.0f, 0x0E1B33}, {1.0f, 0x12305C}}, 2},
    {L"糖果粉", L"Candy pink", {{0.0f, 0xE23CA6}, {1.0f, 0xF26AC2}}, 2},
    {L"薄荷清新", L"Mint fresh", {{0.0f, 0x2BD9A8}, {1.0f, 0x3DBB6E}}, 2},
    {L"橙色活力", L"Orange energy", {{0.0f, 0xF5A623}, {1.0f, 0xF7B733}}, 2},
    {L"星空", L"Starry night", {{0.0f, 0x0B1530}, {1.0f, 0x0E2B4E}}, 2},
    {L"彩虹", L"Rainbow",
     {{0.0f, 0xFF3B30}, {0.17f, 0xFFCC00}, {0.33f, 0x34C759}, {0.5f, 0x00C7BE},
      {0.67f, 0x007AFF}, {0.83f, 0xAF52DE}, {1.0f, 0xFF2D55}}, 7},
    {L"径向日出", L"Radial sunrise", {{0.0f, 0xF7B733}, {1.0f, 0xFC4A1A}}, 2},
    {L"径向深海", L"Radial deep sea", {{0.0f, 0x0E2B4E}, {1.0f, 0x06131F}}, 2},
    {L"紫色梦幻", L"Purple dream", {{0.0f, 0xC9B8E8}, {1.0f, 0xE8C9DC}}, 2},
    {L"冰蓝极地", L"Ice polar", {{0.0f, 0xBFE3F0}, {1.0f, 0xCBE8F5}}, 2},
    {L"暖阳橙", L"Warm sun", {{0.0f, 0xF8D6B8}, {1.0f, 0xF5C9A8}}, 2},
    {L"深空黑", L"Deep space", {{0.0f, 0x1A1E24}, {1.0f, 0x0C1014}}, 2},
};

inline constexpr int kTabGradientCount =
    static_cast<int>(sizeof(kTabGradients) / sizeof(kTabGradients[0]));
// 海洋蓝绿 is the default active-tab wash.
inline constexpr int kTabGradientDefault = 1;

} // namespace luling::ui
