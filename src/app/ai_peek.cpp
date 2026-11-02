// ai_peek.cpp - The bubble over the AI chip (ai_peek.h).
#include "ai_peek.h"

#include "ai_agent.h"
#include "ai_assistant.h"
#include "ai_assistant_conn.h"
#include "app_internal.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <windows.h>

#include <string>

namespace luling {
namespace {

constexpr UINT_PTR kPeekTimer = 0x4156;   // the bubble's in and out
constexpr uint64_t kPeekMs = 4200;

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

uint64_t Now() { return ui::motion::NowMs(); }

// Display text back to plain: the bubble has no bold.
std::wstring Plain(const std::wstring& text) {
    std::wstring out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'*' && i + 1 < text.size() && text[i + 1] == L'*') {
            ++i;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

void CALLBACK PeekTimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s) return;
    Redraw(*s);
    const uint64_t now = Now();
    const AiAssistantState& a = s->aiAssistant;
    const uint64_t next = a.peek_at > now ? a.peek_at : a.peek_until > now ? a.peek_until : 0;
    if (next) SetTimer(hwnd, kPeekTimer, static_cast<UINT>(next - now + 1), PeekTimerProc);
}

} // namespace

void AiPeekShow(AppState& s, uint32_t delay) {
    AiAssistantState& a = s.aiAssistant;
    if (a.open || a.busy || !a.turn_start) return;
    if (!a.peek_failed && a.chip_text != L"完成") return;
    if (a.peek_turn == a.turn_start) return;   // once per turn
    if (a.peek_text.empty()) {
        // No summary of its own: the start of the answer.
        for (auto it = a.blocks.rbegin(); it != a.blocks.rend(); ++it) {
            if (it->kind == ui::AiBlockKind::User) break;
            if (it->kind != ui::AiBlockKind::Text) continue;
            std::wstring t = Plain(it->text);
            for (wchar_t& ch : t)
                if (ch == L'\n' || ch == L'\r') ch = L' ';
            if (t.size() > 80) t = t.substr(0, 78) + L"…";
            a.peek_text = t;
            break;
        }
    }
    if (a.peek_text.empty()) return;
    a.peek_turn = a.turn_start;
    a.peek_at = Now() + delay;
    a.peek_until = a.peek_at + kPeekMs;
    if (s.hwnd) SetTimer(s.hwnd, kPeekTimer, delay + 1, PeekTimerProc);
    Redraw(s);
}

void AiPeekHide(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.peek_at) return;
    a.peek_at = 0;
    a.peek_until = 0;
    if (s.hwnd) KillTimer(s.hwnd, kPeekTimer);
    Redraw(s);
}

void AiPeekFill(AppState& s, ui::AiPeekView& peek) {
    AiAssistantState& a = s.aiAssistant;
    if (a.peek_text.empty() || !a.peek_turn) return;
    const uint64_t now = Now();
    peek.on = !a.open && a.peek_at && now >= a.peek_at && now < a.peek_until;
    peek.text = a.peek_text;
    if (a.peek_failed) {
        peek.failed = true;
        peek.tone = a.peek_tone;
        peek.title = a.peek_title.empty() ? std::wstring(L"鹿铃助手 · 出错") : a.peek_title;
        peek.undo = a.peek_tone == ui::AiErrTone::Accent ? std::wstring(L"连接模型") : std::wstring(L"重试");
        peek.open = L"打开对话";
        return;
    }
    peek.title = L"鹿铃助手 · 完成";
    peek.undo.clear();   // this version's turns change nothing to undo
    peek.open = L"打开对话";
}

bool AiPeekActivate(AppState& s, ui::AiPanelPart part) {
    AiAssistantState& a = s.aiAssistant;
    switch (part) {
    case ui::AiPanelPart::PeekCard:
        return true;
    case ui::AiPanelPart::PeekClose:
        AiPeekHide(s);
        return true;
    case ui::AiPanelPart::PeekUndo:
        AiPeekHide(s);
        if (a.peek_failed) {
            if (a.peek_tone == ui::AiErrTone::Accent) {
                // 连接模型: the panel with the connection card open.
                AiAssistantOpen(s);
                a.menu_open = true;
                AiConnOpen(s);
            } else {
                AiAgentRetry(s);
            }
        }
        Redraw(s);
        return true;
    case ui::AiPanelPart::PeekOpen:
        AiPeekHide(s);
        AiAssistantOpen(s);
        s.renderer.AiPanel().ScrollToEnd();
        Redraw(s);
        return true;
    default:
        return false;
    }
}

void AiPeekDestroy(AppState& s) {
    if (s.hwnd) KillTimer(s.hwnd, kPeekTimer);
}

} // namespace luling
