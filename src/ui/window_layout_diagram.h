#pragma once
// Miniature window arrangements, shared by the window-management menu tiles.
#include <algorithm>
#include <d2d1_1.h>

namespace luling::ui {

inline void DrawWindowLayoutDiagram(ID2D1DeviceContext* dc, const D2D1_RECT_F& bounds,
                                    int layout, ID2D1SolidColorBrush* brush, float scale,
                                    D2D1_COLOR_F accent) {
    const float width = bounds.right - bounds.left;
    const float height = bounds.bottom - bounds.top;
    const float gap = 2.0f * scale;
    const float line = std::max(1.0f, scale);
    const auto ink = brush->GetColor();
    int index = 0;
    auto outline = ink;
    outline.a *= 0.15f;
    brush->SetColor(outline);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(bounds, 4 * scale, 4 * scale), brush, line);
    const auto window = [&](float x, float y, float w, float h) {
        const float inset = 3 * scale;
        const float inner_w = width - 2 * inset + gap;
        const float inner_h = height - 2 * inset + gap;
        const auto box = D2D1::RectF(bounds.left + inset + x * inner_w,
                                     bounds.top + inset + y * inner_h,
                                     bounds.left + inset + (x + w) * inner_w - gap,
                                     bounds.top + inset + (y + h) * inner_h - gap);
        const bool current = layout == 5 ? index == 2 : index == 0;
        D2D1_COLOR_F color = current ? accent : ink;
        if (!current) color.a *= 0.22f;
        brush->SetColor(color);
        dc->FillRoundedRectangle(D2D1::RoundedRect(box, 2 * scale, 2 * scale), brush);
        ++index;
    };
    switch (layout) {
    case 0:
        window(0, 0, .5f, 1);
        window(.5f, 0, .5f, 1);
        break;
    case 1:
        window(0, 0, 1, .5f);
        window(0, .5f, 1, .5f);
        break;
    case 2:
        for (int i = 0; i < 3; ++i) window(i / 3.0f, 0, 1 / 3.0f, 1);
        break;
    case 3:
        for (int i = 0; i < 4; ++i) window((i % 2) * .5f, (i / 2) * .5f, .5f, .5f);
        break;
    case 4:
        window(0, 0, .6f, 1);
        window(.6f, 0, .4f, .5f);
        window(.6f, .5f, .4f, .5f);
        break;
    case 5:
        for (int i = 0; i < 3; ++i) window(i * .16f, i * .16f, .68f, .68f);
        break;
    case 6:
        window(.4f, 0, .6f, 1);
        window(0, 0, .4f, .5f);
        window(0, .5f, .4f, .5f);
        break;
    case 7:
        window(0, 0, 1, .6f);
        window(0, .6f, .5f, .4f);
        window(.5f, .6f, .5f, .4f);
        break;
    case 8:
        window(0, 0, 1, 1);
        brush->SetColor(ink);
        dc->DrawLine(D2D1::Point2F(bounds.left + 5 * scale, bounds.top + 13 * scale),
                     D2D1::Point2F(bounds.right - 5 * scale, bounds.top + 13 * scale), brush,
                     line);
        for (int tab = 1; tab < 3; ++tab) {
            const float x = bounds.left + width * tab / 3;
            dc->DrawLine(D2D1::Point2F(x, bounds.top + 5 * scale),
                         D2D1::Point2F(x, bounds.top + 13 * scale), brush, line);
        }
        break;
    default:
        break;
    }
    brush->SetColor(ink);
}

} // namespace luling::ui
