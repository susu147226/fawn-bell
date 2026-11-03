// ai_assistant.cpp - 鹿铃助手: panel state, composer EDIT and pointer routing.
// Ported from Pulse 1.0.58 app/ai_assistant.cpp; the chat driver (network,
// tools) is not wired yet, so submitting keeps the conversation local.
#include "ai_assistant.h"

#include "ai_agent.h"
#include "ai_assistant_conn.h"
#include "ai_conversation.h"
#include "ai_peek.h"
#include "ai_model_key.h"
#include "ai_model_list.h"
#include "ai_model_stream.h"
#include "ai_settings.h"
#include "app_commands.h"
#include "app_internal.h"
#include "app_runtime.h"
#include "../common/json_value.h"
#include "../ui/confirm_dialog.h"
#include "../ui/edit_host.h"
#include "../ui/FluentTokens.h"
#include "../ui/folder_picker_dialog.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace luling {
namespace {

using ui::AiBlockKind;
using ui::AiBlockView;
using ui::AiModelView;
using ui::AiPanelHit;
using ui::AiPanelPart;
using ui::AiPanelViewModel;

constexpr UINT_PTR kComposerSubclass = 0x4149;  // 'AI'
constexpr uint64_t kMenuFadeMs = 240;
constexpr UINT_PTR kFrameTimer = 0x4141;  // 'AA'

void CALLBACK FrameTick(HWND hwnd, UINT, UINT_PTR, DWORD) { InvalidateRect(hwnd, nullptr, FALSE); }

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

COLORREF ToColorRef(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<BYTE>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return RGB(b(c.r), b(c.g), b(c.b));
}

// Models come from the connected service and are shown and sent as it names
// them (ai_model_list.h). The two official DeepSeek ones get a line.
struct KnownModel {
    const char* id;
    const wchar_t* detail;
};
constexpr KnownModel kKnownModels[] = {
    {"deepseek-flash", L"快，适合日常整理和查找"},
    {"deepseek-v4-pro", L"更强，适合复杂的多步任务"},
};
constexpr const wchar_t* kEffortNames[] = {L"关闭", L"低", L"中", L"高"};
constexpr const wchar_t* kEffortDetails[] = {
    L"不思考，直接回答，最快。",
    L"简单任务，略想一下就动手。",
    L"默认。速度和仔细程度平衡。",
    L"多步整理、复杂查找时更仔细，也更慢、更费 token。",
};

std::string& ChosenModel() {
    static std::string id = "deepseek-flash";
    return id;
}

const std::vector<std::string>& Listed() { return app::ai::ListedModels(AiAgentBaseUrl()); }

// The chosen model's place in the list: -1 when the service does not list it.
int ChosenIndex(const std::vector<std::string>& list) {
    const auto it = std::find(list.begin(), list.end(), ChosenModel());
    return it == list.end() ? -1 : static_cast<int>(it - list.begin());
}

// The id a request uses: the chosen one if listed, else the service's first.
const std::string& CurrentModel() {
    const std::vector<std::string>& list = Listed();
    if (list.empty() || ChosenIndex(list) >= 0) return ChosenModel();
    return list.front();
}

void LoadPrefs(AiAssistantState& a) {
    if (a.prefs_loaded) return;
    a.prefs_loaded = true;
    const app::ai::Settings settings = app::ai::LoadSettings();
    a.floating = settings.assistant.floating;
    a.effort = std::clamp(settings.assistant.effort, 0, 3);
    if (!settings.assistant.model.empty()) ChosenModel() = settings.assistant.model;
}

// The folder the assistant sees: the active tab's, if it is a real folder.
bool CurrentFolder(AppState& s, std::wstring& path, std::wstring& name, size_t& count) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty()) return false;
    if (tab->current_path.rfind(L"luling:", 0) == 0) return false;
    path = tab->current_path;
    name = app::TabTitle(path);
    count = tab->EntryCount();
    return !name.empty();
}

