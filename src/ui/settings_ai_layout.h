// settings_ai_layout.h - Settings > AI 助手: texts, geometry and hit testing.
// Ported from the earlier implementation ui/settings_ai_layout.h (MCP). Included by
// ui_renderer_internal.h after SettingsLayout, so no includes here.
namespace ai_text {
inline const wchar_t* Title() { return L"AI 助手"; }
inline const wchar_t* Intro() {
    return L"让 Claude 等外部 AI 应用通过 MCP 使用鹿铃：查找、整理、重命名你的文件。每一步都由鹿铃向你确认。";
}
inline const wchar_t* EnableTitle() { return L"允许 AI 应用连接"; }
inline const wchar_t* SetupTitle() { return L"连接 Claude"; }
inline const wchar_t* SetupDesc() {
    return L"点「复制配置」，把这段 MCP 配置粘贴到 Claude 等应用的设置里；命令是随鹿铃的 Luling.AI.exe。";
}
inline const wchar_t* CopyConfig() { return L"复制配置"; }
inline const wchar_t* Broken() { return L"设置文件无法解析，暂时不能写入。"; }
inline const wchar_t* AppsSection() { return L"已连接的 AI 应用"; }
inline const wchar_t* AppsEmpty() { return L"还没有 AI 应用连接过。"; }
inline const wchar_t* Allow() { return L"允许"; }
inline const wchar_t* Deny() { return L"拒绝"; }
inline const wchar_t* RemoveEntry() { return L"移除"; }
inline const wchar_t* RulesSection() { return L"允许过的操作"; }
inline const wchar_t* RulesEmpty() { return L"还没有始终允许的操作。"; }
inline const wchar_t* RecentSection() { return L"最近的活动"; }
inline const wchar_t* RecentEmpty() { return L"这次运行还没有 AI 应用调用鹿铃。"; }
inline const wchar_t* OpenLog() { return L"打开活动记录"; }
inline const wchar_t* GroupsSection() { return L"AI 应用可以做的事"; }
inline const wchar_t* GroupsFoot() {
    return L"关闭的分组，AI 应用无法使用；删除与移动始终要在鹿铃里由你确认。";
}
inline const wchar_t* GroupTag(AiGroupTag tag) {
    switch (tag) {
    case AiGroupTag::DefaultOn: return L"默认打开";
    case AiGroupTag::Asks:
    case AiGroupTag::AsksOutside: return L"需要确认";
    default: return L"";
    }
}
inline const wchar_t* PackSection() { return L"AI 增强包"; }
inline const wchar_t* UninstallTitle() { return L"AI 增强包"; }
inline const wchar_t* UninstallDesc() { return L"移除 AI 宿主和它的配置。"; }
inline const wchar_t* Uninstall() { return L"卸载"; }
inline const wchar_t* Grant(AiGrantView grant) {
    switch (grant) {
    case AiGrantView::Allowed: return L"允许";
    case AiGrantView::Session: return L"只允许这次会话";
    case AiGrantView::Denied: return L"拒绝";
    default: return L"还没有选择";
    }
}
inline std::wstring ClientDetail(const AiClientRowView& c) {
    std::wstring text = Grant(c.grant);
    if (c.connected) text += std::wstring(L"  ·  ") + L"在线";
    return text;
}
inline std::wstring RuleDetail(const AiRuleRowView& r) { return r.client + L"  ·  " + r.folder; }
inline std::wstring MoreCalls(size_t n) {
    return L"还有 " + std::to_wstring(n) + L" 次更早的调用";
}
} // namespace ai_text

