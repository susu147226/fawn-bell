// ai_settings_edits.cpp - Settings > AI 助手: the key / address EDITs and the
// 连接并读取模型 probe.
#include "ai_settings_edits.h"

#include "ai_assistant.h"
#include "ai_model_key.h"
#include "ai_model_list.h"
#include "ai_providers.h"
#include "ai_settings.h"
#include "app_internal.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/edit_host.h"
#include "../ui/FluentTokens.h"
#include "../ui/typography.h"
#include "../ui/ui_renderer.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>

namespace luling {
namespace {

constexpr UINT_PTR kEditSubclass = 0x415A;
constexpr UINT_PTR kProbeTimer = 0x4158;
constexpr wchar_t kMaskChar = L'\x2022';

const wchar_t* KeyCue() {
    return l10n::Pick(L"粘贴新的 API Key（留空则保留原值）",
                      L"Paste a new API key (empty keeps the current one)");
}

std::shared_ptr<AiSettingsEdits>& Slot(AppState& s) { return s.aiAssistant.settings_edits; }
AiSettingsEdits* Ui(AppState& s) {
    auto& slot = Slot(s);
    if (!slot) slot = std::make_shared<AiSettingsEdits>();
    return slot.get();
}
void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}
void Present(AppState& s, HWND hwnd);

std::wstring EditText(HWND edit) {
    if (!edit) return {};
    const int n = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));
    return text;
}

std::wstring Trim(std::wstring text) {
    auto space = [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0x3000;
    };
    size_t a = 0, b = text.size();
    while (a < b && space(text[a])) ++a;
    while (b > a && space(text[b - 1])) --b;
    return text.substr(a, b - a);
}

void Wipe(std::wstring& text) {
    if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    text.clear();
}

void PasteClipboard(HWND edit) {
    if (!edit || !OpenClipboard(edit)) return;
    std::wstring text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* raw = static_cast<const wchar_t*>(GlobalLock(data))) {
            text.assign(raw);
            GlobalUnlock(data);
        }
    }
    if (text.empty()) {
        if (HANDLE data = GetClipboardData(CF_TEXT)) {
            if (const auto* raw = static_cast<const char*>(GlobalLock(data))) {
                const int size = MultiByteToWideChar(CP_ACP, 0, raw, -1, nullptr, 0);
                if (size > 1) {
                    text.resize(static_cast<size_t>(size - 1));
                    MultiByteToWideChar(CP_ACP, 0, raw, -1, text.data(), size);
                }
                GlobalUnlock(data);
            }
        }
    }
    CloseClipboard();
    // A key or URL is one line: drop the line breaks an editor may have
    // copied along with it.
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](wchar_t ch) { return ch == L'\r' || ch == L'\n'; }),
               text.end());
    if (!text.empty()) {
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
    }
    Wipe(text);
}

void ShowEditMenu(HWND edit, LPARAM lp) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    DWORD sel_start = 0, sel_end = 0;
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start), reinterpret_cast<LPARAM>(&sel_end));
    const bool selection = sel_end > sel_start;
    const bool has_text = GetWindowTextLengthW(edit) > 0;
    const bool can_paste = IsClipboardFormatAvailable(CF_UNICODETEXT) ||
                           IsClipboardFormatAvailable(CF_TEXT);
    auto add = [&](UINT id, l10n::StringId text, bool enabled) {
        AppendMenuW(menu, MF_STRING | (enabled ? 0u : MF_GRAYED), id,
                    l10n::Get(text).c_str());
    };
    add(1, l10n::StringId::Undo, SendMessageW(edit, EM_CANUNDO, 0, 0) != FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    add(2, l10n::StringId::Cut, selection);
    add(3, l10n::StringId::Copy, selection);
    add(4, l10n::StringId::Paste, can_paste);
    add(5, l10n::StringId::SelectAll, has_text);
    POINT pt{};
    if (lp == static_cast<LPARAM>(-1)) GetCursorPos(&pt);
    else pt = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, edit, nullptr);
    DestroyMenu(menu);
    switch (command) {
    case 1: SendMessageW(edit, WM_UNDO, 0, 0); break;
    case 2: SendMessageW(edit, WM_CUT, 0, 0); break;
    case 3: SendMessageW(edit, WM_COPY, 0, 0); break;
    case 4: PasteClipboard(edit); break;
    case 5: SendMessageW(edit, EM_SETSEL, 0, -1); break;
    default: break;
    }
}

