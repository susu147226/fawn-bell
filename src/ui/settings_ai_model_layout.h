// settings_ai_model_layout.h - Settings > AI 助手 (page 12): texts, geometry
// and hit testing of Pulse 1.0.58's model service page. Included by
// ui_renderer_internal.h after SettingsLayout, so no includes here.
namespace ai_model_text {
inline const wchar_t* Intro() {
    return L"鹿铃内置的对话助手（Ctrl+P）。它用你自己的模型服务和 API Key，Key 只加密保存在这台电脑上。";
}
inline const wchar_t* ModelSection() { return L"模型服务"; }
inline const wchar_t* ChooseTitle() { return L"选择模型服务"; }
inline const wchar_t* ChooseDesc() {
    return L"支持所有兼容 OpenAI 接口的服务。选一个，然后填入它的 API Key。";
}
inline const wchar_t* MoreLabel() { return L"更多"; }
inline const wchar_t* KeyFoot() {
    return L"Key 用 Windows 加密保存在这台电脑上，只发给选中的服务。改动立即生效。";
}
inline const wchar_t* Compatible() { return L" · 兼容 OpenAI 接口"; }
inline const wchar_t* SwitchService() { return L"换一个服务"; }
inline const wchar_t* KeyLabel() { return L"API Key"; }
inline const wchar_t* KeyPlaceholder() { return L"粘贴 API Key"; }
inline std::wstring KeyCreate(const std::wstring& console) {
    return console.empty() ? std::wstring(L"填写服务方提供的密钥")
                           : L"还没有？到 " + console + L" 创建";
}
inline const wchar_t* BaseLabel() { return L"接口地址"; }
inline const wchar_t* BaseHint() {
    return L"一般以 /v1 结尾，例如 http://192.168.1.20:8000/v1";
}
inline const wchar_t* Change() { return L"修改"; }
inline const wchar_t* ConnectLabel() { return L"连接"; }
inline const wchar_t* ConnectDesc() { return L"连接时会读取这个服务提供的模型列表"; }
inline const wchar_t* ConnectButton() { return L"连接并读取模型"; }
inline const wchar_t* Connecting() { return L"正在连接…"; }
inline const wchar_t* ModelLabel() { return L"默认模型"; }
inline const wchar_t* ModelDesc() { return L"新对话用它"; }
inline const wchar_t* ModelNone() { return L"连接并读取模型后可以选择"; }
inline const wchar_t* PackSection() { return L"让其他 AI 应用使用鹿铃"; }
inline const wchar_t* PackTitle() { return L"AI 与 MCP"; }
inline const wchar_t* PackDesc() {
    return L"和上面的助手不是一回事：这是让 Claude 等外部 AI 应用来查找、整理你的文件。需要 AI 增强包。";
}
inline const wchar_t* PackSub() { return L"Luling.AI.exe · MCP · 随鹿铃同步更新"; }
inline const wchar_t* PackManage() { return L"在增强包中管理"; }
inline const wchar_t* PackInstalled() { return L"已安装"; }
inline const wchar_t* PackMissing() { return L"未安装"; }
inline const wchar_t* Broken() { return L"设置文件无法解析，暂时不能写入。"; }
} // namespace ai_model_text