// Geometry of the AI page: the switch and the client setup, the groups, the
// apps that connected, the always-allow rules, this run's calls, the pack.
inline float LayoutSettingsAi(SettingsLayout& l, const WindowViewModel& vm, float s, float y,
                              const fluent::Painter* painter) {
    AiPageLayout& o = l.ai;
    o = {};
    const AiPageView* v = vm.settings_ai.get();
    const float left = l.content.left + 20.0f * s, right = l.content.right - 20.0f * s;
    const float text_left = left + 54.0f * s, inner_right = right - 16.0f * s;
    auto caption_h = [&](std::wstring_view text, float width) {
        if (text.empty()) return 0.0f;
        const float h = painter ? painter->MeasureWrappedCaptionHeight(text, (std::max)(40.0f * s, width)) : 0.0f;
        return h > 0.0f ? h : 20.0f * s;
    };
    auto button_w = [&](std::wstring_view text) { return painter ? painter->MeasureButtonWidth(text) : 88.0f * s; };
    auto centered = [&](const D2D1_RECT_F& row, float right_edge, float width) {
        const float cy = (row.top + row.bottom) * 0.5f;
        return D2D1::RectF(right_edge - width, cy - 16.0f * s, right_edge, cy + 16.0f * s);
    };
    auto section = [&]() {
        y += 24.0f * s;
        const auto r = D2D1::RectF(left, y, right, y + 28.0f * s);
        y = r.bottom;
        return r;
    };
    auto text_row = [&](std::wstring_view caption, float controls_left) {
        const float h = (std::max)(64.0f * s, 36.0f * s + caption_h(caption, controls_left - 14.0f * s - text_left) + 14.0f * s);
        const auto r = D2D1::RectF(left, y, right, y + h);
        y = r.bottom;
        return r;
    };

    y += caption_h(ai_text::Intro(), right - left) + 18.0f * s;
    if (!v) return y;

    const float card_top = y;
    o.enable_row = text_row(v->status, inner_right - 42.0f * s);
    o.toggle = centered(o.enable_row, inner_right, 42.0f * s);
    const float copy_w = button_w(ai_text::CopyConfig());
    o.setup_row = text_row(ai_text::SetupDesc(), inner_right - copy_w);
    o.copy = centered(o.setup_row, inner_right, copy_w);
    if (v->broken) {
        const float h = caption_h(ai_text::Broken(), inner_right - text_left - 12.0f * s);
        o.broken = D2D1::RectF(text_left - 8.0f * s, y, inner_right, y + h + 12.0f * s);
        y = o.broken.bottom + 14.0f * s;
    }
    o.main_card = D2D1::RectF(left, card_top, right, y);

    if (!v->groups.empty()) {
        o.groups_section = section();
        const float groups_top = y;
        for (const AiGroupRowView& g : v->groups) {
            const auto r = text_row(g.desc, g.locked ? inner_right : inner_right - 42.0f * s);
            o.groups.push_back(r);
            o.group_toggles.push_back(g.locked ? D2D1_RECT_F{} : centered(r, inner_right, 42.0f * s));
        }
        o.groups_card = D2D1::RectF(left, groups_top, right, y);
        const float h = caption_h(ai_text::GroupsFoot(), right - left - 4.0f * s);
        o.groups_foot = D2D1::RectF(left + 2.0f * s, y + 8.0f * s, right, y + 8.0f * s + h);
        y = o.groups_foot.bottom;
    }

    o.apps_section = section();
    const float apps_top = y;
    if (v->clients.empty()) {
        o.apps_empty = D2D1::RectF(left, y, right, y + caption_h(ai_text::AppsEmpty(), right - left - 32.0f * s) + 28.0f * s);
        y = o.apps_empty.bottom;
    }
    const float forget_w = button_w(ai_text::RemoveEntry());
    const float allow_w = button_w(ai_text::Allow()), deny_w = button_w(ai_text::Deny());
    for (size_t i = 0; i < v->clients.size() && i < static_cast<size_t>(kAiRowLimit); ++i) {
        const AiClientRowView& c = v->clients[i];
        AiClientRowLayout row;
        row.row = D2D1::RectF(left, y, right, y + 56.0f * s);
        y = row.row.bottom;
        row.forget = centered(row.row, inner_right, forget_w);
        float x = row.forget.left - 8.0f * s;
        if (c.grant != AiGrantView::Denied) {
            row.deny = centered(row.row, x, deny_w);
            x = row.deny.left - 8.0f * s;
        }
        if (c.grant != AiGrantView::Allowed) row.allow = centered(row.row, x, allow_w);
        o.clients.push_back(row);
    }
    o.apps_card = D2D1::RectF(left, apps_top, right, y);

    o.rules_section = section();
    const float rules_top = y;
    if (v->rules.empty()) {
        o.rules_empty = D2D1::RectF(left, y, right, y + caption_h(ai_text::RulesEmpty(), right - left - 32.0f * s) + 28.0f * s);
        y = o.rules_empty.bottom;
    }
    for (size_t i = 0; i < v->rules.size() && i < static_cast<size_t>(kAiRowLimit); ++i) {
        const auto r = D2D1::RectF(left, y, right, y + 56.0f * s);
        y = r.bottom;
        o.rules.push_back(r);
        o.rule_remove.push_back(centered(r, inner_right, forget_w));
    }
    o.rules_card = D2D1::RectF(left, rules_top, right, y);

    o.recent_section = section();
    {
        const float w = button_w(ai_text::OpenLog());
        o.open_log = D2D1::RectF(right - w, o.recent_section.top - 6.0f * s, right, o.recent_section.top + 26.0f * s);
    }
    y += 8.0f * s;
    const float recent_top = y;
    if (v->calls.empty()) {
        o.recent_empty = D2D1::RectF(left, y, right, y + caption_h(ai_text::RecentEmpty(), right - left - 32.0f * s) + 28.0f * s);
        y = o.recent_empty.bottom;
    } else {
        y += 6.0f * s;
        for (size_t i = 0; i < v->calls.size(); ++i) {
            const auto r = D2D1::RectF(left, y, right, y + (v->calls[i].summary.empty() ? 36.0f : 54.0f) * s);
            y = r.bottom;
            o.calls.push_back(r);
        }
        if (v->more_calls) {
            o.more_calls = D2D1::RectF(left, y, right, y + caption_h(ai_text::MoreCalls(v->more_calls), right - left - 32.0f * s) + 16.0f * s);
            y = o.more_calls.bottom;
        }
        y += 6.0f * s;
    }
    o.recent_card = D2D1::RectF(left, recent_top, right, y);

    if (v->can_uninstall) {
        o.pack_section = section();
        const float w = button_w(ai_text::Uninstall());
        o.uninstall_row = text_row(ai_text::UninstallDesc(), inner_right - w);
        o.uninstall = centered(o.uninstall_row, inner_right, w);
    }
    return y + 24.0f * s;
}