std::wstring ServiceBase() {
    bool malformed = false;
    const app::ai::Settings st = app::ai::LoadSettings(&malformed);
    std::wstring base = json::Wide(st.assistant.base_url);
    if (base.empty()) base = std::wstring(app::ai::DefaultProvider().base_url);
    return app::ai::NormalizeBaseUrl(std::move(base));
}

void StoreBase(const std::wstring& url) {
    bool malformed = false;
    app::ai::Settings st = app::ai::LoadSettings(&malformed);
    if (malformed) return;
    const std::wstring def(app::ai::DefaultProvider().base_url);
    st.assistant.base_url = app::ai::SameBaseUrl(url, def) ? std::string() : json::Utf8(url);
    if (!app::ai::SaveSettings(st)) return;
    AiAgentKeyChanged();
}

void CommitKey(AppState& s, AiSettingsEdits& u) {
    if (!u.key_edit) return;
    std::wstring key = Trim(EditText(u.key_edit));
    if (!key.empty() && app::ai::SaveModelKey(key)) {
        u.probed = false;
        u.probe_ok = false;
        ++u.gen;
        AiAgentKeyChanged();
    }
    Wipe(key);
    Redraw(s);
}

void CommitBase(AppState& s, AiSettingsEdits& u) {
    if (!u.base_edit) return;
    std::wstring url = app::ai::NormalizeBaseUrl(EditText(u.base_edit));
    if (url.empty()) url = std::wstring(app::ai::DefaultProvider().base_url);
    StoreBase(url);
    Redraw(s);
}

void HideEdit(AppState& s, HWND edit, bool& shown) {
    if (!edit || !shown) return;
    const bool had_focus = GetFocus() == edit;
    ShowWindow(edit, SW_HIDE);
    shown = false;
    if (had_focus && s.hwnd) SetFocus(s.hwnd);
}

void EnsureFont(AppState& s, AiSettingsEdits& u) {
    if (u.font && u.font_scale == s.scale) return;
    if (u.font) DeleteObject(u.font);
    u.font = ui::typography::CreateEditFont(s.scale, 13.0f);
    u.font_scale = s.scale;
    u.line_px = 0;
    for (HWND e : {u.key_edit, u.base_edit}) {
        if (!e || !u.font) continue;
        SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(u.font), TRUE);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
    if (u.font && s.hwnd) {
        HDC hdc = GetDC(s.hwnd);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, u.font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(s.hwnd, hdc);
        u.line_px = static_cast<int>(tm.tmHeight);
    }
}

void Place(AppState& s, AiSettingsEdits& u, HWND edit, bool& shown, const D2D1_RECT_F& r,
           bool& focus_pending) {
    if (!edit || r.right <= r.left) {
        HideEdit(s, edit, shown);
        return;
    }
    const int line = (std::max)(u.line_px, 1);
    const int x = static_cast<int>(std::lround(r.left));
    const int w = (std::max)(1, static_cast<int>(std::lround(r.right - r.left)));
    const int y = static_cast<int>(std::lround((r.top + r.bottom) * 0.5f - static_cast<float>(line) * 0.5f));
    RECT cur{};
    GetWindowRect(edit, &cur);
    MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&cur), 2);
    const bool placed =
        cur.left == x && cur.top == y && cur.right - cur.left == w && cur.bottom - cur.top == line;
    // `shown` is the state flag: after HideEdit the rect is unchanged, so the
    // position check alone would leave the window hidden on the next open.
    if (!placed || !shown || !IsWindowVisible(edit)) {
        SetWindowPos(edit, HWND_TOP, x, y, w, line, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        shown = true;
        Present(s, edit);
        Redraw(s);   // the page stops drawing the field's text itself
    }
    if (focus_pending) {
        focus_pending = false;
        if (GetForegroundWindow() == s.hwnd || GetForegroundWindow() == GetAncestor(s.hwnd, GA_ROOT)) {
            SetFocus(edit);
            const int n = GetWindowTextLengthW(edit);
            SendMessageW(edit, EM_SETSEL, n, n);
        }
    }
}

D2D1_COLOR_F EditForeground(const AppState& s) {
    return ui::ColorFromRef(ui::EditTextColor(s.darkMode));
}

D2D1_COLOR_F EditBackground(const AppState& s) {
    return ui::ColorFromRef(ui::EditBackColor(s.darkMode));
}

// These EDITs live in the main window, under its composition surface: a plain
// GDI child disappears there. Present through the same redirected editor
// surface the address bar uses.
LRESULT Presented(AppState& s, HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return ui::DefPresentedChildEditProc(s.compositor, s.compositor.TextFormat(),
                                         EditForeground(s), EditBackground(s), hwnd, msg, wp, lp);
}