std::vector<ui::AiSuggestionView> Suggestions(AppState& s) {
    std::wstring path, folder;
    size_t count = 0;
    const bool has_folder = CurrentFolder(s, path, folder, count);
    struct Sug {
        ui::AiIcon icon;
        ui::AiTint tint;
        const wchar_t* title;
        const wchar_t* detail;
        bool folder;
    };
    static constexpr Sug kSugs[] = {
        {ui::AiIcon::Search, ui::AiTint::Document, L"找上个月的发票", L"用鹿铃索引搜所有位置，结果列在右边", false},
        {ui::AiIcon::Rename, ui::AiTint::Image, L"把选中的照片按拍摄日期重命名", L"只出规则，鹿铃在本机改名", false},
        {ui::AiIcon::Move, ui::AiTint::App, L"按类型整理「{folder}」", L"图片、文档、安装包…各放一个文件夹", true},
        {ui::AiIcon::Size, ui::AiTint::Archive, L"「{folder}」里什么最占空间？", L"按大小排出最大的文件和文件夹", true},
    };
    std::vector<ui::AiSuggestionView> out;
    for (const Sug& sug : kSugs) {
        // Folder suggestions only make sense with a folder open.
        if (sug.folder && !has_folder) continue;
        std::wstring title = sug.title;
        if (const size_t at = title.find(L"{folder}"); at != std::wstring::npos)
            title.replace(at, 8, folder);
        out.push_back({sug.icon, sug.tint, std::move(title), sug.detail});
    }
    return out;
}

void SavePrefs(const AiAssistantState& a) {
    bool malformed = false;
    app::ai::Settings settings = app::ai::LoadSettings(&malformed);
    if (malformed) return;   // never overwrite a file we could not read
    settings.assistant.floating = a.floating;
    settings.assistant.model = ChosenModel();
    settings.assistant.effort = a.effort;
    (void)app::ai::SaveSettings(settings);
}

// The docked column / floating card is drawn by the renderer; the EDIT rides
// on top of the composer at the same client coordinates.
void LayoutEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.edit) return;
    // Show the EDIT in both modes: the drawn card sits in client coordinates.
    const bool any = a.open && s.renderer.AiPanel().Visible();
    const bool menu_fading = !a.menu_open && ui::motion::NowMs() - a.menu_closed_at < kMenuFadeMs;
    const bool menu_covers = a.menu_open || menu_fading;
    const D2D1_RECT_F r = s.renderer.AiPanel().InputRect();
    if (!any || menu_covers || r.right <= r.left || r.bottom <= r.top) {
        const bool was = a.edit_visible;
        if (a.edit_visible) ShowWindow(a.edit, SW_HIDE);
        a.edit_visible = false;
        // The panel draws the text from the next frame on.
        if (was && a.open) Redraw(s);
        return;
    }
    const int x = static_cast<int>(r.left);
    const int y = static_cast<int>(r.top);
    const int w = static_cast<int>(r.right - r.left);
    const int h = static_cast<int>(r.bottom - r.top);
    if (w <= 0 || h <= 0) return;
    if (!a.edit_visible) {
        ShowWindow(a.edit, SW_SHOWNA);
        a.edit_visible = true;
        ui::PresentChildEdit(s.compositor, s.compositor.TextFormat(),
                             ui::ColorFromRef(ui::EditTextColor(s.darkMode)),
                             ui::ColorFromRef(ui::EditBackColor(s.darkMode)), a.edit);
    }
    SetWindowPos(a.edit, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
}

// The composer's text, for the frames the EDIT is hidden.
std::wstring ComposerText(AiAssistantState& a) {
    if (!a.edit) return {};
    const int n = GetWindowTextLengthW(a.edit);
    if (n <= 0) return {};
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(a.edit, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));
    return text;
}

void SyncComposerMetrics(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.edit) return;
    a.text_len = GetWindowTextLengthW(a.edit);
    const int line_h = std::max(16, static_cast<int>(21 * (s.scale > 0 ? s.scale : 1.0f)));
    a.line_px = line_h;
    a.lines = std::clamp(1 + a.text_len / 60, 1, 6);
    a.lines = a.text_len == 0 ? 1 : a.lines;
}

void FillComposer(AppState& s, const std::wstring& text) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.edit) return;
    SetWindowTextW(a.edit, text.c_str());
    SyncComposerMetrics(s);
    a.focus_pending = true;
    Redraw(s);
}

