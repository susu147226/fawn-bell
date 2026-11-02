// ai_model_view.h - Settings > AI 助手 (page 12), the model service page of
// Pulse 1.0.58: the service grid, the chosen service's key and address, the
// connection probe, 语音输入, and the way into the AI pack's MCP settings.
#pragma once

#include <d2d1.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace luling::ui {

// HitTestResult::SettingsAi indices on page 12.
enum AiModelHit : int {
    kAiModelSwitchService = 10,   // 换一个服务
    kAiModelKeyField = 11,
    kAiModelKeyEye = 12,          // show/hide the key
    kAiModelBaseField = 13,       // 修改 (an address EDIT)
    kAiModelConnect = 14,         // 连接并读取模型
    kAiModelOpenPack = 16,        // 在增强包中管理
    kAiModelTileBase = 100,       // + the grid tile index
    kAiModelMoreBase = 200,       // + the 更多 chip index
    kAiModelChoiceBase = 300,     // + the 默认模型 chip index
};

struct AiProviderTile {
    std::string id;               // ai_providers.h id ("deepseek", ...)
    std::wstring name;
    std::wstring vendor;
    std::wstring mark;            // fallback letter when the logo cannot be drawn
    uint32_t color = 0;
    int index = 0;
    bool selected = false;
};

struct AiModelPageView {
    std::wstring intro;
    bool broken = false;
    // No service chosen yet: the grid.
    bool chosen = false;
    std::wstring badge;           // 未连接 / 缺少 Key / 连接正常
    std::vector<AiProviderTile> providers, more;
    // A chosen service.
    std::string provider_id;
    std::wstring provider_name, provider_vendor;
    std::wstring console_label;   // platform.deepseek.com; empty: no key page
    bool has_console = false;
    bool needs_key = true;
    std::wstring key_mask;        // sk-…9832
    std::wstring key_link;        // 还没有？到 platform.deepseek.com 创建
    bool key_editing = false;     // the EDIT is over the field
    bool key_revealed = false;
    std::wstring base_text;       // https://api.deepseek.com（默认）
    bool base_editing = false;
    std::wstring connect_status;  // 连接正常 · 42 毫秒 · 读到 2 个模型
    std::wstring connect_detail;
    bool connect_ok = false;
    bool connecting = false;
    bool can_connect = false;
    // 默认模型: what the service lists, the one a new chat starts with.
    std::wstring model_label;     // the current id
    std::vector<std::wstring> model_choices;
    std::wstring model_note;      // why there is nothing to pick
    // 让其他 AI 应用使用鹿铃.
    std::wstring pack_state;      // 未安装 / 已安装
    bool pack_installed = false;
};

struct AiModelLayout {
    D2D1_RECT_F model_section{}, card{}, badge{};
    std::array<D2D1_RECT_F, 10> tiles{};
    std::array<D2D1_RECT_F, 3> more{};
    D2D1_RECT_F more_label{}, card_foot{};
    D2D1_RECT_F provider_row{}, switch_service{};
    D2D1_RECT_F key_row{}, key_field{}, key_eye{};
    D2D1_RECT_F base_row{}, base_field{}, base_change{};
    D2D1_RECT_F connect_row{}, connect{};
    D2D1_RECT_F model_row{};
    std::array<D2D1_RECT_F, 10> model_chips{};
    D2D1_RECT_F pack_section{}, pack_card{}, pack_badge{}, pack_manage{};
};

} // namespace luling::ui