void Present(AppState& s, HWND hwnd) {
    if (!hwnd || !IsWindowVisible(hwnd)) return;
    ui::PresentChildEdit(s.compositor, s.compositor.TextFormat(), EditForeground(s),
                         EditBackground(s), hwnd);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    AppState& s = *reinterpret_cast<AppState*>(ref);
    AiSettingsEdits* u = Slot(s).get();
    if (!u) return DefSubclassProc(hwnd, msg, wp, lp);
    LRESULT early = 0;
    if (ui::HandleChildEditMessage(s.compositor, s.compositor.TextFormat(), EditForeground(s),
                                   EditBackground(s), s.editBrush, hwnd, msg, wp, lp, early))
        return early;
    switch (msg) {
    case WM_GETDLGCODE:
        return Presented(s, hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wp == L'V' || wp == L'v')) {
            PasteClipboard(hwnd);
            return 0;
        }
        if ((GetKeyState(VK_SHIFT) & 0x8000) && wp == VK_INSERT) {
            PasteClipboard(hwnd);
            return 0;
        }
        if (wp == VK_RETURN) {
            if (hwnd == u->key_edit) {
                CommitKey(s, *u);
                HideEdit(s, u->key_edit, u->key_open);
            } else {
                CommitBase(s, *u);
                HideEdit(s, u->base_edit, u->base_open);
            }
            Redraw(s);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            HideEdit(s, hwnd, hwnd == u->key_edit ? u->key_open : u->base_open);
            Redraw(s);
            return 0;
        }
        break;
    case WM_CHAR:
        // 0x16 is the control character TranslateMessage makes from Ctrl+V.
        // WM_KEYDOWN above already pasted once; letting this through makes the
        // native EDIT paste a second copy.
        if (wp == L'\r' || wp == L'\n' || wp == 27 || wp == 0x16) return 0;
        break;
    case WM_PASTE:
        PasteClipboard(hwnd);
        return 0;
    case WM_CONTEXTMENU:
        ShowEditMenu(hwnd, lp);
        return 0;
    case WM_KILLFOCUS: {
        const LRESULT r = Presented(s, hwnd, msg, wp, lp);
        if (hwnd == u->key_edit && u->key_open) {
            CommitKey(s, *u);
            HideEdit(s, u->key_edit, u->key_open);
        } else if (hwnd == u->base_edit && u->base_open) {
            CommitBase(s, *u);
            HideEdit(s, u->base_edit, u->base_open);
        }
        Redraw(s);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, EditProc, kEditSubclass);
        break;
    default:
        break;
    }
    return Presented(s, hwnd, msg, wp, lp);
}

HWND MakeEdit(AppState& s, bool password, const wchar_t* cue) {
    const DWORD style =
        WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0);
    HWND e = ui::CreateRedirectedChildEdit(s.hwnd, L"", style);
    if (!e) return nullptr;
    SetWindowTheme(e, L"", L"");
    SendMessageW(e, EM_LIMITTEXT, 512, 0);
    if (password) SendMessageW(e, EM_SETPASSWORDCHAR, kMaskChar, 0);
    if (cue) SendMessageW(e, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
    SetWindowSubclass(e, EditProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(&s));
    return e;
}

void ProbeDone(AppState& s, AiSettingsEdits& u, app::ai::ModelProbe result) {
    u.probing = false;
    u.probed = true;
    u.probe_ok = result.http_status == 200;
    u.status_code = result.http_status;
    u.ms = result.ms;
    u.model_count = result.models.size();
    u.probed_at = GetTickCount64();
    if (u.probe_ok) {
        app::ai::StoreListedModels(u.probe_url, result.models);
        // A model the service does not list would fail the first turn: use the
        // first one it does list.
        bool malformed = false;
        app::ai::Settings st = app::ai::LoadSettings(&malformed);
        if (!malformed && !result.models.empty()) {
            bool listed = false;
            for (const std::string& id : result.models) listed |= id == st.assistant.model;
            if (!listed) {
                st.assistant.model = result.models.front();
                if (app::ai::SaveSettings(st)) AiAgentKeyChanged();
            }
        }
    }
    Redraw(s);
}