void EnsureFont(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    const float scale = s.scale > 0.0f ? s.scale : 1.0f;
    if (a.font && std::fabs(a.font_scale - scale) < 0.01f) return;
    if (a.font) DeleteObject(a.font);
    a.font = CreateFontW(-MulDiv(10, static_cast<int>(96 * scale), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                         FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    a.font_scale = scale;
}

LRESULT CALLBACK ComposerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR,
                              DWORD_PTR ref);

D2D1_COLOR_F ComposerForeground(const AppState& s) {
    return ui::ColorFromRef(ui::EditTextColor(s.darkMode));
}

D2D1_COLOR_F ComposerBackground(const AppState& s) {
    return ui::ColorFromRef(ui::EditBackColor(s.darkMode));
}

LRESULT PresentedComposer(AppState& s, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return ui::DefPresentedChildEditProc(s.compositor, s.compositor.TextFormat(),
                                         ComposerForeground(s), ComposerBackground(s),
                                         hwnd, msg, wp, lp);
}

void EnsureEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (a.edit) return;
    EnsureFont(s);
    // The panel is part of the main window's composition surface: a plain
    // GDI child is painted underneath it, so typing looks like no input.
    a.edit = ui::CreateRedirectedChildEdit(s.hwnd, L"", ES_MULTILINE | ES_AUTOVSCROLL);
    if (!a.edit) return;
    // Chinese glyphs fill their em box; draw CJK runs a little smaller in the
    // composer while Latin keeps the configured size.
    SetPropW(a.edit, L"Luling.Edit.SmallCjk", reinterpret_cast<HANDLE>(1));
    SendMessageW(a.edit, EM_SETLIMITTEXT, 8000, 0);
    SendMessageW(a.edit, WM_SETFONT, reinterpret_cast<WPARAM>(a.font), TRUE);
    SetWindowSubclass(a.edit, ComposerProc, kComposerSubclass, reinterpret_cast<DWORD_PTR>(&s));
}

void AppendBlock(AppState& s, AiBlockKind kind, const std::wstring& text, bool user) {
    AiAssistantState& a = s.aiAssistant;
    AiBlockView block;
    block.kind = kind;
    block.id = a.next_block_id++;
    block.text = text;
    block.born_ms = GetTickCount64();
    (void)user;
    a.blocks.push_back(std::move(block));
}

void SubmitText(AppState& s, const std::wstring& text) {
    AiAssistantState& a = s.aiAssistant;
    if (text.empty()) return;
    AiAgentSend(s, text);
    a.focused = true;
    if (a.edit) SetFocus(a.edit);
    s.renderer.AiPanel().ScrollToEnd();
    Redraw(s);
}

void SubmitComposer(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.edit) return;
    const int length = GetWindowTextLengthW(a.edit);
    if (length <= 0) return;
    std::wstring text(static_cast<size_t>(length), L'\0');
    GetWindowTextW(a.edit, text.data(), length + 1);
    SetWindowTextW(a.edit, L"");
    SyncComposerMetrics(s);
    SubmitText(s, text);
}

void CloseMenu(AppState& s, bool redraw_now = true) {
    AiAssistantState& a = s.aiAssistant;
    AiConnClose(s);
    if (!a.menu_open) return;
    a.menu_open = false;
    a.menu_closed_at = ui::motion::NowMs();
    if (redraw_now) Redraw(s);
}

void OpenPanel(AppState& s, bool animate) {
    AiAssistantState& a = s.aiAssistant;
    LoadPrefs(a);
    if (!a.conversation_loaded) {
        a.conversation_loaded = true;
        app::ai::LoadConversation(s);
    }
    a.open = true;
    a.unread = false;
    EnsureEdit(s);
    s.renderer.SetAiPanel(true,
                          a.floating ? ui::AiPanelMode::Floating : ui::AiPanelMode::Docked, animate);
    a.focus_pending = true;
    Redraw(s);
}

void ClosePanel(AppState& s, bool animate) {
    AiAssistantState& a = s.aiAssistant;
    a.open = false;
    app::ai::SaveConversation(s);
    CloseMenu(s, false);
    s.renderer.SetAiPanel(false,
                          a.floating ? ui::AiPanelMode::Floating : ui::AiPanelMode::Docked, animate);
    if (a.edit) ShowWindow(a.edit, SW_HIDE);
    a.edit_visible = false;
    Redraw(s);
}

