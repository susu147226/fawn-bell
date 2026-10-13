#pragma once
#include <d2d1.h>
#include <algorithm>

namespace pulse::ui {
struct AddressSearchLayout {
    D2D1_RECT_F scope, mode, name, content, input, clear, options, close;
    bool scope_label = false;
    bool mode_label = false;
};
inline AddressSearchLayout LayoutAddressSearch(D2D1_RECT_F field, float scale) {
    AddressSearchLayout out;
    const float width = (field.right - field.left) / scale;
    const float top = field.top + 3 * scale, bottom = field.bottom - 3 * scale;
    const float button = (width < 140 ? 14.0f : 24.0f) * scale;
    // [scope chip] [query ............] [mode chip] [options] [x]
    // The scope label matters more than the mode label: it says where results come from.
    out.scope_label = width >= 280;
    // The mode label needs ~92 DIP; below 400 the icon alone keeps the query readable.
    out.mode_label = width >= 400;
    const float x = field.left + 4 * scale;
    const float scope_width = (out.scope_label ? (width >= 340 ? 104.0f : 88.0f) : width >= 200 ? 28.0f : 0.0f) * scale;
    out.scope = D2D1::RectF(x, top, x + scope_width, bottom);
    const float right = field.right - 4 * scale;
    out.close = D2D1::RectF(right - (width >= 140 ? button : 0), top, right, bottom);
    // The trailing x clears a draft while editing, so there is no separate clear slot.
    out.clear = D2D1::RectF(out.close.left, top, out.close.left, bottom);
    out.options = D2D1::RectF(out.close.left - (width >= 260 ? button : 0), top, out.close.left, bottom);
    // Wide enough for "File name" / "Contents" next to the glyph.
    const float mode_width = (out.mode_label ? 92.0f : width < 140 ? 12.0f : 26.0f) * scale;
    out.mode = D2D1::RectF(out.options.left - 2 * scale - mode_width, top, out.options.left - 2 * scale, bottom);
    // One chip toggles the mode; name keeps the hit target, content is unused.
    out.name = out.mode;
    out.content = D2D1::RectF(out.mode.right, top, out.mode.right, bottom);
    out.input = D2D1::RectF(out.scope.right + (scope_width > 0 ? 6 : 0) * scale, field.top + 2 * scale,
                           out.mode.left - 4 * scale, field.bottom - 2 * scale);
    return out;
}
} // namespace pulse::ui