void CALLBACK ProbeTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    AiSettingsEdits* u = s ? Slot(*s).get() : nullptr;
    if (!u || !u->job) {
        KillTimer(hwnd, id);
        return;
    }
    if (u->job->gen != u->gen) {
        u->job.reset();
        u->probing = false;
        KillTimer(hwnd, id);
        return;
    }
    if (!u->job->done.load(std::memory_order_acquire)) return;
    KillTimer(hwnd, id);
    auto job = std::move(u->job);
    ProbeDone(*s, *u, std::move(job->result));
}

} // namespace

void AiSettingsEditsLayout(AppState& s) {
    auto& slot = Slot(s);
    if (!slot || !s.hwnd) return;
    AiSettingsEdits& u = *slot;
    D2D1_RECT_F key{}, base{};
    if (!s.renderer.AiModelFieldRects(key, base)) {
        HideEdit(s, u.key_edit, u.key_open);
        HideEdit(s, u.base_edit, u.base_open);
        return;
    }
    EnsureFont(s, u);
    if (u.key_open && !u.key_edit) u.key_edit = MakeEdit(s, true, KeyCue());
    if (u.base_open && !u.base_edit) u.base_edit = MakeEdit(s, false, L"https://…");
    if (u.key_open) Place(s, u, u.key_edit, u.key_open, key, u.key_focus_pending);
    else HideEdit(s, u.key_edit, u.key_open);
    if (u.base_open) Place(s, u, u.base_edit, u.base_open, base, u.base_focus_pending);
    else HideEdit(s, u.base_edit, u.base_open);
}

void AiSettingsBeginKeyEdit(AppState& s) {
    AiSettingsEdits& u = *Ui(s);
    if (!u.key_edit) u.key_edit = MakeEdit(s, true, KeyCue());
    if (!u.key_edit) return;
    // Never load the stored secret into the EDIT: the page already shows the
    // mask, and opening the field must not turn a paste into an append.
    SetWindowTextW(u.key_edit, L"");
    SendMessageW(u.key_edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(KeyCue()));
    u.base_open = false;
    u.key_open = true;
    u.key_revealed = false;
    SendMessageW(u.key_edit, EM_SETPASSWORDCHAR, kMaskChar, 0);
    u.key_focus_pending = true;
    Redraw(s);
}

void AiSettingsBeginBaseEdit(AppState& s) {
    AiSettingsEdits& u = *Ui(s);
    if (!u.base_edit) u.base_edit = MakeEdit(s, false, L"https://…");
    if (!u.base_edit) return;
    const std::wstring url = ServiceBase();
    SetWindowTextW(u.base_edit, url.c_str());
    u.key_open = false;
    u.base_open = true;
    u.base_focus_pending = true;
    Redraw(s);
}

void AiSettingsToggleReveal(AppState& s) {
    AiSettingsEdits* u = Slot(s).get();
    if (!u) return;
    u->key_revealed = !u->key_revealed;
    if (u->key_edit) {
        SendMessageW(u->key_edit, EM_SETPASSWORDCHAR, u->key_revealed ? 0 : kMaskChar, 0);
        InvalidateRect(u->key_edit, nullptr, TRUE);
    }
    Redraw(s);
}

void AiSettingsProbeConnect(AppState& s) {
    AiSettingsEdits& u = *Ui(s);
    if (u.probing || !s.hwnd) return;
    std::wstring key;
    if (!app::ai::LoadModelKey(key) || key.empty()) {
        Wipe(key);
        return;
    }
    app::ai::ModelEndpoint endpoint;
    endpoint.base_url = ServiceBase();
    endpoint.key = std::move(key);
    u.probe_url = endpoint.base_url;
    u.probing = true;
    u.probed = false;
    auto job = std::make_shared<AiSettingsEdits::Job>();
    job->gen = ++u.gen;
    u.job = job;
    std::thread([job, endpoint = std::move(endpoint)]() mutable {
        job->result = app::ai::ProbeModelService(std::move(endpoint));
        job->done.store(true, std::memory_order_release);
    }).detach();
    SetTimer(s.hwnd, kProbeTimer, 50, ProbeTimer);
    Redraw(s);
}

void AiSettingsEditsDestroy(AppState& s) {
    AiSettingsEdits* u = Slot(s).get();
    if (!u) return;
    ++u->gen;
    u->job.reset();
    if (s.hwnd) KillTimer(s.hwnd, kProbeTimer);
    if (u->key_edit) {
        SetWindowTextW(u->key_edit, L"");
        DestroyWindow(u->key_edit);
    }
    if (u->base_edit) DestroyWindow(u->base_edit);
    u->key_edit = u->base_edit = nullptr;
    if (u->font) DeleteObject(u->font);
    u->font = nullptr;
}

} // namespace luling