void AttachContextPaths(AppState& s, const std::vector<std::wstring>& paths, const wchar_t* heading) {
    if (paths.empty()) return;
    AiAssistantState& a = s.aiAssistant;
    std::wstring text = ComposerText(a);
    if (!text.empty() && text.back() != L'\n') text += L'\n';
    text += heading;
    size_t added = 0;
    for (const std::wstring& path : paths) {
        if (path.empty() || added++ >= 20) continue;
        text += L"- " + path + L"\n";
    }
    if (a.edit) SetWindowTextW(a.edit, text.c_str());
    SyncComposerMetrics(s);
    a.focus_pending = true;
}

void ClearChat(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    AiAgentReset(s);
    a.blocks.clear();
    app::ai::ClearConversation();
    a.busy = false;
    a.context_removed_for.clear();
    if (a.edit) SetWindowTextW(a.edit, L"");
    SyncComposerMetrics(s);
    a.focus_pending = true;
}

void Activate(AppState& s, const AiPanelHit& hit) {
    AiAssistantState& a = s.aiAssistant;
    if (AiPeekActivate(s, hit.part)) return;
    if (bool close_menu = false; AiConnActivate(s, hit, close_menu)) {
        if (close_menu) {
            CloseMenu(s, false);
            a.focus_pending = true;
        }
        Redraw(s);
        return;
    }
    if (a.menu_open) {
        // Only the menu itself (including the connection card it unfolds into
        // and the button that toggles it) keeps the menu open; every other
        // click in the panel dismisses it.
        const bool in_menu = hit.part == AiPanelPart::Menu ||
                             hit.part == AiPanelPart::MenuModel ||
                             hit.part == AiPanelPart::MenuEffort ||
                             hit.part == AiPanelPart::MenuManage ||
                             hit.part == AiPanelPart::ModelButton ||
                             hit.part == AiPanelPart::ConnCard ||
                             AiConnIsField(hit.part);
        if (!in_menu) CloseMenu(s, false);
    }
    switch (hit.part) {
    case AiPanelPart::Close:
        ClosePanel(s, true);
        break;
    case AiPanelPart::Mode:
        CloseMenu(s, false);
        a.floating = !a.floating;
        a.focus_pending = GetFocus() == a.edit;
        s.renderer.SetAiPanel(true, a.floating ? ui::AiPanelMode::Floating : ui::AiPanelMode::Docked,
                              true);
        SavePrefs(a);
        Redraw(s);
        break;
    case AiPanelPart::NewChat:
        CloseMenu(s, false);
        ClearChat(s);
        Redraw(s);
        break;
    case AiPanelPart::Send:
        CloseMenu(s, false);
        if (a.busy) AiAgentStop(s);
        else SubmitComposer(s);
        a.focus_pending = true;
        break;
    case AiPanelPart::ModelButton:
        if (a.menu_open) {
            CloseMenu(s);
        } else {
            a.menu_open = true;
            if (AiAgentHasKey()) app::ai::RefreshListedModels(s.hwnd, AiAgentBaseUrl());
            Redraw(s);
        }
        break;
    case AiPanelPart::MenuModel: {
        const std::vector<std::string>& list = Listed();
        if (hit.index >= 0 && hit.index < static_cast<int>(list.size()) &&
            list[static_cast<size_t>(hit.index)] != CurrentModel()) {
            ChosenModel() = list[static_cast<size_t>(hit.index)];
            SavePrefs(a);
        }
        CloseMenu(s);
        a.focus_pending = true;
        break;
    }
    case AiPanelPart::MenuEffort:
        // The menu stays open to show the chosen strength's line.
        if (hit.index >= 0 && hit.index < 4 && hit.index != a.effort) {
            a.effort = hit.index;
            SavePrefs(a);
        }
        break;
    case AiPanelPart::MenuManage:
        // The menu unfolds into the 管理连接 card in place.
        AiConnOpen(s);
        break;
    case AiPanelPart::ClearChat: {
        CloseMenu(s, false);
        ui::ConfirmDialogSpec spec;
        spec.title = L"清空聊天记录";
        spec.message = L"删除当前对话的全部记录，无法恢复。";
        spec.confirm_text = L"清空";
        spec.cancel_text = L"取消";
        spec.danger = true;
        spec.default_choice = ui::ConfirmChoice::Cancel;
        if (ui::ShowConfirmDialog(s.hwnd, spec, s.darkMode, s.accentColor)) ClearChat(s);
        Redraw(s);
        break;
    }
    case AiPanelPart::Menu:
        CloseMenu(s);
        break;
    case AiPanelPart::Suggestion: {
        CloseMenu(s, false);
        const std::vector<ui::AiSuggestionView> suggestions = Suggestions(s);
        if (hit.index >= 0 && hit.index < static_cast<int>(suggestions.size())) {
            const std::wstring& title = suggestions[static_cast<size_t>(hit.index)].title;
            // 找上个月的发票 only fills the composer: "last month" is easy to change.
            if (title == L"找上个月的发票") FillComposer(s, L"找一下上个月的发票，PDF 和图片都算");
            else SubmitText(s, title);
        }
        break;
    }
    case AiPanelPart::ErrAction:
        AiAgentErrorAction(s, hit.index / 4, hit.index % 4);
        break;
    case AiPanelPart::ConfirmAccept:
        AiAgentConfirm(s, true);
        break;
    case AiPanelPart::ConfirmDeny:
        AiAgentConfirm(s, false);
        break;
    case AiPanelPart::ResultUndo:
        AiAgentUndoResult(s, hit.index);
        break;
    case AiPanelPart::ContextRemove: {
        std::wstring path, name;
        size_t count = 0;
        if (CurrentFolder(s, path, name, count)) a.context_removed_for = path;
        a.focus_pending = true;
        Redraw(s);
        break;
    }
    case AiPanelPart::AddFiles: {
        CloseMenu(s, false);
        int command = 0;
        if (EnsureMenu(s)) {
            std::vector<ui::FluentMenuItem> items(2);
            items[0].command = 1;
            items[0].text = L"添加文件…";
            items[1].command = 2;
            items[1].text = L"添加文件夹…";
            POINT pt{};
            GetCursorPos(&pt);
            command = s.menu->TrackPopup(pt, std::move(items));
        }
        if (command == 1) {
            ui::FolderPickerSpec spec;
            spec.mode = ui::PickerMode::File;
            spec.allow_multiselect = true;
            spec.title = L"添加文件到对话";
            ui::FilePickerResult picked;
            if (ui::ShowFilePicker(s.hwnd, spec, s.darkMode, s.accentColor, picked))
                AttachContextPaths(s, picked.paths, L"参考这些文件：\n");
        } else if (command == 2) {
            ui::FolderPickerSpec spec;
            spec.mode = ui::PickerMode::Folder;
            spec.title = L"添加文件夹到对话";
            ui::FilePickerResult picked;
            if (ui::ShowFilePicker(s.hwnd, spec, s.darkMode, s.accentColor, picked) &&
                !picked.paths.empty()) {
                AttachContextPaths(s, {picked.paths.front()}, L"参考这个文件夹：\n");
            }
        }
        if (a.edit) {
            SetFocus(a.edit);
            a.focused = true;
        }
        Redraw(s);
        break;
    }
    case AiPanelPart::Input:
        if (a.edit) {
            SetFocus(a.edit);
            a.focused = true;
            Redraw(s);
        }
        break;
    default:
        break;
    }
}

