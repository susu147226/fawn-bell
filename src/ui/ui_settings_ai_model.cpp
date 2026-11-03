// ui_settings_ai_model.cpp - Settings > AI 助手 (page 12): Pulse 1.0.58's
// model service page - the service grid, the chosen service's key and address,
// the connection probe, 语音输入 and the AI pack's MCP entry.
#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "provider_logos.h"
#include "../common/localization.h"

#include <shlwapi.h>

#include <cstdio>

namespace luling::ui {

void MainRenderer::DrawSettingsAiModel(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                       const Theme& theme) {
    using H = HitTestResult;
    const AiModelPageView* v = vm.settings_ai_model.get();
    auto* dc = compositor_->Dc();
    if (!dc) return;
    const auto lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const AiModelLayout& o = lay.ai_model;
    ai_model_fields_valid_ = false;
    const float s = scale_;
    const bool dark = theme.bg.r + theme.bg.g + theme.bg.b < 1.5f;
    auto hex = [](uint32_t rgb) {
        return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                            (rgb & 0xFF) / 255.0f, 1.0f);
    };
    const D2D1_COLOR_F success = hex(dark ? 0x45D7A4 : 0x107C5A);
    const D2D1_COLOR_F warning = hex(dark ? 0xF7D154 : 0x8A5500);
    const D2D1_COLOR_F failure = hex(dark ? 0xEF8A75 : 0xB3261E);
    auto text = [&](std::wstring_view value, D2D1_RECT_F r, bool is_small = false,
                    D2D1_COLOR_F color = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)) {
        const bool custom = color.a > 0.0f;
        painter_.DrawText(value, r, is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),
                          custom ? color : (is_small ? theme.text_secondary : theme.text));
    };
    auto card = [&](D2D1_RECT_F r, float radius = 10.0f) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, WithAlpha(theme.surface_card, card_alpha_), brFillInput_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius * s, radius * s), brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius * s, radius * s), brStrokeCard_.get(), 1);
    };
    auto divider = [&](float y) {
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), o.card.left + 16 * s, y, o.card.right - o.card.left - 32 * s, 1);
    };
    auto hovered = [&](int target) { return IsHovered(vm, H::SettingsAiAction, target); };
    auto button = [&](D2D1_RECT_F r, std::wstring_view label, int target,
                      fluent::ButtonKind kind = fluent::ButtonKind::Standard, bool enabled = true,
                      std::wstring_view glyph = {}) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.enabled = enabled;
        state.hovered = enabled && hovered(target);
        painter_.DrawButton({r, label, glyph, kind, state});
    };
    auto mark = [&](D2D1_RECT_F r, std::wstring_view letter, uint32_t color, float size) {
        MakeBrush(dc, WithAlpha(hex(color), dark ? 0.22f : 0.14f), brFillSelected_);
        FillRoundedRect(dc, brFillSelected_.get(), r.left, r.top, r.right - r.left, r.bottom - r.top,
                        size * 0.28f);
        painter_.DrawText(letter, r, size >= 28 * s ? compositor_->TextFormat() : compositor_->SmallFormat(),
                          hex(color), fluent::HorizontalAlignment::Center);
    };
    // The service's own mark; the letter only when there is none to draw.
    // Ollama's mark has no brand colour (Pulse draws it in the text colour),
    // and 自定义 shows the link glyph like Pulse does.
    auto logo = [&](std::string_view id, const D2D1_RECT_F& r, uint32_t color,
                    std::wstring_view letter) {
        if (id == "custom") {
            painter_.DrawGlyph(L"\xE71B", r, theme.text_secondary);
            return;
        }
        const D2D1_COLOR_F tint = id == "ollama" ? theme.text : hex(color);
        if (!DrawProviderLogo(id, r, tint)) mark(r, letter, color, r.bottom - r.top);
    };
    auto ctext = [&](std::wstring_view value, D2D1_RECT_F r, bool is_small, D2D1_COLOR_F color) {
        painter_.DrawText(value, r, is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),
                          color, fluent::HorizontalAlignment::Center);
    };
    auto section_title = [&](const D2D1_RECT_F& r, std::wstring_view title) {
        if (r.bottom <= r.top) return;
        text(title, D2D1::RectF(r.left, r.top, r.right, r.bottom));
    };

    if (!v) return;
    const float pad = 20.0f * s;
    // The paragraph above 模型服务.
    {
        const auto& sec = o.model_section;
        const float h = painter_.MeasureWrappedCaptionHeight(
            ai_model_text::Intro(), (std::max)(40.0f * s, lay.content.right - lay.content.left - 2 * pad));
        const float top = sec.top > 0.0f ? sec.top - 24 * s - 16 * s - h : lay.content_origin + 56 * s;
        painter_.DrawWrappedCaption(ai_model_text::Intro(),
                                    D2D1::Point2F(lay.content.left + pad, top),
                                    lay.content.right - lay.content.left - 2 * pad,
                                    theme.text_secondary);
    }

    section_title(o.model_section, ai_model_text::ModelSection());
    card(o.card);

    if (!v->chosen) {
        text(ai_model_text::ChooseTitle(),
             D2D1::RectF(o.card.left + 16 * s, o.card.top + 16 * s, o.card.right - 16 * s,
                         o.card.top + 40 * s));
        const float desc_w = o.card.right - o.card.left - 32 * s;
        painter_.DrawWrappedCaption(ai_model_text::ChooseDesc(),
                                    D2D1::Point2F(o.card.left + 16 * s, o.card.top + 40 * s),
                                    desc_w, theme.text_secondary);
        if (!v->badge.empty())
            painter_.DrawBadge({o.badge, v->badge,
                                v->badge == L"未连接" ? fluent::BadgeKind::Neutral
                                                      : fluent::BadgeKind::Accent});
        for (size_t i = 0; i < v->providers.size() && i < o.tiles.size(); ++i) {
            const AiProviderTile& t = v->providers[i];
            const auto& r = o.tiles[i];
            const int target = kAiModelTileBase + static_cast<int>(i);
            if (hovered(target) && !t.selected) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), r.left, r.top, r.right - r.left,
                                r.bottom - r.top, 8 * s);
            }
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), r.left, r.top, r.right - r.left,
                            r.bottom - r.top, 8 * s);
            MakeBrush(dc, t.selected ? theme.accent : theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 8 * s, 8 * s), brStrokeCard_.get(),
                                     t.selected ? 1.5f : 1.0f);
            const float cx = (r.left + r.right) * 0.5f;
            const float ms = 24 * s;
            logo(t.id, D2D1::RectF(cx - ms * 0.5f, r.top + 9 * s, cx + ms * 0.5f,
                                   r.top + 9 * s + ms),
                 t.color, t.mark);
            ctext(t.name, D2D1::RectF(r.left + 6 * s, r.top + 39 * s, r.right - 6 * s, r.top + 57 * s),
                  false, theme.text);
            if (!t.vendor.empty())
                ctext(t.vendor, D2D1::RectF(r.left + 4 * s, r.top + 57 * s, r.right - 4 * s,
                                            r.top + 71 * s),
                      true, theme.text_secondary);
        }
        if (!v->more.empty()) {
            text(ai_model_text::MoreLabel(), o.more_label, true);
            for (size_t i = 0; i < v->more.size() && i < o.more.size(); ++i) {
                const auto& t = v->more[i];
                const auto& r = o.more[i];
                const int target = kAiModelMoreBase + static_cast<int>(i);
                MakeBrush(dc, hovered(target) ? theme.fill_hover : theme.fill_input, brFillInput_);
                FillRoundedRect(dc, brFillInput_.get(), r.left, r.top, r.right - r.left,
                                r.bottom - r.top, (r.bottom - r.top) * 0.5f);
                const float ms = 16 * s;
                logo(t.id, D2D1::RectF(r.left + 10 * s, (r.top + r.bottom) * 0.5f - ms * 0.5f,
                                       r.left + 10 * s + ms, (r.top + r.bottom) * 0.5f + ms * 0.5f),
                     t.color, t.mark);
                text(t.name, D2D1::RectF(r.left + 32 * s, r.top, r.right - 8 * s, r.bottom), true,
                     theme.text);
            }
        }
        if (o.card_foot.bottom > o.card_foot.top)
            painter_.DrawWrappedCaption(ai_model_text::KeyFoot(),
                                        D2D1::Point2F(o.card_foot.left, o.card_foot.top),
                                        o.card_foot.right - o.card_foot.left, theme.text_secondary);
    } else {
        // Who the service is.
        {
            const auto& r = o.provider_row;
            const float ms = 36 * s;
            const uint32_t color = [&] {
                for (const AiProviderTile& t : v->providers)
                    if (t.id == v->provider_id) return t.color;
                for (const AiProviderTile& t : v->more)
                    if (t.id == v->provider_id) return t.color;
                return uint32_t(0x5F6B7A);
            }();
            logo(v->provider_id,
                 D2D1::RectF(r.left + 16 * s, r.top + (r.bottom - r.top - ms) * 0.5f,
                             r.left + 16 * s + ms, r.top + (r.bottom - r.top + ms) * 0.5f),
                 color, v->provider_name.substr(0, 1));
            text(v->provider_name,
                 D2D1::RectF(r.left + 64 * s, r.top + 12 * s, r.right - 16 * s, r.top + 36 * s));
            text(v->provider_vendor + ai_model_text::Compatible(),
                 D2D1::RectF(r.left + 64 * s, r.top + 38 * s, r.right - 16 * s, r.top + 58 * s), true);
            button(o.switch_service, ai_model_text::SwitchService(), kAiModelSwitchService);
        }
        divider(o.provider_row.bottom);

        // API Key.
        {
            const auto& r = o.key_row;
            const std::wstring create = ai_model_text::KeyCreate(v->console_label);
            text(ai_model_text::KeyLabel(), D2D1::RectF(r.left + 16 * s, r.top + 12 * s,
                                                        r.left + 148 * s, r.top + 36 * s));
            painter_.DrawWrappedCaption(create,
                                        D2D1::Point2F(r.left + 16 * s, r.top + 34 * s),
                                        o.key_field.left - r.left - 32 * s, theme.text_secondary);
            // The field: the EDIT sits over it while the row is being edited.
            MakeBrush(dc, theme.fill_input, brFillInput_);
            FillRoundedRect(dc, brFillInput_.get(), o.key_field.left, o.key_field.top,
                            o.key_field.right - o.key_field.left,
                            o.key_field.bottom - o.key_field.top, 6 * s);
            MakeBrush(dc, v->key_editing ? theme.accent : theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(o.key_field, 6 * s, 6 * s),
                                     brStrokeCard_.get(), 1.0f);
            if (!v->key_editing) {
                const bool has = !v->key_mask.empty();
                text(has ? std::wstring_view(v->key_mask)
                         : std::wstring_view(ai_model_text::KeyPlaceholder()),
                     D2D1::RectF(o.key_field.left + 12 * s, o.key_field.top,
                                 o.key_field.right - 8 * s, o.key_field.bottom),
                     true, has ? theme.text : theme.text_secondary);
            }
            MakeBrush(dc, hovered(kAiModelKeyEye) ? theme.fill_hover : D2D1::ColorF(0, 0, 0, 0),
                      brFillHover_);
            if (hovered(kAiModelKeyEye))
                FillRoundedRect(dc, brFillHover_.get(), o.key_eye.left, o.key_eye.top,
                                o.key_eye.right - o.key_eye.left, o.key_eye.bottom - o.key_eye.top,
                                4 * s);
            DrawIconText(o.key_eye.left + 4 * s, o.key_eye.top + 7 * s, 24 * s, 18 * s,
                         v->key_revealed ? L"\xED1A" : L"\xE890", L"", theme.text_secondary, 0.9f);
        }
        divider(o.key_row.bottom);

        // 接口地址.
        {
            const auto& r = o.base_row;
            text(ai_model_text::BaseLabel(), D2D1::RectF(r.left + 16 * s, r.top + 12 * s,
                                                         r.left + 148 * s, r.top + 36 * s));
            if (!v->base_editing) {
                if (hovered(kAiModelBaseField)) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), o.base_field.left, o.base_field.top,
                                    o.base_field.right - o.base_field.left,
                                    o.base_field.bottom - o.base_field.top, 6 * s);
                }
                text(v->base_text, D2D1::RectF(o.base_field.left, o.base_field.top,
                                               o.base_field.right, o.base_field.bottom), true);
            } else {
                MakeBrush(dc, theme.fill_input, brFillInput_);
                FillRoundedRect(dc, brFillInput_.get(), o.base_field.left, o.base_field.top,
                                o.base_field.right - o.base_field.left,
                                o.base_field.bottom - o.base_field.top, 6 * s);
                MakeBrush(dc, theme.accent, brStrokeCard_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(o.base_field, 6 * s, 6 * s),
                                         brStrokeCard_.get(), 1.0f);
            }
            button(o.base_change, ai_model_text::Change(), kAiModelBaseField);
        }
        divider(o.base_row.bottom);

        // 连接.
        {
            const auto& r = o.connect_row;
            text(ai_model_text::ConnectLabel(), D2D1::RectF(r.left + 16 * s, r.top + 12 * s,
                                                            r.left + 148 * s, r.top + 36 * s));
            text(ai_model_text::ConnectDesc(), D2D1::RectF(r.left + 16 * s, r.top + 34 * s,
                                                           o.connect.left - 20 * s, r.top + 56 * s),
                 true);
            if (!v->connect_status.empty())
                text(v->connect_status, D2D1::RectF(r.left + 16 * s, r.top + 54 * s,
                                                    o.connect.left - 20 * s, r.top + 74 * s),
                     true, v->connect_ok ? success
                                         : (v->connecting ? theme.text_secondary : failure));
            button(o.connect,
                   v->connecting ? ai_model_text::Connecting() : ai_model_text::ConnectButton(),
                   kAiModelConnect, fluent::ButtonKind::Primary, v->can_connect && !v->connecting);
        }
        divider(o.connect_row.bottom);

        // 默认模型.
        {
            const auto& r = o.model_row;
            text(ai_model_text::ModelLabel(), D2D1::RectF(r.left + 16 * s, r.top + 16 * s,
                                                          r.left + 148 * s, r.top + 44 * s));
            if (v->model_choices.empty()) {
                const std::wstring note = v->model_note.empty() ? std::wstring(ai_model_text::ModelNone())
                                                                : v->model_note;
                text(note, D2D1::RectF(r.left + 148 * s, r.top + 16 * s, r.right - 16 * s,
                                       r.top + 44 * s), true);
            }
            for (size_t i = 0; i < v->model_choices.size() && i < o.model_chips.size(); ++i) {
                const auto& c = o.model_chips[i];
                if (c.right <= c.left) continue;
                const bool picked = v->model_choices[i] == v->model_label;
                MakeBrush(dc, picked ? theme.fill_hover : theme.fill_input, brFillInput_);
                FillRoundedRect(dc, brFillInput_.get(), c.left, c.top, c.right - c.left,
                                c.bottom - c.top, (c.bottom - c.top) * 0.5f);
                if (picked || hovered(kAiModelChoiceBase + static_cast<int>(i))) {
                    MakeBrush(dc, picked ? theme.accent : theme.stroke_card, brStrokeCard_);
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(c, (c.bottom - c.top) * 0.5f,
                                                               (c.bottom - c.top) * 0.5f),
                                             brStrokeCard_.get(), 1.0f);
                }
                dc->PushAxisAlignedClip(c, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                text(v->model_choices[i],
                     D2D1::RectF(c.left + 12 * s, c.top, c.right - 12 * s, c.bottom), true,
                     picked ? theme.accent : theme.text);
                dc->PopAxisAlignedClip();
            }
        }
        if (o.card_foot.bottom > o.card_foot.top)
            painter_.DrawWrappedCaption(ai_model_text::KeyFoot(),
                                        D2D1::Point2F(o.card_foot.left, o.card_foot.top),
                                        o.card_foot.right - o.card_foot.left, theme.text_secondary);
        if (v->broken) {
            painter_.DrawWrappedCaption(ai_model_text::Broken(),
                                        D2D1::Point2F(o.card_foot.left, o.card_foot.bottom + 4 * s),
                                        o.card_foot.right - o.card_foot.left, warning);
        }
    }

    // 让其他 AI 应用使用鹿铃.
    section_title(o.pack_section, ai_model_text::PackSection());
    card(o.pack_card);
    {
        const auto& r = o.pack_card;
        DrawIconText(r.left + 16 * s, r.top + 14 * s, 22 * s, 22 * s, L"\xE945", L"",
                     theme.text_secondary, 0.85f);
        const float text_left = r.left + 50 * s;
        const float text_right = o.pack_badge.left - 12 * s;
        text(ai_model_text::PackTitle(), D2D1::RectF(text_left, r.top + 12 * s, text_right,
                                                     r.top + 36 * s));
        painter_.DrawWrappedCaption(ai_model_text::PackDesc(),
                                    D2D1::Point2F(text_left, r.top + 36 * s),
                                    text_right - text_left, theme.text_secondary);
        if (!v->pack_state.empty())
            painter_.DrawBadge({o.pack_badge, v->pack_state,
                                v->pack_installed ? fluent::BadgeKind::Accent
                                                  : fluent::BadgeKind::Neutral});
        painter_.DrawWrappedCaption(ai_model_text::PackSub(),
                                    D2D1::Point2F(text_left, o.pack_manage.top + 8 * s),
                                    o.pack_manage.left - text_left - 12 * s, theme.text_secondary);
        button(o.pack_manage, ai_model_text::PackManage(), kAiModelOpenPack);
    }

    if (v && v->chosen) {
        ai_model_key_rect_ = o.key_field;
        ai_model_base_rect_ = o.base_field;
        ai_model_fields_valid_ = true;
    }
}

