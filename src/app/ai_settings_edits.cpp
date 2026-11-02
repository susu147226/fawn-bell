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
#include "../ui/typography.h"
#include "../ui/ui_renderer.h"

#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>

namespace luling {
namespace {

constexpr UINT_PTR kEditSubclass = 0x415A;
constexpr UINT_PTR kProbeTimer = 0x4158;
constexpr wchar_t kMaskChar = L'\x2022';

std::shared_ptr<AiSettingsEdits>& Slot(AppState& s) { return s.aiAssistant.settings_edits; }
AiSettingsEdits* Ui(AppState& s) {
    auto& slot = Slot(s);
    if (!slot) slot = std::make_shared<AiSettingsEdits>();
    return slot.get();
}
void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

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
    if (!placed || !shown) {
        SetWindowPos(edit, HWND_TOP, x, y, w, line, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        shown = true;
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

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    AppState& s = *reinterpret_cast<AppState*>(ref);
    AiSettingsEdits* u = Slot(s).get();
    if (!u) return DefSubclassProc(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
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
        if (wp == L'\r' || wp == L'\n' || wp == 27) return 0;
        break;
    case WM_KILLFOCUS: {
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
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
    return DefSubclassProc(hwnd, msg, wp, lp);
}

HWND MakeEdit(AppState& s, bool password, const wchar_t* cue) {
    const DWORD style =
        WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0);
    HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 0, 0, s.hwnd, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
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
    if (u.key_open && !u.key_edit) u.key_edit = MakeEdit(s, true, L"粘贴 API Key");
    if (u.base_open && !u.base_edit) u.base_edit = MakeEdit(s, false, L"https://…");
    if (u.key_open) Place(s, u, u.key_edit, u.key_open, key, u.key_focus_pending);
    else HideEdit(s, u.key_edit, u.key_open);
    if (u.base_open) Place(s, u, u.base_edit, u.base_open, base, u.base_focus_pending);
    else HideEdit(s, u.base_edit, u.base_open);
}

void AiSettingsBeginKeyEdit(AppState& s) {
    AiSettingsEdits& u = *Ui(s);
    if (!u.key_edit) u.key_edit = MakeEdit(s, true, L"粘贴 API Key");
    if (!u.key_edit) return;
    std::wstring key;
    const bool has = app::ai::LoadModelKey(key) && !key.empty();
    SetWindowTextW(u.key_edit, has ? key.c_str() : L"");
    Wipe(key);
    u.base_open = false;
    u.key_open = true;
    SendMessageW(u.key_edit, EM_SETPASSWORDCHAR, u.key_revealed ? 0 : kMaskChar, 0);
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