LRESULT CALLBACK ComposerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR,
                              DWORD_PTR ref) {
    auto* s = reinterpret_cast<AppState*>(ref);
    if (!s) return DefSubclassProc(hwnd, msg, wParam, lParam);
    LRESULT early = 0;
    if (ui::HandleChildEditMessage(s->compositor, s->compositor.TextFormat(),
                                   ComposerForeground(*s), ComposerBackground(*s),
                                   s->editBrush, hwnd, msg, wParam, lParam, early))
        return early;
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
            SubmitComposer(*s);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            AiAssistantEscape(*s);
            return 0;
        }
        break;
    case WM_SETFOCUS:
        s->aiAssistant.focused = true;
        InvalidateRect(s->hwnd, nullptr, FALSE);
        break;
    case WM_KILLFOCUS:
        s->aiAssistant.focused = false;
        InvalidateRect(s->hwnd, nullptr, FALSE);
        break;
    case WM_CHAR:
        if (wParam == VK_RETURN) return 0;
        break;
    default:
        break;
    }
    const LRESULT result = PresentedComposer(*s, hwnd, msg, wParam, lParam);
    if (msg == WM_CHAR || msg == WM_PASTE || msg == WM_CUT || msg == WM_KEYUP)
        InvalidateRect(s->hwnd, nullptr, FALSE);
    return result;
}

