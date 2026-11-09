// ai_providers.cpp - the model service list of Settings > AI 助手.
#include "ai_providers.h"

#include <algorithm>
#include <cwctype>

namespace luling::app::ai {
namespace {

const std::vector<ProviderInfo> kProviders = {
    // The grid, two rows of five (the app's order).
    {"deepseek", L"DeepSeek", L"深度求索", L"https://api.deepseek.com",
     L"https://platform.deepseek.com/api_keys", L"", 0x4D6BFE, false, true},
    {"qwen", L"通义千问", L"阿里云百炼", L"https://dashscope.aliyuncs.com/compatible-mode/v1",
     L"https://bailian.console.aliyun.com/?apiKey=1#/api-key", L"", 0x6B4EFF, false, true},
    {"kimi", L"Kimi", L"月之暗面", L"https://api.moonshot.cn/v1",
     L"https://platform.moonshot.cn/console/api-keys", L"", 0x2F5BEA, false, true},
    {"zhipu", L"智谱 GLM", L"智谱开放平台", L"https://open.bigmodel.cn/api/paas/v4",
     L"https://open.bigmodel.cn/usercenter/apikeys", L"", 0x2E6BFF, false, true},
    {"doubao", L"豆包", L"火山方舟", L"https://ark.cn-beijing.volces.com/api/v3",
     L"", L"", 0x2F7DFF, false, true},
    {"mimo", L"小米 MiMo", L"小米开放平台", L"https://api.xiaomimimo.com/v1",
     L"https://platform.xiaomimimo.com/#/console/api-keys", L"", 0xFF6900, false, true},
    {"siliconflow", L"硅基流动", L"SiliconCloud", L"https://api.siliconflow.cn/v1",
     L"https://cloud.siliconflow.cn/account/ak", L"", 0x7A3CF0, false, true},
    {"openai", L"OpenAI", L"需要代理", L"https://api.openai.com/v1",
     L"https://platform.openai.com/api-keys", L"", 0x10A37F, false, true},
    {"ollama", L"Ollama", L"本机 · 无需 Key", L"http://localhost:11434/v1",
     L"", L"", 0x333333, false, false},
    {"custom", L"自定义", L"任意兼容地址", L"", L"", L"", 0x5F6B7A, false, true},
    // The 更多 chips.
    {"openrouter", L"OpenRouter", L"一个 Key 用多家", L"https://openrouter.ai/api/v1",
     L"https://openrouter.ai/keys", L"", 0x6467F2, true, true},
    {"gemini", L"Gemini", L"Google AI Studio", L"https://generativelanguage.googleapis.com/v1beta/openai",
     L"https://aistudio.google.com/app/apikey", L"", 0x4285F4, true, true},
    {"claude", L"Claude", L"Anthropic", L"https://api.anthropic.com/v1",
     L"https://console.anthropic.com/settings/keys", L"", 0xD97757, true, true},
};

wchar_t Lower(wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); }

bool EqualNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](wchar_t x, wchar_t y) { return Lower(x) == Lower(y); });
}

} // namespace

const std::vector<ProviderInfo>& Providers() { return kProviders; }

const ProviderInfo& DefaultProvider() { return kProviders.front(); }

std::wstring NormalizeBaseUrl(std::wstring url) {
    auto space = [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0x3000;
    };
    size_t a = 0, b = url.size();
    while (a < b && space(url[a])) ++a;
    while (b > a && space(url[b - 1])) --b;
    url = url.substr(a, b - a);
    while (!url.empty() && (url.back() == L'/' || url.back() == L'\\')) url.pop_back();
    if (!url.empty() && url.find(L"://") == std::wstring::npos) url = L"https://" + url;
    return url;
}

bool SameBaseUrl(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = NormalizeBaseUrl(a);
    const std::wstring nb = NormalizeBaseUrl(b);
    return !na.empty() && EqualNoCase(na, nb);
}

const ProviderInfo* ProviderByBase(const std::wstring& base_url) {
    const std::wstring url = NormalizeBaseUrl(base_url);
    if (url.empty()) return &DefaultProvider();
    for (const ProviderInfo& p : kProviders) {
        if (!p.base_url.empty() && EqualNoCase(url, NormalizeBaseUrl(std::wstring(p.base_url))))
            return &p;
    }
    return nullptr;
}

} // namespace luling::app::ai