inline float LayoutSettingsAiModel(SettingsLayout& l, const WindowViewModel& vm, float s, float y,
                                   const fluent::Painter* painter) {
    AiModelLayout& o = l.ai_model;
    o = {};
    const ui::AiModelPageView* v = vm.settings_ai_model.get();
    const float left = l.content.left + 20.0f * s, right = l.content.right - 20.0f * s;
    const float inner_left = left + 16.0f * s, inner_right = right - 16.0f * s;
    auto caption_h = [&](std::wstring_view text, float width) {
        if (text.empty()) return 0.0f;
        const float h = painter ? painter->MeasureWrappedCaptionHeight(text, (std::max)(40.0f * s, width)) : 0.0f;
        return h > 0.0f ? h : 20.0f * s;
    };
    auto button_w = [&](std::wstring_view text) { return painter ? painter->MeasureButtonWidth(text) : 96.0f * s; };
    auto badge_w = [&](std::wstring_view text) {
        if (text.empty()) return 0.0f;
        return painter ? painter->MeasureBadgeWidth(text) : 64.0f * s;
    };
    auto section = [&]() {
        y += 24.0f * s;
        const auto r = D2D1::RectF(left, y, right, y + 26.0f * s);
        y = r.bottom;
        return r;
    };

    y += caption_h(ai_model_text::Intro(), right - left) + 16.0f * s;
    if (!v) return y;

    // 模型服务.
    o.model_section = section();
    const float card_top = y;
    if (!v->chosen) {
        const float title_top = y + 16.0f * s;
        const float desc_w = inner_right - inner_left - badge_w(v->badge) - 16.0f * s;
        const float desc_h = caption_h(ai_model_text::ChooseDesc(), desc_w);
        y = title_top + 24.0f * s + desc_h + 14.0f * s;
        const float bw = badge_w(v->badge), bh = 22.0f * s;
        o.badge = D2D1::RectF(inner_right - bw, title_top + (24.0f * s - bh) * 0.5f,
                              inner_right, title_top + (24.0f * s + bh) * 0.5f);

        const float gap = 8.0f * s, tile_h = 72.0f * s;
        const int cols = 5;
        const int rows = v->providers.empty() ? 0
            : (static_cast<int>(v->providers.size()) + cols - 1) / cols;
        const float tile_w = (inner_right - inner_left - gap * (cols - 1)) / cols;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                const size_t i = static_cast<size_t>(r * cols + c);
                if (i >= v->providers.size()) break;
                const float x = inner_left + c * (tile_w + gap);
                o.tiles[i] = D2D1::RectF(x, y, x + tile_w, y + tile_h);
            }
            y += tile_h + gap;
        }
        if (rows > 0) y += 2.0f * s;
        if (!v->more.empty()) {
            const float chip_h = 28.0f * s;
            float x = inner_left + 52.0f * s;
            o.more_label = D2D1::RectF(inner_left, y + (chip_h - 20.0f * s) * 0.5f,
                                       inner_left + 48.0f * s, y + (chip_h + 20.0f * s) * 0.5f);
            for (size_t i = 0; i < v->more.size() && i < o.more.size(); ++i) {
                const float w = (std::max)(96.0f * s, button_w(v->more[i].name) + 34.0f * s);
                o.more[i] = D2D1::RectF(x, y, x + w, y + chip_h);
                x += w + 8.0f * s;
            }
            y += chip_h;
        }
        y += 10.0f * s;
        o.card = D2D1::RectF(left, card_top, right, y);
        y += 8.0f * s;
        const float fh = caption_h(ai_model_text::KeyFoot(), right - left - 4.0f * s);
        o.card_foot = D2D1::RectF(left + 2.0f * s, y, right, y + fh);
        y = o.card_foot.bottom;
    } else {
        // The chosen service: who it is, the key, the address, the probe.
        o.provider_row = D2D1::RectF(left, y, right, y + 70.0f * s);
        const float sw = button_w(ai_model_text::SwitchService()), sh = 32.0f * s;
        const float scy = (o.provider_row.top + o.provider_row.bottom) * 0.5f;
        o.switch_service = D2D1::RectF(inner_right - sw, scy - sh * 0.5f, inner_right, scy + sh * 0.5f);
        y = o.provider_row.bottom;

        const float label_w = 132.0f * s;
        const float field_w = (std::min)(320.0f * s, (inner_right - inner_left - label_w) * 0.55f);
        // API Key.
        const float key_text_w = inner_right - field_w - 20.0f * s - (inner_left + label_w);
        const std::wstring key_create = ai_model_text::KeyCreate(v->console_label);
        const float key_cap_h = caption_h(key_create, key_text_w);
        o.key_row = D2D1::RectF(left, y, right, y + (std::max)(68.0f * s, 30.0f * s + key_cap_h + 16.0f * s));
        const float field_h = 32.0f * s;
        const float kcy = (o.key_row.top + o.key_row.bottom) * 0.5f;
        o.key_field = D2D1::RectF(inner_right - field_w, kcy - field_h * 0.5f,
                                  inner_right - 40.0f * s, kcy + field_h * 0.5f);
        o.key_eye = D2D1::RectF(inner_right - 36.0f * s, kcy - 16.0f * s,
                                inner_right - 4.0f * s, kcy + 16.0f * s);
        y = o.key_row.bottom;

        // 接口地址.
        o.base_row = D2D1::RectF(left, y, right, y + 66.0f * s);
        const float cw = button_w(ai_model_text::Change()), ch = 32.0f * s;
        const float bcy = (o.base_row.top + o.base_row.bottom) * 0.5f;
        o.base_change = D2D1::RectF(inner_right - cw, bcy - ch * 0.5f, inner_right, bcy + ch * 0.5f);
        o.base_field = D2D1::RectF(inner_left + label_w, o.base_row.top + 30.0f * s,
                                   o.base_change.left - 16.0f * s, o.base_row.top + 62.0f * s);
        y = o.base_row.bottom;

        // 连接.
        o.connect_row = D2D1::RectF(left, y, right, y + 66.0f * s);
        const float bw = button_w(v->connecting ? ai_model_text::Connecting()
                                                : ai_model_text::ConnectButton());
        const float bh = 32.0f * s;
        const float ccy = (o.connect_row.top + o.connect_row.bottom) * 0.5f;
        o.connect = D2D1::RectF(inner_right - bw, ccy - bh * 0.5f, inner_right, ccy + bh * 0.5f);
        y = o.connect_row.bottom;

        // 默认模型: the listed ids, wrapping under the label.
        {
            const float row_top = y;
            const float chip_h = 30.0f * s;
            float x = inner_left + label_w;
            float chip_top = y + 16.0f * s;
            size_t shown = 0;
            for (size_t i = 0; i < v->model_choices.size() && i < o.model_chips.size(); ++i) {
                const float w = (std::max)(84.0f * s, button_w(v->model_choices[i]) + 8.0f * s);
                if (x + w > inner_right && x > inner_left + label_w) {
                    x = inner_left + label_w;
                    chip_top += chip_h + 8.0f * s;
                }
                o.model_chips[i] = D2D1::RectF(x, chip_top, x + w, chip_top + chip_h);
                x += w + 8.0f * s;
                ++shown;
            }
            y = shown > 0 ? chip_top + chip_h + 16.0f * s : row_top + 56.0f * s;
            o.model_row = D2D1::RectF(left, row_top, right, y);
        }
        y += 4.0f * s;
        o.card = D2D1::RectF(left, card_top, right, y);
        y += 8.0f * s;
        const float fh = caption_h(ai_model_text::KeyFoot(), right - left - 4.0f * s);
        o.card_foot = D2D1::RectF(left + 2.0f * s, y, right, y + fh);
        y = o.card_foot.bottom;
    }
    if (v->broken) y += 4.0f * s;

    // 让其他 AI 应用使用鹿铃.
    o.pack_section = section();
    const float pack_top = y;
    {
        const float desc_w = inner_right - (inner_left + 38.0f * s) - 12.0f * s;
        const float desc_h = caption_h(ai_model_text::PackDesc(), desc_w);
        const float mw = button_w(ai_model_text::PackManage()), mh = 32.0f * s;
        y += 12.0f * s + 24.0f * s + desc_h + 8.0f * s + mh + 12.0f * s;
        o.pack_manage = D2D1::RectF(inner_right - mw, y - 12.0f * s - mh, inner_right, y - 12.0f * s);
        const float bw = badge_w(v->pack_state), bh = 22.0f * s;
        o.pack_badge = D2D1::RectF(inner_right - bw, pack_top + 14.0f * s,
                                   inner_right, pack_top + 14.0f * s + bh);
    }
    o.pack_card = D2D1::RectF(left, pack_top, right, y);
    return y + 24.0f * s;
}