bool MainRenderer::DrawProviderLogo(std::string_view provider_id, const D2D1_RECT_F& bounds,
                                    const D2D1_COLOR_F& color) {
    if (provider_id.empty() || !compositor_ || !compositor_->Dc()) return false;
    char hex[8] = {};
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                  static_cast<unsigned>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f + 0.5f),
                  static_cast<unsigned>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                  static_cast<unsigned>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f + 0.5f));
    // The colour is baked into the document, so it is part of the cache key
    // (Ollama's mark follows the theme's text colour).
    const std::string key = std::string(provider_id) + hex;
    if (provider_logo_failed_.count(key)) return false;
    auto it = provider_logos_.find(key);
    if (it == provider_logos_.end()) {
        const char* svg = ProviderLogoSvg(provider_id);
        if (!svg) {
            provider_logo_failed_.insert(key);
            return false;
        }
        if (!empty_state_svg_dc_.get() &&
            FAILED(compositor_->Dc()->QueryInterface(IID_PPV_ARGS(&empty_state_svg_dc_))))
            return false;
        std::string text(svg);
        for (size_t at = text.find("currentColor"); at != std::string::npos;
             at = text.find("currentColor", at + 7))
            text.replace(at, 12, hex);
        ComPtr<IStream> stream;
        stream.p = SHCreateMemStream(reinterpret_cast<const BYTE*>(text.data()),
                                     static_cast<UINT>(text.size()));
        ComPtr<ID2D1SvgDocument> document;
        if (!stream.get() ||
            FAILED(empty_state_svg_dc_->CreateSvgDocument(stream.get(), D2D1::SizeF(24.0f, 24.0f),
                                                          &document)) ||
            !document.get()) {
            provider_logo_failed_.insert(key);
            return false;
        }
        it = provider_logos_.emplace(key, std::move(document)).first;
    }
    ID2D1SvgDocument* svg = it->second.get();
    if (!svg) return false;
    const float w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    const float art = (std::min)(w, h);
    if (art <= 1.0f) return false;
    const float left = (bounds.left + bounds.right - art) * 0.5f;
    const float top = (bounds.top + bounds.bottom - art) * 0.5f;
    svg->SetViewportSize(D2D1::SizeF(24.0f, 24.0f));
    D2D1_MATRIX_3X2_F previous{};
    empty_state_svg_dc_->GetTransform(&previous);
    empty_state_svg_dc_->SetTransform(D2D1::Matrix3x2F::Scale(art / 24.0f, art / 24.0f) *
                                      D2D1::Matrix3x2F::Translation(left, top) * previous);
    empty_state_svg_dc_->DrawSvgDocument(svg);
    empty_state_svg_dc_->SetTransform(previous);
    return true;
}

bool MainRenderer::AiModelFieldRects(D2D1_RECT_F& key, D2D1_RECT_F& base) const {
    if (!ai_model_fields_valid_) return false;
    key = ai_model_key_rect_;
    base = ai_model_base_rect_;
    return true;
}

} // namespace luling::ui