uint64_t g_key_checked = 0;
bool g_key_present = false;
bool g_base_loaded = false;
std::wstring g_base_url;

}  // namespace

bool AiAgentHasKey() {
    const uint64_t now = GetTickCount64();
    if (!g_key_checked || now - g_key_checked > 2000) {
        g_key_present = app::ai::HasModelKey();
        g_key_checked = now;
    }
    return g_key_present;
}

void AiAgentKeyChanged() {
    g_key_checked = 0;
    g_base_loaded = false;
}

std::wstring AiAgentBaseUrl() {
    if (!g_base_loaded) {
        g_base_loaded = true;
        const std::string url = app::ai::LoadSettings().assistant.base_url;
        g_base_url = url.empty() ? AiAgentDefaultBaseUrl() : json::Wide(url);
    }
    return g_base_url;
}

std::wstring AiAgentDefaultBaseUrl() { return L"https://api.deepseek.com"; }

std::wstring AiAgentHost() { return app::ai::ModelHost(AiAgentBaseUrl()); }

std::wstring AiAssistantModelName(const AppState&) { return json::Wide(CurrentModel()); }

void AiAssistantToggle(AppState& s) {
    if (s.aiAssistant.open) AiAssistantClose(s, true);
    else AiAssistantOpen(s, true);
}

void AiAssistantHotkey(AppState& s) {
    if (s.aiAssistant.open) ClosePanel(s, true);
    else OpenPanel(s, true);
}

void AiAssistantOpen(AppState& s, bool animate) { OpenPanel(s, animate); }

void AiAssistantClose(AppState& s, bool animate) { ClosePanel(s, animate); }