inline int HitTestSettingsAi(const SettingsLayout& l, const WindowViewModel& vm, float x, float y) {
    const AiPageLayout& o = l.ai;
    const AiPageView* v = vm.settings_ai.get();
    if (!v) return -1;
    auto in = [&](const D2D1_RECT_F& r) {
        return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    };
    if (in(o.copy)) return kAiCopyConfig;
    if (in(o.enable_row)) return kAiToggle;
    for (size_t i = 0; i < o.groups.size() && i < v->groups.size(); ++i)
        if (!v->groups[i].locked && in(o.groups[i])) return kAiGroupBase + static_cast<int>(i);
    if (in(o.open_log)) return kAiOpenLog;
    if (in(o.uninstall)) return kAiUninstall;
    for (size_t i = 0; i < o.clients.size(); ++i) {
        const int row = static_cast<int>(i);
        if (in(o.clients[i].allow)) return kAiClientAllowBase + row;
        if (in(o.clients[i].deny)) return kAiClientDenyBase + row;
        if (in(o.clients[i].forget)) return kAiClientForgetBase + row;
    }
    for (size_t i = 0; i < o.rule_remove.size(); ++i)
        if (in(o.rule_remove[i])) return kAiRuleRemoveBase + static_cast<int>(i);
    return -1;
}