inline int HitTestSettingsAiModel(const SettingsLayout& l, const WindowViewModel& vm, float x, float y) {
    const AiModelLayout& o = l.ai_model;
    const ui::AiModelPageView* v = vm.settings_ai_model.get();
    if (!v) return -1;
    auto in = [&](const D2D1_RECT_F& r) {
        return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    };
    if (!v->chosen) {
        for (size_t i = 0; i < v->providers.size() && i < o.tiles.size(); ++i)
            if (in(o.tiles[i])) return kAiModelTileBase + static_cast<int>(i);
        for (size_t i = 0; i < v->more.size() && i < o.more.size(); ++i)
            if (in(o.more[i])) return kAiModelMoreBase + static_cast<int>(i);
        return -1;
    }
    if (in(o.switch_service)) return kAiModelSwitchService;
    if (in(o.key_eye)) return kAiModelKeyEye;
    if (in(o.key_field)) return kAiModelKeyField;
    // The address itself is clickable, not only the 更改 button beside it.
    if (in(o.base_field) || in(o.base_change)) return kAiModelBaseField;
    if (in(o.connect)) return kAiModelConnect;
    for (size_t i = 0; i < v->model_choices.size() && i < o.model_chips.size(); ++i)
        if (in(o.model_chips[i])) return kAiModelChoiceBase + static_cast<int>(i);
    if (in(o.pack_manage)) return kAiModelOpenPack;
    return -1;
}