void FillAiAssistantVm(AppState& s, ui::WindowViewModel& vm) {
    AiAssistantState& a = s.aiAssistant;
    // The chip is always there: "✦ AI" opens the panel.
    if (vm.status.ai_state == ui::StatusAiState::Hidden) vm.status.ai_state = ui::StatusAiState::Idle;
    if (vm.status.ai_state == ui::StatusAiState::Idle) vm.status.ai_text.clear();
    vm.status.ai_label = L"AI";
    vm.status.ai_open = a.open;
    vm.status.ai_pressed = a.chip_pressed;
    // While the assistant works its words take the chip.
    if (a.chip != ui::StatusAiState::Idle) {
        vm.status.ai_state = a.chip;
        vm.status.ai_text = a.chip_text;
        vm.status.ai_progress = a.chip_progress;
        vm.status.ai_unread = a.unread;
    }
    ui::AiPanelViewModel& p = vm.ai_panel;
    p.dark = vm.dark;
    p.hover = a.hover;
    p.pressed = a.pressed;
    AiPeekFill(s, p.peek);
    if (!a.open && !s.renderer.AiPanel().Visible()) return;
    LoadPrefs(a);

    p.dark = s.darkMode;
    p.title = L"鹿铃助手";
    p.empty = a.blocks.empty();
    p.blocks = a.blocks;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    p.greeting = now.wHour < 11 ? L"早上好，想整理点什么？"
                 : now.wHour < 13 ? L"中午好，想整理点什么？"
                 : now.wHour < 18 ? L"下午好，想整理点什么？"
                                  : L"晚上好，想整理点什么？";
    p.intro = L"我能看到你在鹿铃里打开的文件夹和选中的项目，可以帮你找文件、整理、重命名、加标签。";
    p.safe_note = L"所有修改都会先在鹿铃里让你确认，做完可以撤销。";
    std::wstring path, folder;
    size_t count = 0;
    const bool has_folder = CurrentFolder(s, path, folder, count);
    p.suggestions = Suggestions(s);
    p.context_visible = has_folder && a.context_removed_for != path;
    if (p.context_visible)
        p.context_text = folder + L" · " + std::to_wstring(count) + L" 项";
    p.placeholder = L"让鹿铃助手帮你找文件、整理文件夹…";
    p.add_files = L"添加文件";
    p.composer_focused = a.focused && a.edit_visible;
    SyncComposerMetrics(s);
    p.composer_empty = a.text_len == 0;
    p.composer_lines = a.lines;
    p.composer_line_px = static_cast<float>(a.line_px);
    const bool menu_fading = !a.menu_open && ui::motion::NowMs() - a.menu_closed_at < kMenuFadeMs;
    if (!p.composer_empty && (!a.edit_visible || menu_fading)) {
        // The EDIT is hidden (menu, opening): the panel draws its text.
        p.draw_composer_text = true;
        p.composer_text = ComposerText(a);
    }
    p.send = a.busy ? ui::AiSendState::Busy
                    : (a.text_len == 0 ? ui::AiSendState::Dim : ui::AiSendState::Ready);

    p.menu_open = a.menu_open;
    p.model_heading = L"模型";
    p.effort_heading = L"思考强度";
    // Models come from the connected service (ai_model_list.h); before it
    // listed anything, the chosen one alone.
    const std::vector<std::string>& listed = Listed();
    const std::vector<std::string> alone{CurrentModel()};
    const std::vector<std::string>& rows = listed.empty() ? alone : listed;
    p.models.clear();
    for (const std::string& id : rows) {
        std::wstring detail;
        for (const KnownModel& k : kKnownModels)
            if (id == k.id) detail = k.detail;
        p.models.push_back({json::Wide(id), std::move(detail)});
    }
    const auto chosen = std::find(rows.begin(), rows.end(), CurrentModel());
    p.model = chosen == rows.end() ? -1 : static_cast<int>(chosen - rows.begin());
    p.model_label = json::Wide(CurrentModel());
    a.model = p.model;
    a.model_id = p.model_label;
    p.effort = std::clamp(a.effort, 0, 3);
    p.effort_label = p.effort == 0 ? std::wstring() : L" · " + std::wstring(kEffortNames[p.effort]);
    for (size_t i = 0; i < 4; ++i) p.effort_names[i] = kEffortNames[i];
    p.effort_detail = kEffortDetails[p.effort];
    // Connected once a key is stored (ai_model_key.h).
    p.connected = AiAgentHasKey();
    p.connection_text = p.connected ? (L"已连接 " + AiAgentHost()) : std::wstring(L"未连接 · 需要 API 密钥");
    p.manage_text = L"管理连接";
    AiConnFill(s, p);
}

void AiAssistantLayout(AppState& s) {
    AiConnLayout(s);
    EnsureEdit(s);
    EnsureFont(s);
    LayoutEdit(s);
    // The panel's springs and the peek bubble's fade advance on frames; the
    // app only paints on demand, so ask for them while something moves.
    if (!s.hwnd) return;
    if (s.renderer.AiPanel().Animating()) SetTimer(s.hwnd, kFrameTimer, 16, FrameTick);
    else KillTimer(s.hwnd, kFrameTimer);
}

// The bubble first (it floats over the chip), then the panel.
ui::AiPanelHit HitAt(AppState& s, int x, int y) {
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const ui::AiPanelHit peek = view.PeekHitTest(static_cast<float>(x), static_cast<float>(y));
    if (peek.part != ui::AiPanelPart::None) return peek;
    return view.HitTest(static_cast<float>(x), static_cast<float>(y));
}

bool Reachable(AppState& s) {
    return s.aiAssistant.open || s.renderer.AiPanel().PeekVisible();
}

bool AiAssistantMouseMove(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    if (a.float_drag) {
        s.renderer.AiPanel().SetFloatingOffset(a.drag_dx + static_cast<float>(x - a.drag_start.x),
                                               a.drag_dy + static_cast<float>(y - a.drag_start.y));
        Redraw(s);
        return true;
    }
    if (!Reachable(s)) return false;
    const AiPanelHit hit = HitAt(s, x, y);
    if (hit.part == AiPanelPart::None) {
        if (a.hover.part != AiPanelPart::None) {
            a.hover = {};
            Redraw(s);
        }
        return false;
    }
    if (!(a.hover == hit)) {
        a.hover = hit;
        a.hover_point = POINT{x, y};
        Redraw(s);
    }
    return true;
}

