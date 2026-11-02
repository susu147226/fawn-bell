// ai_conversation.cpp - conversation.json (ai_conversation.h).
#include "ai_conversation.h"

#include "ai_assistant.h"
#include "ai_settings.h"
#include "app_internal.h"
#include "../common/json_value.h"
#include "../ui/ai_panel_view.h"

#include <windows.h>

#include <vector>

namespace luling::app::ai {
namespace {

constexpr size_t kMaxTurns = 40;
constexpr size_t kMaxChars = 20000;
constexpr DWORD kMaxFile = 2 * 1024 * 1024;

bool ReadWhole(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= kMaxFile;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) &&
             read == out.size();
    }
    CloseHandle(file);
    return ok;
}

bool WriteWhole(const std::wstring& path, const std::string& data) {
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written,
                              nullptr) && written == data.size();
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

bool Keeps(const ui::AiBlockView& b) {
    return (b.kind == ui::AiBlockKind::User || b.kind == ui::AiBlockKind::Text) && !b.text.empty();
}

} // namespace

std::wstring ConversationPath() {
    const std::wstring dir = AiDirectory();
    return dir.empty() ? std::wstring() : dir + L"\\conversation.json";
}

void LoadConversation(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.blocks.empty()) return;   // a turn already ran: keep it
    std::string text;
    if (!ReadWhole(ConversationPath(), text)) return;
    json::Value root;
    if (!json::Parse(text, root) || !root.is_object()) return;
    if (root.get_string("format") != "luling-ai-chat") return;
    const json::Value* turns = root.find("turns");
    if (!turns || !turns->is_array()) return;
    for (const json::Value& turn : turns->items()) {
        if (!turn.is_object()) continue;
        const std::wstring body = json::Wide(turn.get_string("body"));
        if (body.empty()) continue;
        ui::AiBlockView block;
        block.kind = turn.get_string("role") == "user" ? ui::AiBlockKind::User
                                                       : ui::AiBlockKind::Text;
        block.id = a.next_block_id++;
        block.text = body;
        a.blocks.push_back(std::move(block));
    }
}

void SaveConversation(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.conversation_loaded) return;
    std::vector<const ui::AiBlockView*> kept;
    for (const ui::AiBlockView& b : a.blocks)
        if (Keeps(b)) kept.push_back(&b);
    if (kept.empty()) {
        ClearConversation();
        return;
    }
    if (kept.size() > kMaxTurns)
        kept.erase(kept.begin(), kept.end() - static_cast<ptrdiff_t>(kMaxTurns));
    json::Value root = json::Value::Object();
    root.set("format", "luling-ai-chat");
    json::Value turns = json::Value::Array();
    for (const ui::AiBlockView* b : kept) {
        std::wstring_view body(b->text);
        if (body.size() > kMaxChars) body = body.substr(0, kMaxChars);
        json::Value turn = json::Value::Object();
        turn.set("role", b->kind == ui::AiBlockKind::User ? "user" : "assistant");
        turn.set("body", json::Utf8(body));
        turns.push(std::move(turn));
    }
    root.set("turns", std::move(turns));
    (void)WriteWhole(ConversationPath(), json::Serialize(root));
}

void ClearConversation() {
    const std::wstring path = ConversationPath();
    if (!path.empty()) DeleteFileW(path.c_str());
}

} // namespace app::ai
