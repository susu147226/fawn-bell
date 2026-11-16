#pragma once
// mtime_palette.h — the colour sequences behind 修改时间提示's palettes.
//
// Each palette is six steps from "just now" to "thirty days". The single-hue
// palettes step down in strength; the multi-hue ones (冷暖, 极光) carry the age
// in the hue itself. Index 0 follows the theme accent, 6 is the custom colour.

#include <algorithm>
#include <cstdint>

namespace luling::ui {

struct MtimePalette {
    uint32_t light[6];
    uint32_t dark[6];
    bool ramp;
};

inline const MtimePalette& MtimePaletteAt(int index) {
    static const MtimePalette kPalettes[6] = {
        // 主题色: colours come from the theme accent.
        {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, true},
        // 冷暖: hot red through orange and amber to green and calm blue.
        {{0xE5484D, 0xF2711C, 0xE59E0C, 0x6FA82F, 0x2E9E6A, 0x3A7FCC},
         {0xFF6B6F, 0xFF8F4D, 0xFFC24D, 0xA5D46A, 0x4CC38A, 0x62A8F0}, false},
        // 青碧
        {{0x0E9384, 0x0E9384, 0x0E9384, 0x0E9384, 0x0E9384, 0x0E9384},
         {0x2CC9B4, 0x2CC9B4, 0x2CC9B4, 0x2CC9B4, 0x2CC9B4, 0x2CC9B4}, true},
        // 琥珀
        {{0xD97706, 0xD97706, 0xD97706, 0xD97706, 0xD97706, 0xD97706},
         {0xF5A742, 0xF5A742, 0xF5A742, 0xF5A742, 0xF5A742, 0xF5A742}, true},
        // 极光: magenta, violet, indigo, cyan, teal, green.
        {{0xD6409F, 0x8E4EC6, 0x5B5BD6, 0x0088B8, 0x12A594, 0x3E9B4F},
         {0xF07CC2, 0xB78CF0, 0x8D8DF5, 0x4CC2E8, 0x3DD0BC, 0x7BD282}, false},
        // 石墨: no hue at all, for a quiet list.
        {{0x3F4652, 0x3F4652, 0x3F4652, 0x3F4652, 0x3F4652, 0x3F4652},
         {0xD0D4DB, 0xD0D4DB, 0xD0D4DB, 0xD0D4DB, 0xD0D4DB, 0xD0D4DB}, true},
    };
    return kPalettes[std::clamp(index, 0, 5)];
}

inline int MtimePaletteStep(int bucket) { return std::clamp(bucket, 0, 5); }

} // namespace luling::ui
