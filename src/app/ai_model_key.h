// ai_model_key.h - The model service's API key (AI 助手, DeepSeek) and the
// voice recognition service's key, kept only on this machine: encrypted with
// DPAPI for the current user in %LOCALAPPDATA%\the app\ai\model_key.bin and
// voice_key.bin. Never written to ai.json, the activity log or the runtime log.
#pragma once

#include <string>
#include <string_view>

namespace luling::app::ai {

// Small file, local disk: fine on the UI thread. One slot per service
// (ai_providers.h id), so switching services in Settings keeps each key:
// model_key_<id>.bin, with the id in the DPAPI entropy. DeepSeek keeps the
// legacy model_key.bin, so a key stored before per-service slots still works.
bool SaveModelKeyFor(std::wstring_view service, std::wstring_view key);
bool LoadModelKeyFor(std::wstring_view service, std::wstring& key);
bool HasModelKeyFor(std::wstring_view service);
bool ClearModelKeyFor(std::wstring_view service);
// The service the assistant is set to: assistant.provider, else the one
// assistant.base_url belongs to, else "deepseek".
std::wstring CurrentService();

// The above for CurrentService().
bool SaveModelKey(std::wstring_view key);
bool LoadModelKey(std::wstring& key);   // false: none stored or unreadable
bool HasModelKey();
bool ClearModelKey();
// 语音识别连接 (third batch, flow V): the same, in voice_key.bin.
bool SaveVoiceKey(std::wstring_view key);
bool LoadVoiceKey(std::wstring& key);
bool HasVoiceKey();
bool ClearVoiceKey();
// "sk-…a1b2" for the settings page; empty for an empty key.
std::wstring MaskModelKey(std::wstring_view key);

} // namespace luling::app::ai