bool AiAssistantMouseDown(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    if (!Reachable(s)) return false;
    const AiPanelHit hit = HitAt(s, x, y);
    if (hit.part == AiPanelPart::None) {
        // A click anywhere outside the card dismisses an open model menu.
        if (a.menu_open) CloseMenu(s);
        return false;
    }
    // A floating card moves by its header; the docked column never moves.
    if (a.floating && hit.part == AiPanelPart::Panel) {
        const D2D1_RECT_F head = s.renderer.AiPanel().HeaderRect();
        if (x >= head.left && x < head.right && y >= head.top && y < head.bottom) {
            a.float_drag = true;
            a.drag_start = POINT{x, y};
            a.drag_dx = s.renderer.AiPanel().FloatingDx();
            a.drag_dy = s.renderer.AiPanel().FloatingDy();
            SetCapture(s.hwnd);
            return true;
        }
    }
    a.pressed = hit;
    Redraw(s);
    return true;
}

bool AiAssistantMouseUp(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    if (a.float_drag) {
        a.float_drag = false;
        if (GetCapture() == s.hwnd) ReleaseCapture();
        Redraw(s);
        return true;
    }
    if (!Reachable(s)) return false;
    const AiPanelHit hit = HitAt(s, x, y);
    const AiPanelHit pressed = a.pressed;
    a.pressed = {};
    if (pressed.part == AiPanelPart::None && hit.part == AiPanelPart::None) return false;
    if (pressed == hit && pressed.part != AiPanelPart::None) Activate(s, hit);
    Redraw(s);
    return true;
}

bool AiAssistantWheel(AppState& s, int x, int y, int delta) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) return false;
    const AiPanelHit hit = s.renderer.AiPanel().HitTest(static_cast<float>(x), static_cast<float>(y));
    if (hit.part == AiPanelPart::None) return false;
    s.renderer.AiPanel().ScrollBy(-static_cast<float>(delta) * 0.5f);
    Redraw(s);
    return true;
}

bool AiAssistantSetCursor(AppState& s, int x, int y) {
    if (!Reachable(s)) return false;
    const AiPanelHit hit = HitAt(s, x, y);
    if (hit.part == AiPanelPart::None) return false;
    if (hit.part == AiPanelPart::Input || AiConnIsField(hit.part)) {
        SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        return true;
    }
    return false;
}

bool AiAssistantEscape(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) return false;
    if (a.menu_open) {
        CloseMenu(s);
        return true;
    }
    ClosePanel(s, true);
    return true;
}

void AiAssistantMouseLeave(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (a.hover.part == AiPanelPart::None) return;
    a.hover = {};
    Redraw(s);
}

HBRUSH AiAssistantCtlColor(AppState& s, HDC hdc, HWND edit) {
    AiAssistantState& a = s.aiAssistant;
    if (!edit || edit != a.edit || !a.open) return nullptr;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const COLORREF back = ToColorRef(view.CardColor());
    if (!a.brush || a.brush_color != back) {
        if (a.brush) DeleteObject(a.brush);
        a.brush = CreateSolidBrush(back);
        a.brush_color = back;
    }
    SetTextColor(hdc, ToColorRef(view.TextColor()));
    SetBkColor(hdc, back);
    SetBkMode(hdc, OPAQUE);
    return a.brush;
}

void AiAssistantDestroy(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    app::ai::SaveConversation(s);
    AiAgentDestroy(s);
    AiPeekDestroy(s);
    if (s.hwnd) KillTimer(s.hwnd, kFrameTimer);
    AiConnDestroy(s);
    if (a.edit) {
        RemoveWindowSubclass(a.edit, ComposerProc, kComposerSubclass);
        DestroyWindow(a.edit);
        a.edit = nullptr;
    }
    if (a.font) {
        DeleteObject(a.font);
        a.font = nullptr;
        a.font_scale = 0.0f;
    }
    if (a.brush) {
        DeleteObject(a.brush);
        a.brush = nullptr;
        a.brush_color = 0;
    }
}

}  // namespace luling
