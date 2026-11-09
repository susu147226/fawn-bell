// ui_settings_ai.cpp - Settings > AI 助手 (page 12). Ported from 应用 1.0.58
// ui/ui_settings_ai.cpp: the MCP switch, the action groups, the apps that
// connected, the always-allow rules and this run's calls.
#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace luling::ui {

void MainRenderer::DrawSettingsAi(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using H = HitTestResult;
    const AiPageView* v = vm.settings_ai.get();
    auto* dc = compositor_->Dc();
    if (!dc) return;
    const auto lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const AiPageLayout& o = lay.ai;
    const float s = scale_;
    const bool dark = theme.bg.r + theme.bg.g + theme.bg.b < 1.5f;
    auto hex = [](uint32_t rgb) {
        return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f);
    };
    const D2D1_COLOR_F success = hex(dark ? 0x45D7A4 : 0x107C5A);
    const D2D1_COLOR_F warning = hex(dark ? 0xF7D154 : 0x8A5500);
    const D2D1_COLOR_F failure = hex(dark ? 0xEF8A75 : 0xB3261E);
    auto text = [&](std::wstring_view value, D2D1_RECT_F r, bool is_small = false) {
        painter_.DrawText(value, r, is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),
                          is_small ? theme.text_secondary : theme.text);
    };
    auto card = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, WithAlpha(theme.surface_card, card_alpha_), brFillInput_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8 * s, 8 * s), brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 8 * s, 8 * s), brStrokeCard_.get(), 1);
    };
    auto divider_below = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), r.left + 16 * s, r.bottom, r.right - r.left - 32 * s, 1);
    };
    auto hovered = [&](int target) { return IsHovered(vm, H::SettingsAiAction, target); };
    auto button = [&](D2D1_RECT_F r, std::wstring_view label, int target,
                      fluent::ButtonKind kind = fluent::ButtonKind::Standard, bool enabled = true) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.enabled = enabled;
        state.hovered = enabled && hovered(target);
        painter_.DrawButton({r, label, {}, kind, state});
    };
    auto row_text = [&](D2D1_RECT_F r, const wchar_t* icon, std::wstring_view title, std::wstring_view caption,
                        float controls_left) {
        DrawIconText(r.left + 16 * s, r.top + 20 * s, 24 * s, 24 * s, icon, L"", theme.text_secondary, 0.85f);
        const float right = controls_left - 14 * s;
        text(title, D2D1::RectF(r.left + 54 * s, r.top + 12 * s, right, r.top + 36 * s));
        painter_.DrawWrappedCaption(caption, D2D1::Point2F(r.left + 54 * s, r.top + 36 * s),
                                    right - r.left - 54 * s, theme.text_secondary);
    };
    auto empty_text = [&](D2D1_RECT_F r, std::wstring_view caption) {
        if (r.bottom <= r.top) return;
        painter_.DrawWrappedCaption(caption, D2D1::Point2F(r.left + 16 * s, r.top + 14 * s),
                                    r.right - r.left - 32 * s, theme.text_secondary);
    };

    if (v) {
        // The paragraph above the first card.
        {
            const float pad_x = 20.0f * s;
            const float w = (std::max)(40.0f * s, lay.content.right - lay.content.left - pad_x * 2);
            const float h = painter_.MeasureWrappedCaptionHeight(ai_text::Intro(), w);
            const float top = o.main_card.top > 0.0f ? o.main_card.top - 18.0f * s - h
                                                     : lay.content_origin + 56.0f * s;
            painter_.DrawWrappedCaption(ai_text::Intro(),
                                        D2D1::Point2F(lay.content.left + pad_x, top), w,
                                        theme.text_secondary);
        }
        // 允许 AI 应用连接 / 连接 Claude.
        card(o.main_card);
        if (hovered(kAiToggle)) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            const auto& r = o.enable_row;
            FillRoundedRect(dc, brFillHover_.get(), r.left + 4 * s, r.top + 4 * s, r.right - r.left - 8 * s,
                            r.bottom - r.top - 6 * s, 4 * s);
        }
        row_text(o.enable_row, L"\xE99A", ai_text::EnableTitle(), v->status, o.toggle.left);
        {
            fluent::ControlState state{};
            state.checked = v->enabled;
            state.hovered = hovered(kAiToggle);
            painter_.DrawSwitch(o.toggle, L"", state);
        }
        divider_below(o.enable_row);
        row_text(o.setup_row, L"\xE943", ai_text::SetupTitle(), ai_text::SetupDesc(), o.copy.left);
        button(o.copy, ai_text::CopyConfig(), kAiCopyConfig, fluent::ButtonKind::Standard, v->can_copy);
        if (o.broken.bottom > o.broken.top) {
            const auto& n = o.broken;
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), n.left, n.top, n.right - n.left, n.bottom - n.top, 6 * s);
            MakeBrush(dc, warning, brFillSelected_);
            FillRoundedRect(dc, brFillSelected_.get(), n.left, n.top + 6 * s, 3 * s, n.bottom - n.top - 12 * s, 1.5f * s);
            painter_.DrawWrappedCaption(ai_text::Broken(), D2D1::Point2F(n.left + 12 * s, n.top + 6 * s),
                                        n.right - n.left - 12 * s, theme.text);
        }

        // AI 应用可以做的事.
        if (!o.groups.empty()) {
            text(ai_text::GroupsSection(), o.groups_section);
            card(o.groups_card);
            for (size_t i = 0; i < o.groups.size() && i < v->groups.size(); ++i) {
                const AiGroupRowView& g = v->groups[i];
                const auto& r = o.groups[i];
                const int target = kAiGroupBase + static_cast<int>(i);
                if (!g.locked && hovered(target)) {
                    MakeBrush(dc, theme.fill_hover, brFillHover_);
                    FillRoundedRect(dc, brFillHover_.get(), r.left + 4 * s, r.top + 4 * s, r.right - r.left - 8 * s,
                                    r.bottom - r.top - 6 * s, 4 * s);
                }
                const float controls = g.locked ? r.right - 16 * s : o.group_toggles[i].left;
                const float right = controls - 14 * s;
                DrawIconText(r.left + 16 * s, r.top + 20 * s, 24 * s, 24 * s, g.icon.c_str(), L"",
                             theme.text_secondary, g.locked ? 0.6f : 0.85f);
                float x = r.left + 54 * s;
                painter_.DrawText(g.title, D2D1::RectF(x, r.top + 12 * s, right, r.top + 36 * s),
                                  compositor_->TextFormat(), g.locked ? theme.text_secondary : theme.text);
                x += MeasureTextWidth(compositor_->DwriteFactory(), compositor_->TextFormat(), g.title) + 8 * s;
                if (!g.count.empty() && x < right) {
                    const float w = MeasureTextWidth(compositor_->DwriteFactory(), compositor_->SmallFormat(), g.count);
                    painter_.DrawText(g.count, D2D1::RectF(x, r.top + 13 * s, (std::min)(right, x + w + 2 * s), r.top + 35 * s),
                                      compositor_->SmallFormat(), theme.text_secondary);
                    x += w + 8 * s;
                }
                if (g.tag != AiGroupTag::None) {
                    const wchar_t* label = ai_text::GroupTag(g.tag);
                    const float w = painter_.MeasureBadgeWidth(label);
                    if (x + w <= right) {
                        const D2D1_RECT_F br = D2D1::RectF(x, r.top + 14 * s, x + w, r.top + 32 * s);
                        MakeBrush(dc, WithAlpha(success, 0.14f), brFillSelected_);
                        FillRoundedRect(dc, brFillSelected_.get(), br.left, br.top, br.right - br.left,
                                        br.bottom - br.top, 8 * s);
                        painter_.DrawText(label, br, compositor_->SmallFormat(), success,
                                          fluent::HorizontalAlignment::Center);
                    }
                }
                painter_.DrawWrappedCaption(g.desc, D2D1::Point2F(r.left + 54 * s, r.top + 36 * s),
                                            right - r.left - 54 * s, theme.text_secondary);
                if (!g.locked) {
                    fluent::ControlState state{};
                    state.checked = g.on;
                    state.hovered = hovered(target);
                    painter_.DrawSwitch(o.group_toggles[i], L"", state);
                }
            }
            if (o.groups_foot.bottom > o.groups_foot.top)
                painter_.DrawWrappedCaption(ai_text::GroupsFoot(),
                                            D2D1::Point2F(o.groups_foot.left, o.groups_foot.top),
                                            o.groups_foot.right - o.groups_foot.left, theme.text_secondary);
        }

        // 已连接的 AI 应用.
        text(ai_text::AppsSection(), o.apps_section);
        card(o.apps_card);
        if (v->clients.empty()) empty_text(o.apps_empty, ai_text::AppsEmpty());
        for (size_t i = 0; i < o.clients.size() && i < v->clients.size(); ++i) {
            const AiClientRowView& c = v->clients[i];
            const auto& r = o.clients[i].row;
            DrawIconText(r.left + 16 * s, r.top + 16 * s, 24 * s, 24 * s, L"\xE8D4", L"", theme.text_secondary, 0.85f);
            text(c.name, D2D1::RectF(r.left + 54 * s, r.top + 10 * s, r.right - 16 * s, r.top + 32 * s));
            painter_.DrawWrappedCaption(ai_text::ClientDetail(c), D2D1::Point2F(r.left + 54 * s, r.top + 32 * s),
                                        r.right - r.left - 54 * s - 260 * s, theme.text_secondary);
            if (o.clients[i].allow.right > o.clients[i].allow.left)
                button(o.clients[i].allow, ai_text::Allow(), kAiClientAllowBase + static_cast<int>(i));
            if (o.clients[i].deny.right > o.clients[i].deny.left)
                button(o.clients[i].deny, ai_text::Deny(), kAiClientDenyBase + static_cast<int>(i));
            button(o.clients[i].forget, ai_text::RemoveEntry(), kAiClientForgetBase + static_cast<int>(i));
        }

        // 允许过的操作.
        text(ai_text::RulesSection(), o.rules_section);
        card(o.rules_card);
        if (v->rules.empty()) empty_text(o.rules_empty, ai_text::RulesEmpty());
        for (size_t i = 0; i < o.rules.size() && i < v->rules.size(); ++i) {
            const auto& r = o.rules[i];
            DrawIconText(r.left + 16 * s, r.top + 16 * s, 24 * s, 24 * s, L"\xE8FB", L"", theme.text_secondary, 0.85f);
            text(v->rules[i].tool_label, D2D1::RectF(r.left + 54 * s, r.top + 10 * s, r.right - 16 * s, r.top + 32 * s));
            painter_.DrawWrappedCaption(ai_text::RuleDetail(v->rules[i]),
                                        D2D1::Point2F(r.left + 54 * s, r.top + 32 * s),
                                        r.right - r.left - 54 * s - 120 * s, theme.text_secondary);
            button(o.rule_remove[i], ai_text::RemoveEntry(), kAiRuleRemoveBase + static_cast<int>(i));
        }

        // 最近的活动.
        text(ai_text::RecentSection(), o.recent_section);
        button(o.open_log, ai_text::OpenLog(), kAiOpenLog);
        card(o.recent_card);
        if (v->calls.empty()) empty_text(o.recent_empty, ai_text::RecentEmpty());
        for (size_t i = 0; i < o.calls.size() && i < v->calls.size(); ++i) {
            const auto& r = o.calls[i];
            const AiCallRowView& c = v->calls[i];
            const D2D1_COLOR_F tone = c.tone == AiCallTone::Failed ? failure
                                    : c.tone == AiCallTone::Declined ? theme.text_secondary
                                    : c.tone == AiCallTone::Running ? theme.accent
                                                                    : success;
            painter_.DrawText(c.time, D2D1::RectF(r.left + 16 * s, r.top + 8 * s, r.left + 90 * s, r.top + 28 * s),
                              compositor_->SmallFormat(), theme.text_secondary);
            painter_.DrawText(c.client + L" · " + c.tool_label,
                              D2D1::RectF(r.left + 96 * s, r.top + 8 * s, r.right - 16 * s, r.top + 28 * s),
                              compositor_->SmallFormat(), theme.text);
            painter_.DrawText(c.outcome, D2D1::RectF(r.left + 96 * s, r.top + 8 * s, r.right - 16 * s, r.top + 28 * s),
                              compositor_->SmallFormat(), tone, fluent::HorizontalAlignment::Right);
            if (!c.summary.empty())
                painter_.DrawWrappedCaption(c.summary, D2D1::Point2F(r.left + 96 * s, r.top + 30 * s),
                                            r.right - r.left - 112 * s, theme.text_secondary);
        }
        if (v->more_calls) empty_text(o.more_calls, ai_text::MoreCalls(v->more_calls));

        if (o.uninstall.bottom > o.uninstall.top) {
            text(ai_text::PackSection(), o.pack_section);
            row_text(o.uninstall_row, L"\xE7B8", ai_text::UninstallTitle(), ai_text::UninstallDesc(),
                     o.uninstall.left);
            button(o.uninstall, ai_text::Uninstall(), kAiUninstall);
        }
    }

}

} // namespace luling::ui
