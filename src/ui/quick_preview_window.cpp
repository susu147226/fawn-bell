#include "edit_host.h"
#include "FluentTokens.h"
#include "legacy_icons.h"
#include "../common/windows_compat.h"
#include "../common/preview_extensions.h"
#include "quick_preview_window.h"
#include "preview_notice.h"
#include "../app/shell_verbs.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include "../ipc/preview_protocol.h"
#include "../ops/clipboard.h"
#include "fluent_components.h"
#include "lumatext_renderer.h"
#include "syntax_highlight.h"
#include "typography.h"
#include "ui_motion.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <commctrl.h>
#include <d2d1helper.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shlwapi.lib")

namespace luling::ui {
namespace {

constexpr wchar_t kWindowClass[] = L"Luling.QuickPreview";
constexpr DWORD kRecallOnOpen = 0x00040000;
constexpr DWORD kRecallOnData = 0x00400000;
constexpr DWORD kPinned = 0x00080000;
constexpr float kCloseButtonWidth = 46.0f;
constexpr float kChromeButtonWidth = 40.0f;   // prev / next / more, left of close
constexpr float kCopyButtonWidth = 64.0f;     // "Copy" / "Copied" chip, left of prev
constexpr UINT_PTR kCopyFeedbackTimer = 74;
constexpr float kCloseGlyphSize = 16.0f * 0.66f;
constexpr float kFindBarHeight = 44.0f;
// Rollback switch: false shows Markdown as highlighted source only (no host
// rich-text request, no Rendered | Source pill).
constexpr bool kMarkdownRender = true;
// Rollback switch: false shows CSV as text and XLSX through the system preview
// handler instead of the table grid.
constexpr bool kTableView = true;
// Rollback switch: false shows JSON/XML as highlighted source (no tree view).
constexpr bool kTreeView = true;
// Rollback switch: false leaves DOCX to the system preview handler and EPUB
// unpreviewed (no host document conversion).
constexpr bool kDocView = true;
// Rollback switch: false keeps animated WebP / APNG still, .ico/.cur without
// size pills and HEIF / AVIF without the codec card.
constexpr bool kImageExtras = true;
constexpr UINT_PTR kTableTipTimer = 73;
constexpr float kHudHeight = 28.0f;
constexpr float kFooterHeight = 30.0f;
constexpr float kInfoDrawerWidth = 250.0f;
constexpr UINT_PTR kAnimationTimer = 7;
constexpr UINT_PTR kPreviewDeadlineTimer = 31;
constexpr UINT_PTR kFindEditCaretTimer = 71;
constexpr UINT_PTR kZoomCloseTimer = 72;
// The open-with chooser opens from a posted message: the popup's modal loop
// must not start inside WM_LBUTTONDOWN, or the matching button-up counts as a
// click outside and dismisses it at once.
constexpr UINT kOpenWithMenuMessage = WM_APP + 40;

// Shell association facts are per type, but RefreshItemFacts used to query the
// registry for every shown item; caching them per extension keeps stepping
// through a folder instant.
struct ItemFactsCache {
    std::wstring type_name;
    std::wstring app;
    std::wstring open_label;
};
std::mutex g_facts_mutex;
std::unordered_map<std::wstring, ItemFactsCache> g_facts_cache;
constexpr double kZoomOpenSeconds = 0.22;
constexpr double kZoomCloseSeconds = 0.15;
constexpr float kMaxDecodedZoom = 4.0f;
constexpr size_t kFindQueryLimit = 256;
constexpr wchar_t kSearchGlyph[] = L"\xE721";
constexpr wchar_t kCopyGlyph[] = L"\xE8C8";
constexpr wchar_t kSelectAllGlyph[] = L"\xE8B3";
constexpr wchar_t kPrevGlyph[] = L"\xE72B";
constexpr wchar_t kNextGlyph[] = L"\xE72A";
constexpr wchar_t kMoreGlyph[] = L"\xE712";
constexpr wchar_t kOpenGlyph[] = L"\xE8E5";
constexpr wchar_t kCutGlyph[] = L"\xE8C6";
constexpr wchar_t kLinkGlyph[] = L"\xE71B";
constexpr wchar_t kStarGlyph[] = L"\xE734";
constexpr wchar_t kStarFilledGlyph[] = L"\xE735";
constexpr wchar_t kRenameGlyph[] = L"\xE8AC";
constexpr wchar_t kDeleteGlyph[] = L"\xE74D";
constexpr wchar_t kPropertiesGlyph[] = L"\xE946";

enum {
    kTextCmdCopy = 1,
    kTextCmdSelectAll,
    kTextCmdFind,
    kTextCmdLineNumbers,
    kImageCmdRotate,
    kImageCmdInfo,
    kArchiveCmdCopyPath,
    kFileCmdBase = 100,  // + static_cast<int>(QuickPreviewAction)
};

std::wstring ExtensionLabel(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towupper(c));
    return ext;
}

// 1: CSV family drawn as a grid; 2: workbook (grid or system preview handler).
int TableFileKind(const std::wstring& path) {
    const std::wstring ext = ExtensionLabel(path);
    if (ext == L"CSV" || ext == L"TSV" || ext == L"TAB" || ext == L"PSV") return 1;
    if (ext == L"XLSX" || ext == L"XLSM") return 2;
    return 0;
}

bool IsIconFile(const std::wstring& path) {
    const std::wstring ext = ExtensionLabel(path);
    return ext == L"ICO" || ext == L"CUR";
}

// Word documents the host converts to the block payload ("Document | System").
bool IsDocxFile(const std::wstring& path) {
    const std::wstring ext = ExtensionLabel(path);
    return ext == L"DOCX" || ext == L"DOCM" || ext == L"DOTX";
}

// Pictures the footer's zoom / backdrop / info / rotate chips belong to.
bool IsImageItemPath(const std::wstring& path) {
    std::wstring ext = ExtensionLabel(path);
    if (ext.empty()) return false;
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    const std::wstring dotted = L"." + ext;
    return preview::IsImageExtension(dotted) ||
           preview::IsFamilyExtension(preview::PreviewFamily::Psd, dotted) ||
           preview::IsFamilyExtension(preview::PreviewFamily::Raw, dotted);
}

// "PotPlayer (64-bit)" -> "PotPlayer": an edition note only crowds the button.
std::wstring ShortAppName(std::wstring name) {
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    if (!name.empty() && (name.back() == L')' || name.back() == L'\xFF09')) {
        const size_t open = name.find_last_of(L"(\xFF08");
        if (open != std::wstring::npos && open > 0) {
            std::wstring head = name.substr(0, open);
            while (!head.empty() && iswspace(head.back())) head.pop_back();
            if (!head.empty()) name = std::move(head);
        }
    }
    return name;
}

// Apps registered under HKCR\<ext>\OpenWithProgids that have a launch command:
// the chooser lists them beyond the Explorer MRU.
struct CandidateApp {
    std::wstring label;
    std::wstring exe;
};

bool EndsWithNoCase(const std::wstring& text, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return text.size() >= n && _wcsicmp(text.c_str() + text.size() - n, suffix) == 0;
}

std::vector<CandidateApp> ProgidOpenWithApps(const std::wstring& dotted_ext) {
    std::vector<CandidateApp> apps;
    HKEY key = nullptr;
    const std::wstring key_path = dotted_ext + L"\\OpenWithProgids";
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key_path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return apps;
    wchar_t name[256]{};
    for (DWORD index = 0; index < 64 && apps.size() < 4; ++index) {
        DWORD length = ARRAYSIZE(name);
        if (RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        const std::wstring progid(name, length);
        if (progid.empty()) continue;
        HKEY command = nullptr;
        const std::wstring command_path = progid + L"\\shell\\open\\command";
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, command_path.c_str(), 0, KEY_READ, &command) != ERROR_SUCCESS)
            continue;
        wchar_t line[2048]{};
        DWORD bytes = sizeof(line) - sizeof(wchar_t);
        const LONG read = RegQueryValueExW(command, nullptr, nullptr, nullptr,
                                           reinterpret_cast<BYTE*>(line), &bytes);
        RegCloseKey(command);
        if (read != ERROR_SUCCESS || !line[0]) continue;
        std::wstring exe;
        const std::wstring command_line = line;
        if (command_line.front() == L'"') {
            const size_t close = command_line.find(L'"', 1);
            if (close != std::wstring::npos) exe = command_line.substr(1, close - 1);
        } else {
            const size_t space = command_line.find(L' ');
            exe = space == std::wstring::npos ? command_line : command_line.substr(0, space);
        }
        if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (EndsWithNoCase(exe, L"\\rundll32.exe") || EndsWithNoCase(exe, L"\\OpenWith.exe") ||
            EndsWithNoCase(exe, L"\\explorer.exe"))
            continue;
        bool duplicate = false;
        for (const CandidateApp& existing : apps)
            if (_wcsicmp(existing.exe.c_str(), exe.c_str()) == 0) { duplicate = true; break; }
        if (duplicate) continue;
        std::wstring label = app::ProgramFriendlyName(exe);
        if (label.empty()) {
            const size_t slash = exe.find_last_of(L"\\/");
            label = slash == std::wstring::npos ? exe : exe.substr(slash + 1);
            if (EndsWithNoCase(label, L".exe")) label.resize(label.size() - 4);
        }
        apps.push_back({ShortAppName(std::move(label)), std::move(exe)});
    }
    RegCloseKey(key);
    return apps;
}

std::wstring FormatShortTime(uint64_t modified) {
    FILETIME utc{};
    utc.dwLowDateTime = static_cast<DWORD>(modified);
    utc.dwHighDateTime = static_cast<DWORD>(modified >> 32);
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
        return {};
    wchar_t when[64]{};
    swprintf_s(when, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute);
    return when;
}

// 12,480: digits grouped for the status pill.
std::wstring GroupedNumber(uint32_t value) {
    std::wstring digits = std::to_wstring(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) digits.insert(static_cast<size_t>(at), 1, L',');
    return digits;
}

// Themed brush color; high contrast is applied where the colors are used.
COLORREF FindEditBrushColor(bool dark) noexcept {
    return dark ? RGB(30, 30, 30) : RGB(255, 255, 255);
}

D2D1_COLOR_F FindEditBg(bool dark) noexcept { return ColorFromRef(EditBackColor(dark)); }
D2D1_COLOR_F FindEditFg(bool dark) noexcept { return ColorFromRef(EditTextColor(dark)); }

void FillHitRange(ID2D1DeviceContext* dc, IDWriteTextLayout* layout,
                  uint32_t start, uint32_t length, float origin_x, float origin_y,
                  ID2D1SolidColorBrush* brush) {
    if (!dc || !layout || !brush || length == 0) return;
    DWRITE_HIT_TEST_METRICS metrics[96];
    UINT32 actual = 0;
    if (FAILED(layout->HitTestTextRange(start, length, origin_x, origin_y,
                                        metrics, ARRAYSIZE(metrics), &actual))) return;
    const UINT32 count = (std::min)(actual, static_cast<UINT32>(ARRAYSIZE(metrics)));
    for (UINT32 i = 0; i < count; ++i) {
        dc->FillRectangle(D2D1::RectF(metrics[i].left, metrics[i].top,
                                      metrics[i].left + metrics[i].width,
                                      metrics[i].top + metrics[i].height), brush);
    }
}

} // namespace

QuickPreviewWindow::~QuickPreviewWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool QuickPreviewWindow::Initialize(HWND owner, UINT navigate_message, UINT open_message,
                                    UINT command_message) {
    if (hwnd_) return true;
    owner_ = owner;
    navigate_message_ = navigate_message;
    open_message_ = open_message;
    command_message_ = command_message;
    thumbnails_.SetQuickLookContent(kMarkdownRender || kTableView || kTreeView || kDocView);
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = WndProc;
    window_class.lpszClassName = kWindowClass;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = static_cast<HICON>(LoadImageW(window_class.hInstance,
        MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    RegisterClassExW(&window_class);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        kWindowClass, luling::l10n::Get(luling::l10n::StringId::QuickPreview).c_str(),
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 680, owner, nullptr, window_class.hInstance, this);
    return hwnd_ != nullptr;
}

void QuickPreviewWindow::ResetTextState() {
    preview_text_.clear();
    text_layout_.reset();
    text_layout_width_ = 0.0f;
    text_layout_scale_ = 0.0f;
    text_layout_hex_ = false;
    sel_anchor_ = sel_focus_ = 0;
    selecting_ = false;
    copy_feedback_ = false;
    image_turns_ = 0;
    image_info_ = false;
    pointer_tick_ = GetTickCount64();
    CloseFind();
}

void QuickPreviewWindow::ResetView() {
    if (hwnd_) KillTimer(hwnd_, kPreviewDeadlineTimer);
    preview_deadline_ = 0;
    thumbnails_.TakeRetryDeadline();
    sheet_request_ = 0;
    preview_notice_.clear();
    preview_notice_height_ = 0;
    ResetPlayback();
    text_scroll_ = 0.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    video_pan_x_ = 0.0f;
    video_pan_y_ = 0.0f;
    video_panning_ = false;
    image_fit_ = true;
    image_zoom_ = 1.0f;
    decoded_w_ = decoded_h_ = 0;
    source_w_ = source_h_ = 0;
    native_kind_ = NativeKind::None;
    archive_.Clear();
    markdown_.Clear();
    table_.Clear();
    tree_.Clear();
    panning_ = false;
    waveform_.Reset();
    audio_wave_rect_ = {};
    ResetPages();
    ResetTextState();
}

void QuickPreviewWindow::ResetAnimation() {
    CancelPlaybackScrub();
    if (hwnd_) KillTimer(hwnd_, kAnimationTimer);
    frame_index_ = 0; requested_frame_ = 0; frame_count_ = 1; frame_delay_ms_ = 0;
    loop_count_ = 0; completed_loops_ = 0;
    animation_active_ = false; waiting_for_frame_ = false; animation_started_ = false;
}

void QuickPreviewWindow::RecreateFormats() {
    close_format_.reset();
    preview_text_format_.reset();
    text_layout_.reset();
    if (!compositor_.DwriteFactory()) return;
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Icon, kCloseGlyphSize * scale_}, &close_format_);
    if (close_format_.get()) {
        close_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        close_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Monospace, 13.0f * scale_}, &preview_text_format_);
    if (preview_text_format_.get()) {
        preview_text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        preview_text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        preview_text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    gutter_format_.reset();
    typography::CreateTextFormat(compositor_.DwriteFactory(),
        {typography::FontRole::Monospace, 13.0f * scale_}, &gutter_format_);
    if (gutter_format_.get()) {
        gutter_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        gutter_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        gutter_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    find_painter_.SetCompositor(&compositor_);
    find_painter_.SetScale(scale_);
    if (find_edit_font_) {
        DeleteObject(find_edit_font_);
        find_edit_font_ = nullptr;
    }
    find_edit_font_ = typography::CreateEditFont(scale_);
    if (find_edit_ && find_edit_font_)
        SendMessageW(find_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(find_edit_font_), TRUE);
    if (find_edit_brush_) {
        DeleteObject(find_edit_brush_);
        find_edit_brush_ = nullptr;
    }
}

void QuickPreviewWindow::Show(const QuickPreviewItem& item, bool dark, WindowEffect effect,
                              bool safe_mode, const POINT* zoom_from) {
    if (!hwnd_ || item.path.empty()) return;
    if (closing_) {
        // Re-opened while the close zoom is still running.
        KillTimer(hwnd_, kZoomCloseTimer);
        closing_ = false;
        compositor_.ClearZoom();
    }
    item_ = item;
    dark_ = dark;
    commands_.Clear();
    effect_ = effect;
    safe_mode_ = safe_mode;
    handler_immediate_ = true;
    ApplyWindowEffect(hwnd_, effect_, dark_);
    if (find_edit_brush_) {
        DeleteObject(find_edit_brush_);
        find_edit_brush_ = nullptr;
    }
    ++generation_;
    ResetView();
    ResetAnimation();
    icon_request_ = 0; icon_sizes_.clear(); icon_selected_ = -1; icon_pill_rects_.clear();
    BeginVideo();
    RefreshItemFacts();
    RECT owner_rect{};
    GetWindowRect(owner_, &owner_rect);
    const int owner_width = owner_rect.right - owner_rect.left;
    const int owner_height = owner_rect.bottom - owner_rect.top;
    const int width = std::clamp(static_cast<int>(owner_width * 0.82), 640, 1280);
    const int height = std::clamp(static_cast<int>(owner_height * 0.82), 480, 900);
    const int x = owner_rect.left + (owner_width - width) / 2;
    const int y = owner_rect.top + (owner_height - height) / 2;
    SetWindowTextW(hwnd_, item_.name.empty()
        ? luling::l10n::Get(luling::l10n::StringId::QuickPreview).c_str() : item_.name.c_str());
    // Zoom out of the item's icon. Only with a plain (None) window effect:
    // a DWM system backdrop would pop in at full size around the zooming
    // content. The DWM show transition is disabled while our zoom runs.
    // The icon zoom read as two layers (DWM frame + zooming content); the
    // user preferred the plain system show/hide animation, so it stays off.
    (void)zoom_from;
    zoom_enabled_ = false;
    const BOOL no_dwm_transition = zoom_enabled_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &no_dwm_transition,
                          sizeof(no_dwm_transition));
    zoom_pending_ = zoom_enabled_ && compositor_.HideContent();
    if (zoom_pending_) zoom_origin_ = POINT{zoom_from->x - x, zoom_from->y - y};
    else compositor_.ClearZoom();
    // Size and draw the first frame while still hidden so the system show
    // animation starts from real content instead of an empty (black) window.
    const bool was_visible = IsWindowVisible(hwnd_) != FALSE;
    SetWindowPos(hwnd_, HWND_TOP, x, y, width, height,
                 was_visible ? 0u : static_cast<UINT>(SWP_NOACTIVATE));
    if (!was_visible) Render();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::Update(const QuickPreviewItem& item) {
    if (!visible() || item.path.empty()) return;
    commands_.Clear();
    item_ = item;
    ++generation_;
    handler_immediate_ = false;
    ResetView();
    ResetAnimation();
    icon_request_ = 0; icon_sizes_.clear(); icon_selected_ = -1; icon_pill_rects_.clear();
    handler_.Reset();
    BeginVideo();
    RefreshItemFacts();
    SetWindowTextW(hwnd_, item_.name.empty()
        ? luling::l10n::Get(luling::l10n::StringId::QuickPreview).c_str() : item_.name.c_str());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::SetStarred(bool starred) {
    if (item_.starred == starred) return;
    item_.starred = starred;
}

void QuickPreviewWindow::Close() {
    if (!hwnd_ || closing_) return;
    commands_.Clear();
    handler_.Hide();
    ResetAnimation();
    ResetView();
    thumbnails_.Evict();
    preview_pixels_ = 0;
    close_hover_ = false;
    chrome_hover_ = ChromeButton::None;
    zoom_pending_ = false;
    // Shrink back into the item, then hide. Focus returns to the owner right
    // away so keyboard input never waits for the animation.
    if (zoom_enabled_ && IsWindowVisible(hwnd_) &&
        compositor_.PlayZoom(static_cast<float>(zoom_origin_.x),
                             static_cast<float>(zoom_origin_.y), false, kZoomCloseSeconds)) {
        closing_ = true;
        SetTimer(hwnd_, kZoomCloseTimer,
                 static_cast<UINT>(kZoomCloseSeconds * 1000.0) + 20, nullptr);
        if (owner_) SetForegroundWindow(owner_);
        return;
    }
    FinishClose(true);
}

// restore_focus is false after the close zoom: focus already went back to the
// owner when the close started, and the user may have switched apps since.
void QuickPreviewWindow::FinishClose(bool restore_focus) {
    closing_ = false;
    if (hwnd_) {
        KillTimer(hwnd_, kZoomCloseTimer);
        ShowWindow(hwnd_, SW_HIDE);
    }
    compositor_.ClearZoom();
    if (restore_focus && owner_) SetForegroundWindow(owner_);
}

bool QuickPreviewWindow::visible() const noexcept {
    return hwnd_ && !closing_ && IsWindowVisible(hwnd_) != FALSE;
}

bool QuickPreviewWindow::OfflinePlaceholder() const noexcept {
    return (item_.attrs & (kRecallOnOpen | kRecallOnData)) && !(item_.attrs & kPinned);
}

void QuickPreviewWindow::Resize() {
    if (!hwnd_ || !compositor_.Dc()) return;
    RECT rect{};
    GetClientRect(hwnd_, &rect);
    compositor_.Resize((std::max)(1L, rect.right), (std::max)(1L, rect.bottom));
}

float QuickPreviewWindow::FindBarHeight() const noexcept {
    return find_open_ && (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                          native_kind_ == NativeKind::Archive || native_kind_ == NativeKind::Markdown ||
                          native_kind_ == NativeKind::Table || native_kind_ == NativeKind::Tree)
        ? kFindBarHeight * scale_ : 0.0f;
}

D2D1_RECT_F QuickPreviewWindow::FindBarRect() const {
    const float width = static_cast<float>(compositor_.Width());
    const float header = kTitleBarHeight * scale_;
    return D2D1::RectF(0, header, width, header + kFindBarHeight * scale_);
}

D2D1_RECT_F QuickPreviewWindow::FindFieldRect() const {
    const D2D1_RECT_F bar = FindBarRect();
    const float pad_x = 12.0f * scale_;
    const float pad_y = 6.0f * scale_;
    return D2D1::RectF(bar.left + pad_x, bar.top + pad_y, bar.right - pad_x, bar.bottom - pad_y);
}

D2D1_RECT_F QuickPreviewWindow::FindEditCell() const {
    const D2D1_RECT_F field = FindFieldRect();
    const float inset = 10.0f * scale_;
    const float icon = 18.0f * scale_;
    const float gap = 6.0f * scale_;
    const float trail = find_query_.empty() ? inset : 72.0f * scale_;
    return D2D1::RectF(field.left + inset + icon + gap, field.top,
                       field.right - trail, field.bottom);
}

D2D1_RECT_F QuickPreviewWindow::ContentRect() const {
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float header = kTitleBarHeight * scale_;
    float right = width;
    if (image_info_ && native_kind_ == NativeKind::Bitmap) right -= InfoDrawerWidth();
    float bottom = height - PlaybackHeight();
    if (ShowsImageFooter()) bottom -= FooterHeight();
    return D2D1::RectF(0, header + FindBarHeight() + preview_notice_height_, right, bottom);
}

bool QuickPreviewWindow::ShowsImageFooter() const noexcept {
    if (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) return false;
    return IsImageItemPath(item_.path) || native_kind_ == NativeKind::Bitmap;
}

float QuickPreviewWindow::FooterHeight() const noexcept {
    return ShowsImageFooter() ? kFooterHeight * scale_ : 0.0f;
}

D2D1_RECT_F QuickPreviewWindow::FooterRect() const {
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float bottom = height - PlaybackHeight();
    const float top = bottom - FooterHeight();
    return D2D1::RectF(0, top, width, bottom);
}

float QuickPreviewWindow::InfoDrawerWidth() const noexcept {
    // Only beside a single picture, and only when the picture keeps room.
    if (!image_info_ || native_kind_ != NativeKind::Bitmap || pages_count_ > 1) return 0.0f;
    const float width = kInfoDrawerWidth * scale_;
    return static_cast<float>(compositor_.Width()) >= width + 320.0f * scale_ ? std::round(width) : 0.0f;
}

float QuickPreviewWindow::TextWidthDip(const std::wstring& text) const {
    IDWriteFactory* dw = compositor_.DwriteFactory();
    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (!dw || !format || text.empty()) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
                                    1000.0f * scale_, 40.0f * scale_, &layout)) ||
        !layout.get())
        return static_cast<float>(text.size()) * 7.0f * scale_;
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return static_cast<float>(text.size()) * 7.0f * scale_;
    return metrics.widthIncludingTrailingWhitespace;
}

float QuickPreviewWindow::OpenButtonWidth() const {
    if (open_label_.empty()) return 0.0f;
    // Measured each frame so a DPI change or a new item never leaves a stale width.
    // Room for the app icon, the label, the chevron and 8 DIP of padding each side.
    const float text = TextWidthDip(open_label_);
    return std::clamp(text + 56.0f * scale_, 96.0f * scale_, 220.0f * scale_);
}

// Header right-to-left: × | ⋯ | next | prev | toggle | counter | open-with.
D2D1_RECT_F QuickPreviewWindow::CounterRect() const {
    if (item_.position <= 0 || item_.count <= 0) return D2D1::RectF(0, 0, 0, 0);
    // The tree/source toggle occupies the slot immediately left of the arrows;
    // the counter has to sit left of it, not on top of it.
    const float right = markdown_shown_
        ? MarkdownToggleRect(-1).left - 6.0f * scale_
        : ChromeButtonRect(ChromeButton::Prev).left - 6.0f * scale_;
    const float width = 58.0f * scale_;
    return D2D1::RectF(right - width, 0, right, kTitleBarHeight * scale_);
}

D2D1_RECT_F QuickPreviewWindow::OpenButtonRect() const {
    const float width = OpenButtonWidth();
    if (width <= 0.0f) return D2D1::RectF(0, 0, 0, 0);
    const D2D1_RECT_F counter = CounterRect();
    float right = 0.0f;
    if (counter.right > counter.left) right = counter.left - 6.0f * scale_;
    else if (markdown_shown_) right = MarkdownToggleRect(-1).left - 6.0f * scale_;
    else right = ChromeButtonRect(ChromeButton::Prev).left - 6.0f * scale_;
    const float inset = 6.0f * scale_;
    return D2D1::RectF(right - width, inset, right, kTitleBarHeight * scale_ - inset);
}

void QuickPreviewWindow::RefreshItemFacts() {
    type_name_.clear();
    open_label_.clear();
    open_label_width_ = 0.0f;
    const bool folder = (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring ext = folder ? std::wstring() : ExtensionLabel(item_.path);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    const std::wstring key = folder ? std::wstring(L":folder") : ext;
    ItemFactsCache facts;
    bool cached = false;
    {
        std::lock_guard lock(g_facts_mutex);
        const auto found = g_facts_cache.find(key);
        if (found != g_facts_cache.end()) {
            facts = found->second;
            cached = true;
        }
    }
    if (!cached) {
        const std::wstring probe = folder ? std::wstring(L"folder") : L"file." + ext;
        SHFILEINFOW info{};
        if (SHGetFileInfoW(probe.c_str(), folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                           &info, sizeof(info), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
            facts.type_name = info.szTypeName;
        if (facts.type_name.empty()) {
            std::wstring upper = ext;
            for (wchar_t& c : upper) c = static_cast<wchar_t>(std::towupper(c));
            facts.type_name = folder ? luling::l10n::Pick(L"\x6587\x4EF6\x5939", L"Folder")
                : upper.empty() ? luling::l10n::Pick(L"\x6587\x4EF6", L"File")
                                : upper + luling::l10n::Pick(L" \x6587\x4EF6", L" file");
        }
        if (!folder && !ext.empty()) {
            const std::wstring dotted = L"." + ext;
            wchar_t name[256]{};
            DWORD length = ARRAYSIZE(name);
            std::wstring app;
            if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME,
                                            dotted.c_str(), nullptr, name, &length)))
                app = ShortAppName(name);
            if (!app.empty()) {
                facts.open_label = luling::l10n::Get(luling::l10n::StringId::ShellOpenApp);
                if (const size_t at = facts.open_label.find(L"{app}"); at != std::wstring::npos)
                    facts.open_label.replace(at, 5, app);
            } else {
                facts.open_label = luling::l10n::Get(luling::l10n::StringId::ShellOpenWith);
            }
            facts.app = std::move(app);
        }
        std::lock_guard lock(g_facts_mutex);
        if (g_facts_cache.size() >= 512) g_facts_cache.clear();
        g_facts_cache.emplace(key, facts);
    }
    type_name_ = facts.type_name;
    open_label_ = folder ? std::wstring() : facts.open_label;
    if (!open_label_.empty()) open_label_width_ = TextWidthDip(open_label_);
}

std::wstring QuickPreviewWindow::OpenLabel() const {
    return open_label_;
}

std::wstring QuickPreviewWindow::MetaLine() const {
    std::vector<std::wstring> parts;
    if (!type_name_.empty()) parts.push_back(type_name_);
    if (native_kind_ == NativeKind::Bitmap && decoded_w_ && decoded_h_) {
        wchar_t dims[64]{};
        swprintf_s(dims, L"%u \x00D7 %u", source_w_ ? source_w_ : decoded_w_,
                   source_h_ ? source_h_ : decoded_h_);
        parts.push_back(dims);
    }
    if (const std::wstring size = luling::format::ByteSize(item_.size, true); !size.empty())
        parts.push_back(size);
    if (item_.modified) {
        const std::wstring when = FormatShortTime(item_.modified);
        if (!when.empty()) parts.push_back(when);
    }
    std::wstring line;
    for (const std::wstring& part : parts) {
        if (!line.empty()) line += L"  \x00B7  ";
        line += part;
    }
    return line;
}

D2D1_RECT_F QuickPreviewWindow::ChromeButtonRect(ChromeButton button) const {
    const float width = static_cast<float>(compositor_.Width());
    const float header = kTitleBarHeight * scale_;
    const float slot = kChromeButtonWidth * scale_;
    const float right = width - kCloseButtonWidth * scale_;
    // The copy chip is wider than an icon slot. Everything else has to be
    // placed from its real left edge, or the next arrow lands inside it.
    const float copy_w = ShowsCopyButton() ? kCopyButtonWidth * scale_ : 0.0f;
    const float copy_left = right - copy_w;
    switch (button) {
    case ChromeButton::Copy:
        return copy_w > 0.0f ? D2D1::RectF(copy_left, 0, right, header)
                             : D2D1::RectF(0, 0, 0, 0);
    case ChromeButton::More:
        return D2D1::RectF(copy_left - slot, 0, copy_left, header);
    case ChromeButton::Next:
        return D2D1::RectF(copy_left - slot * 2.0f, 0, copy_left - slot, header);
    case ChromeButton::Prev:
        return D2D1::RectF(copy_left - slot * 3.0f, 0, copy_left - slot * 2.0f, header);
    default:
        return D2D1::RectF(0, 0, 0, 0);
    }
}

D2D1_RECT_F QuickPreviewWindow::MarkdownToggleRect(int segment) const {
    const bool zh = luling::l10n::IsChinese();
    const float inner = 2.0f * scale_;
    float w0 = zh ? 46.0f : 52.0f;  // "表格" / "Table"
    if (toggle_kind_ == ToggleKind::Markdown) w0 = zh ? 46.0f : 68.0f;
    else if (toggle_kind_ == ToggleKind::TreeSource) w0 = zh ? 40.0f : 48.0f;
    else if (toggle_kind_ == ToggleKind::NotebookSource) w0 = zh ? 58.0f : 72.0f;
    else if (toggle_kind_ == ToggleKind::DocHandler) w0 = zh ? 46.0f : 76.0f;
    w0 *= scale_;
    const bool handler_pill = toggle_kind_ == ToggleKind::TableHandler || toggle_kind_ == ToggleKind::DocHandler;
    const float w1 = (handler_pill ? (zh ? 70.0f : 64.0f)
                                                               : (zh ? 46.0f : 58.0f)) * scale_;
    const float height = 24.0f * scale_;
    const float top = (kTitleBarHeight * scale_ - height) * 0.5f;
    const float right = ChromeButtonRect(ChromeButton::Prev).left - 6.0f * scale_;
    const float left = right - w0 - w1 - inner * 2.0f;
    if (segment == 0)
        return D2D1::RectF(left + inner, top + inner, left + inner + w0, top + height - inner);
    if (segment == 1)
        return D2D1::RectF(left + inner + w0, top + inner, right - inner, top + height - inner);
    return D2D1::RectF(left, top, right, top + height);
}

bool& QuickPreviewWindow::ToggleSecond() noexcept {
    switch (toggle_kind_) {
    case ToggleKind::TableSource: return table_source_;
    case ToggleKind::TableHandler: return table_handler_;
    case ToggleKind::TreeSource: return tree_source_;
    case ToggleKind::NotebookSource: return notebook_source_;
    case ToggleKind::DocHandler: return doc_handler_;
    default: return markdown_source_;
    }
}

int QuickPreviewWindow::HitMarkdownToggle(POINT client) const {
    if (!markdown_shown_) return -1;
    const D2D1_RECT_F pill = MarkdownToggleRect(-1);
    const float x = static_cast<float>(client.x), y = static_cast<float>(client.y);
    if (x < pill.left || x >= pill.right || y < pill.top || y >= pill.bottom) return -1;
    return x < MarkdownToggleRect(0).right ? 0 : 1;
}

void QuickPreviewWindow::DrawMarkdownToggle(ID2D1DeviceContext* dc,
                                            ID2D1SolidColorBrush* text_brush) {
    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (!dc || !text_brush || !format) return;
    const wchar_t* labels[2] = {luling::l10n::Pick(L"\x6E32\x67D3", L"Rendered"), luling::l10n::Pick(L"\x6E90\x7801", L"Source")};
    if (toggle_kind_ == ToggleKind::TableSource || toggle_kind_ == ToggleKind::TableHandler)
        labels[0] = luling::l10n::Pick(L"\x8868\x683C", L"Table");
    else if (toggle_kind_ == ToggleKind::TreeSource)
        labels[0] = luling::l10n::Pick(L"\x6811", L"Tree");
    else if (toggle_kind_ == ToggleKind::NotebookSource)
        labels[0] = luling::l10n::Pick(L"\x7B14\x8BB0\x672C", L"Notebook");
    else if (toggle_kind_ == ToggleKind::DocHandler)
        labels[0] = luling::l10n::Pick(L"\x6B63\x6587", L"Document");
    if (toggle_kind_ == ToggleKind::TableHandler || toggle_kind_ == ToggleKind::DocHandler)
        labels[1] = luling::l10n::Pick(L"\x7CFB\x7EDF\x9884\x89C8", L"System");
    D2D1_COLOR_F color = text_brush->GetColor();
    ComPtr<ID2D1SolidColorBrush> track, thumb, dim;
    color.a = dark_ ? 0.08f : 0.06f;
    dc->CreateSolidColorBrush(color, &track);
    color.a = 0.62f;
    dc->CreateSolidColorBrush(color, &dim);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.14f) : D2D1::ColorF(0xFFFFFF, 1.0f),
                              &thumb);
    const D2D1_RECT_F pill = MarkdownToggleRect(-1);
    const float radius = (pill.bottom - pill.top) * 0.5f;
    if (track.get()) dc->FillRoundedRectangle(D2D1::RoundedRect(pill, radius, radius), track.get());
    const int active = ToggleSecond() ? 1 : 0;
    const D2D1_RECT_F knob = MarkdownToggleRect(active);
    const float knob_radius = (knob.bottom - knob.top) * 0.5f;
    if (thumb.get()) dc->FillRoundedRectangle(D2D1::RoundedRect(knob, knob_radius, knob_radius), thumb.get());
    if (!dark_ && track.get())
        dc->DrawRoundedRectangle(D2D1::RoundedRect(knob, knob_radius, knob_radius), track.get(), 1.0f);
    const auto text_alignment = format->GetTextAlignment();
    const auto paragraph_alignment = format->GetParagraphAlignment();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (int i = 0; i < 2; ++i) {
        ID2D1SolidColorBrush* brush = i == active || !dim.get() ? text_brush : dim.get();
        dc->DrawTextW(labels[i], static_cast<UINT32>(wcslen(labels[i])), format,
                      MarkdownToggleRect(i), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                      DWRITE_MEASURING_MODE_NATURAL);
    }
    format->SetTextAlignment(text_alignment);
    format->SetParagraphAlignment(paragraph_alignment);
}

QuickPreviewWindow::ChromeButton QuickPreviewWindow::HitChromeButton(POINT client) const {
    const float x = static_cast<float>(client.x);
    const float y = static_cast<float>(client.y);
    for (ChromeButton button : {ChromeButton::Copy, ChromeButton::Prev, ChromeButton::Next,
                                ChromeButton::More, ChromeButton::Open}) {
        if (button == ChromeButton::Copy && !ShowsCopyButton()) continue;
        const D2D1_RECT_F rect = button == ChromeButton::Open ? OpenButtonRect()
                                                              : ChromeButtonRect(button);
        if (rect.right <= rect.left) continue;
        if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) return button;
    }
    return ChromeButton::None;
}

void QuickPreviewWindow::ActivateChromeButton(ChromeButton button) {
    switch (button) {
    case ChromeButton::Copy:
        CopyAllText();
        break;
    case ChromeButton::Prev:
        if (owner_ && navigate_message_)
            PostMessageW(owner_, navigate_message_, static_cast<WPARAM>(-1), 0);
        break;
    case ChromeButton::Next:
        if (owner_ && navigate_message_) PostMessageW(owner_, navigate_message_, 1, 0);
        break;
    case ChromeButton::Open:
        // The label opens the default app; the chevron runs the chooser
        // (WM_LBUTTONDOWN/WM_LBUTTONUP decide which half was hit).
        PostAction(QuickPreviewAction::Open);
        break;
    case ChromeButton::More: {
        const D2D1_RECT_F rect = ChromeButtonRect(ChromeButton::More);
        POINT screen{static_cast<LONG>(rect.left), static_cast<LONG>(rect.bottom)};
        ClientToScreen(hwnd_, &screen);
        ShowContextMenu(screen);
        break;
    }
    default:
        break;
    }
}

void QuickPreviewWindow::ShowOpenWithMenu() {
    if (text_menu_.IsOpen()) return;
    if (!hwnd_ || item_.path.empty() || (item_.attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        PostAction(QuickPreviewAction::Open);
        return;
    }
    std::wstring ext = ExtensionLabel(item_.path);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    std::vector<app::StaticVerb> verbs;
    if (!ext.empty()) verbs = app::EnumerateStaticVerbs(L"." + ext);

    std::vector<FluentMenuItem> items;
    FluentMenuItem default_app;
    default_app.command = 1;
    default_app.text = open_label_.empty()
        ? luling::l10n::Get(luling::l10n::StringId::ShellOpenWith) : open_label_;
    default_app.glyph = kOpenGlyph;
    if (!ext.empty()) {
        auto query_icon = [&](ASSOCSTR kind, wchar_t* out, DWORD size) {
            DWORD length = size;
            return SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, kind,
                                               (L"." + ext).c_str(), nullptr, out, &length)) &&
                   out[0];
        };
        // The app-list icon is the program's own logo; DefaultIcon is often the
        // document picture Explorer shows for the type, which reads as the
        // wrong app in this menu.
        wchar_t reference[1024]{};
        if (query_icon(ASSOCSTR_APPICONREFERENCE, reference, ARRAYSIZE(reference)) ||
            query_icon(ASSOCSTR_DEFAULTICON, reference, ARRAYSIZE(reference))) {
            if (reference[0] == L'@') {
                // Store-app logo: the indirect string resolves to a PNG on disk.
                wchar_t logo[MAX_PATH * 2]{};
                if (SUCCEEDED(SHLoadIndirectString(reference, logo, ARRAYSIZE(logo), nullptr)))
                    default_app.icon = LoadAppIconPixels(
                        logo, static_cast<uint32_t>(16.0f * scale_));
            } else {
                std::wstring icon_file = reference;
                int icon_index = 0;
                if (const size_t comma = icon_file.find_last_of(L','); comma != std::wstring::npos) {
                    icon_index = _wtoi(icon_file.c_str() + comma + 1);
                    icon_file.resize(comma);
                }
                wchar_t clean[MAX_PATH * 2]{};
                wcsncpy_s(clean, icon_file.c_str(), _TRUNCATE);
                PathUnquoteSpacesW(clean);
                if (GetFileAttributesW(clean) != INVALID_FILE_ATTRIBUTES) {
                    default_app.icon_path = clean;
                    default_app.icon_index = icon_index;
                }
            }
        }
        // A generic document glyph would read as the wrong app: the row shows
        // the real icon or none at all.
        if (default_app.icon_path.empty() && !default_app.icon) default_app.glyph.clear();
    }
    items.push_back(std::move(default_app));

    std::vector<std::wstring> app_paths;
    for (const app::StaticVerb& verb : verbs) {
        if (verb.verb != L"__openwith" || verb.app_path.empty() || app_paths.size() >= 3) continue;
        FluentMenuItem row;
        row.command = 100 + static_cast<int>(app_paths.size());
        row.text = verb.display;
        std::wstring icon_file;
        int icon_index = 0;
        if (app::StaticVerbIcon(verb, icon_file, icon_index)) {
            row.icon_path = icon_file;
            row.icon_index = icon_index;
        }
        items.push_back(std::move(row));
        app_paths.push_back(verb.app_path);
    }
    // Programs registered for this type (HKCR\<ext>\OpenWithProgids) fill the
    // list further, so the button offers a real choice, not just the default app.
    if (!ext.empty()) {
        const std::wstring dotted = L"." + ext;
        wchar_t default_exe[MAX_PATH * 2]{};
        DWORD default_len = ARRAYSIZE(default_exe);
        const bool have_default = SUCCEEDED(AssocQueryStringW(
            ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE, dotted.c_str(), nullptr,
            default_exe, &default_len));
        for (const CandidateApp& candidate : ProgidOpenWithApps(dotted)) {
            if (app_paths.size() >= 4) break;
            bool duplicate = have_default && _wcsicmp(candidate.exe.c_str(), default_exe) == 0;
            for (const std::wstring& existing : app_paths)
                if (_wcsicmp(existing.c_str(), candidate.exe.c_str()) == 0) duplicate = true;
            if (duplicate) continue;
            FluentMenuItem row;
            row.command = 100 + static_cast<int>(app_paths.size());
            row.text = candidate.label.empty() ? candidate.exe : candidate.label;
            row.icon_path = candidate.exe;
            row.icon_index = 0;
            items.push_back(std::move(row));
            app_paths.push_back(candidate.exe);
        }
    }
    items.back().separator_after = true;
    FluentMenuItem other;
    other.command = 2;
    other.text = luling::l10n::Pick(L"\x9009\x62E9\x5176\x4ED6\x5E94\x7528\x2026",
                                    L"Choose another app\x2026");
    other.glyph = L"\xE71D";
    items.push_back(std::move(other));

    if (!text_menu_.Create(hwnd_, &compositor_, scale_)) return;
    text_menu_.SetTheme(dark_, HexColor(0x0078D4));
    text_menu_.SetTopmost(native_kind_ == NativeKind::None);
    const D2D1_RECT_F rect = OpenButtonRect();
    POINT screen{static_cast<LONG>(rect.left), static_cast<LONG>(rect.bottom)};
    ClientToScreen(hwnd_, &screen);
    const int cmd = text_menu_.TrackPopup(screen, std::move(items));
    if (cmd == 1) {
        PostAction(QuickPreviewAction::Open);
    } else if (cmd == 2) {
        PostAction(QuickPreviewAction::OpenWithDialog);
    } else if (cmd >= 100 && static_cast<size_t>(cmd - 100) < app_paths.size()) {
        PostAction(QuickPreviewAction::OpenWith, nullptr, app_paths[static_cast<size_t>(cmd - 100)]);
    }
}

QuickPreviewItem QuickPreviewWindow::ActionTarget() const {
    if (native_kind_ == NativeKind::Archive && archive_.IsFolderListing())
        return FolderPreviewTarget(item_, archive_.SelectedPath(), archive_.SelectedIsDirectory());
    return item_;
}

void QuickPreviewWindow::PostAction(QuickPreviewAction action, const QuickPreviewItem* target,
                                    const std::wstring& app_path) {
    if (!owner_ || !command_message_ || action == QuickPreviewAction::None) return;
    if (native_kind_ == NativeKind::Archive &&
        !CanApplyPreviewFileAction(archive_.IsFolderListing(), archive_.HasSelection())) {
        if (action == QuickPreviewAction::CopyPath || action == QuickPreviewAction::Copy)
            luling::ops::WriteClipboardText(archive_.SelectedPath());
        return;
    }
    QuickPreviewCommand command;
    command.action = action;
    command.target = target ? *target : ActionTarget();
    command.app_path = app_path;
    if (command.target.path.empty()) return;
    command.folder_child = command.target.path != item_.path;
    command.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const UINT_PTR token = commands_.Push(std::move(command));
    if (token && !PostMessageW(owner_, command_message_, static_cast<WPARAM>(action), static_cast<LPARAM>(token))) {
        QuickPreviewCommand ignored;
        commands_.Take(token, ignored);
    }
}

float QuickPreviewWindow::FitScale(float view_w, float view_h) const noexcept {
    if (TurnedW() == 0 || TurnedH() == 0) return 1.0f;
    return (std::min)(view_w / TurnedW(), view_h / TurnedH());
}

bool QuickPreviewWindow::CanPanImage() const {
    if (image_fit_ || native_kind_ != NativeKind::Bitmap || decoded_w_ == 0) return false;
    const D2D1_RECT_F content = ContentRect();
    const float view_w = content.right - content.left;
    const float view_h = content.bottom - content.top;
    return TurnedW() * image_zoom_ > view_w + 0.5f ||
           TurnedH() * image_zoom_ > view_h + 0.5f;
}

void QuickPreviewWindow::SetFitMode() {
    image_fit_ = true;
    image_zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
}

void QuickPreviewWindow::SetActualPixels() {
    image_fit_ = false;
    image_zoom_ = 1.0f;
    pan_x_ = 0.0f;
    pan_y_ = 0.0f;
    const D2D1_RECT_F content = ContentRect();
    ClampPan(content.right - content.left, content.bottom - content.top);
}

void QuickPreviewWindow::ToggleFitActual() {
    if (image_fit_) SetActualPixels();
    else SetFitMode();
}

void QuickPreviewWindow::ClampPan(float view_w, float view_h) {
    if (decoded_w_ == 0 || decoded_h_ == 0 || image_fit_) {
        pan_x_ = 0.0f;
        pan_y_ = 0.0f;
        return;
    }
    const float draw_w = TurnedW() * image_zoom_;
    const float draw_h = TurnedH() * image_zoom_;
    const float max_x = (std::max)(0.0f, (draw_w - view_w) * 0.5f);
    const float max_y = (std::max)(0.0f, (draw_h - view_h) * 0.5f);
    pan_x_ = std::clamp(pan_x_, -max_x, max_x);
    pan_y_ = std::clamp(pan_y_, -max_y, max_y);
}

void QuickPreviewWindow::ZoomAt(float cursor_x, float cursor_y, float factor) {
    if (decoded_w_ == 0 || decoded_h_ == 0) return;
    const D2D1_RECT_F content = ContentRect();
    const float view_w = content.right - content.left;
    const float view_h = content.bottom - content.top;
    const float fit = FitScale(view_w, view_h);
    const float old_zoom = image_fit_ ? fit : image_zoom_;
    const float max_zoom = (std::max)(fit, kMaxDecodedZoom);
    float next = std::clamp(old_zoom * factor, fit, max_zoom);
    if (next <= fit * 1.02f) {
        SetFitMode();
        return;
    }
    const float draw_w = TurnedW() * old_zoom;
    const float draw_h = TurnedH() * old_zoom;
    const float left = content.left + (view_w - draw_w) * 0.5f + pan_x_;
    const float top = content.top + (view_h - draw_h) * 0.5f + pan_y_;
    const float u = (cursor_x - left) / (std::max)(0.001f, old_zoom);
    const float v = (cursor_y - top) / (std::max)(0.001f, old_zoom);
    image_fit_ = false;
    image_zoom_ = next;
    const float new_w = TurnedW() * next;
    const float new_h = TurnedH() * next;
    pan_x_ = cursor_x - u * next - content.left - (view_w - new_w) * 0.5f;
    pan_y_ = cursor_y - v * next - content.top - (view_h - new_h) * 0.5f;
    ClampPan(view_w, view_h);
}

D2D1_RECT_F QuickPreviewWindow::ImageDest(const D2D1_RECT_F& content) const {
    if (image_fit_ || decoded_w_ == 0 || decoded_h_ == 0) return content;
    const float view_w = content.right - content.left;
    const float view_h = content.bottom - content.top;
    const float draw_w = TurnedW() * image_zoom_;
    const float draw_h = TurnedH() * image_zoom_;
    const float left = content.left + (view_w - draw_w) * 0.5f + pan_x_;
    const float top = content.top + (view_h - draw_h) * 0.5f + pan_y_;
    return D2D1::RectF(left, top, left + draw_w, top + draw_h);
}

uint32_t QuickPreviewWindow::RequestedPixelSize(const D2D1_RECT_F& content) const {
    const float longest = (std::max)(content.right - content.left, content.bottom - content.top);
    return ipc::BucketPreviewPixelSize(static_cast<uint32_t>((std::max)(1.0f, longest)));
}

// PULSEICO payload from the host (preview_host/image_frames.h).
void QuickPreviewWindow::ParseIconSizes(const std::wstring& payload) {
    if (payload.rfind(L"PULSEICO\t1\n", 0) != 0) return;
    std::vector<IconSize> sizes;
    int selected = -1;
    size_t pos = payload.find(L'\n') + 1;
    while (pos < payload.size()) {
        size_t end = payload.find(L'\n', pos);
        if (end == std::wstring::npos) end = payload.size();
        const std::wstring line = payload.substr(pos, end - pos);
        pos = end + 1;
        if (line.rfind(L"S\t", 0) == 0) {
            selected = _wtoi(line.c_str() + 2);
        } else if (line.rfind(L"E\t", 0) == 0 && sizes.size() < 64) {
            IconSize size;
            const wchar_t* p = line.c_str() + 2;
            wchar_t* next = nullptr;
            size.width = static_cast<uint32_t>(wcstoul(p, &next, 10));
            if (next && *next == L'\t') size.height = static_cast<uint32_t>(wcstoul(next + 1, &next, 10));
            if (next && *next == L'\t') size.bits = static_cast<uint32_t>(wcstoul(next + 1, &next, 10));
            if (next && *next == L'\t') size.png = next[1] == L'1';
            sizes.push_back(size);
        }
    }
    icon_sizes_ = std::move(sizes);
    icon_selected_ = selected >= 0 && selected < static_cast<int>(icon_sizes_.size()) ? selected : -1;
}

// Size pills centred above the HUD; the shown size is highlighted and names
// its format ("256 · PNG"). Only clicks pick a size: ← / → still change files.
void QuickPreviewWindow::DrawIconSizes(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                       ID2D1SolidColorBrush* brush) {
    icon_pill_rects_.clear();
    IDWriteTextFormat* format = compositor_.SmallFormat();
    auto* factory = compositor_.DwriteFactory();
    if (!dc || !brush || !format || !factory || icon_sizes_.size() < 1) return;
    const float s = scale_;
    std::vector<std::wstring> labels;
    float total = 0.0f;
    std::vector<float> widths;
    for (size_t i = 0; i < icon_sizes_.size(); ++i) {
        const IconSize& size = icon_sizes_[i];
        std::wstring label = size.width == size.height ? std::to_wstring(size.width)
            : std::to_wstring(size.width) + L"\x00D7" + std::to_wstring(size.height);
        if (static_cast<int>(i) == icon_selected_) {
            if (size.png) label += L" \x00B7 PNG";
            else if (size.bits) label += L" \x00B7 " + std::to_wstring(size.bits) + L"-bit";
        }
        const float w = typography::MeasureAdvance(factory, format, label) + 20.0f * s;
        widths.push_back(w);
        total += w;
        labels.push_back(std::move(label));
    }
    const float gap = 6.0f * s, h = 22.0f * s;
    total += gap * static_cast<float>(labels.size() - 1);
    if (total > content.right - content.left - 24.0f * s) return;  // too narrow: the HUD still names the size
    const float bottom = content.bottom - kHudHeight * s - 12.0f * s;
    float x = std::round((content.left + content.right - total) * 0.5f);
    const float y = std::round(bottom - h);
    ComPtr<ID2D1SolidColorBrush> pill, accent, on_accent;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x2B2B2B, 0.92f) : D2D1::ColorF(0xF3F3F3, 0.95f), &pill);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x60CDFF) : D2D1::ColorF(0x005FB8), &accent);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x062030) : D2D1::ColorF(0xFFFFFF), &on_accent);
    const auto text_alignment = format->GetTextAlignment();
    const auto paragraph_alignment = format->GetParagraphAlignment();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (size_t i = 0; i < labels.size(); ++i) {
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + widths[i], y + h);
        icon_pill_rects_.push_back(r);
        const bool on = static_cast<int>(i) == icon_selected_;
        ID2D1SolidColorBrush* fill = on ? accent.get() : pill.get();
        if (fill) dc->FillRoundedRectangle(D2D1::RoundedRect(r, h * 0.5f, h * 0.5f), fill);
        ID2D1SolidColorBrush* ink = on && on_accent.get() ? on_accent.get() : brush;
        dc->DrawTextW(labels[i].data(), static_cast<UINT32>(labels[i].size()), format, r, ink,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        x += widths[i] + gap;
    }
    format->SetTextAlignment(text_alignment);
    format->SetParagraphAlignment(paragraph_alignment);
}

int QuickPreviewWindow::HitIconSize(POINT client) const {
    if (native_kind_ != NativeKind::Bitmap) return -1;
    const float x = static_cast<float>(client.x), y = static_cast<float>(client.y);
    for (size_t i = 0; i < icon_pill_rects_.size(); ++i) {
        const D2D1_RECT_F& r = icon_pill_rects_[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return static_cast<int>(i);
    }
    return -1;
}

void QuickPreviewWindow::RotateImage() {
    if (native_kind_ != NativeKind::Bitmap || decoded_w_ == 0 || decoded_h_ == 0) return;
    image_turns_ = (image_turns_ + 1) & 3;
    const D2D1_RECT_F content = ContentRect();
    ClampPan(content.right - content.left, content.bottom - content.top);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int QuickPreviewWindow::HitFooterChip(POINT client) const {
    if (!ShowsImageFooter()) return 0;
    const float x = static_cast<float>(client.x), y = static_cast<float>(client.y);
    for (int chip = 1; chip <= 4; ++chip) {
        const D2D1_RECT_F& rect = footer_chip_rect_[chip];
        if (rect.right <= rect.left) continue;
        if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) return chip;
    }
    return 0;
}

void QuickPreviewWindow::ActivateFooterChip(int chip) {
    if (chip == 0) return;
    if (chip == kChipZoom) {
        if (native_kind_ == NativeKind::Bitmap) ToggleFitActual();
        else if (native_kind_ == NativeKind::Pages) TogglePagesFit();
        else return;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (native_kind_ != NativeKind::Bitmap) return;
    switch (chip) {
    case kChipBackdrop:
        image_backdrop_ = (image_backdrop_ + 1) & 3;
        break;
    case kChipInfo: {
        image_info_ = !image_info_;
        // The picture keeps its zoom; the view just gets narrower.
        const D2D1_RECT_F content = ContentRect();
        ClampPan(content.right - content.left, content.bottom - content.top);
        break;
    }
    case kChipRotate:
        RotateImage();
        return;
    default:
        return;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::DrawImageBackdrop(ID2D1DeviceContext* dc, const D2D1_RECT_F& draw) {
    if (!dc || draw.right <= draw.left || draw.bottom <= draw.top) return;
    const float s = scale_;
    ImageStats stats;
    const bool have = preview_pixels_ &&
        thumbnails_.Stats(item_.path, preview_pixels_, item_.modified, item_.size, stats);
    // Auto: a checkerboard only when the picture actually has transparency;
    // an opaque picture keeps the plain window background (no matte flash).
    if (image_backdrop_ == 0 && (have ? !stats.has_alpha : true)) return;
    const bool checker = image_backdrop_ == 3 ||
        (image_backdrop_ == 0 && have && stats.has_alpha);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (!checker) {
        const bool light = image_backdrop_ == 1;
        dc->CreateSolidColorBrush(D2D1::ColorF(light ? 0xFFFFFF : 0x000000, 1.0f), &brush);
        if (brush.get())
            dc->FillRoundedRectangle(D2D1::RoundedRect(draw, 4.0f * s, 4.0f * s), brush.get());
        return;
    }
    // Checkerboard: 8 DIP cells, light grey on white.
    dc->PushAxisAlignedClip(draw, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, dark_ ? 0.92f : 1.0f), &brush);
    if (brush.get())
        dc->FillRoundedRectangle(D2D1::RoundedRect(draw, 4.0f * s, 4.0f * s), brush.get());
    ComPtr<ID2D1SolidColorBrush> cell;
    dc->CreateSolidColorBrush(D2D1::ColorF(dark_ ? 0x3A3A3A : 0xCFCFCF), &cell);
    const float step = std::round(8.0f * s);
    int row = 0;
    if (cell.get()) {
        for (float y = draw.top; y < draw.bottom; y += step, ++row) {
            for (float x = draw.left + (row % 2 ? step : 0.0f); x < draw.right; x += step * 2.0f) {
                dc->FillRectangle(D2D1::RectF(x, y, (std::min)(x + step, draw.right),
                                              (std::min)(y + step, draw.bottom)), cell.get());
            }
        }
    }
    dc->PopAxisAlignedClip();
}

void QuickPreviewWindow::DrawImageFooter(ID2D1DeviceContext* dc,
                                         ID2D1SolidColorBrush* text_brush,
                                         ID2D1SolidColorBrush* secondary_brush) {
    if (!dc || !text_brush || !ShowsImageFooter()) return;
    const float s = scale_;
    const D2D1_RECT_F bar = FooterRect();
    if (bar.bottom <= bar.top) return;
    ComPtr<ID2D1SolidColorBrush> background, line, hover;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x000000, 0.34f) : D2D1::ColorF(0xFFFFFF, 0.66f),
                              &background);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.08f) : D2D1::ColorF(0x000000, 0.08f),
                              &line);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.10f) : D2D1::ColorF(0x000000, 0.07f),
                              &hover);
    if (background.get()) dc->FillRectangle(bar, background.get());
    if (line.get())
        dc->FillRectangle(D2D1::RectF(bar.left, bar.top, bar.right,
                                      bar.top + (std::max)(1.0f, std::round(s))), line.get());

    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (!format) return;
    const auto old_alignment = format->GetTextAlignment();
    const auto old_paragraph = format->GetParagraphAlignment();

    // Left: type · dimensions · size · modified.
    const std::wstring meta = MetaLine();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    const float left = bar.left + 16.0f * s;
    const float chip_reserve = 300.0f * s;
    dc->DrawTextW(meta.data(), static_cast<UINT32>(meta.size()), format,
                  D2D1::RectF(left, bar.top, (std::max)(left, bar.right - chip_reserve), bar.bottom),
                  secondary_brush ? secondary_brush : text_brush,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    // Right, laid out from the edge inward: rotate, info, backdrop, zoom.
    struct Chip {
        int id;
        std::wstring label;
        bool on;
    };
    std::vector<Chip> chips;
    wchar_t zoom[32]{};
    if (image_fit_) swprintf_s(zoom, L"%s", luling::l10n::Get(luling::l10n::StringId::PreviewFit).c_str());
    else swprintf_s(zoom, L"%.0f%%", image_zoom_ * 100.0f);
    chips.push_back({kChipRotate, luling::l10n::Get(luling::l10n::StringId::PreviewRotate), false});
    chips.push_back({kChipInfo, luling::l10n::Get(luling::l10n::StringId::PreviewImageInfo), image_info_});
    chips.push_back({kChipBackdrop, luling::l10n::Get(luling::l10n::StringId::PreviewBackdrop),
                     image_backdrop_ != 0});
    chips.push_back({kChipZoom, zoom, !image_fit_});

    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    float right = bar.right - 12.0f * s;
    const float chip_h = 22.0f * s;
    for (Chip& chip : chips) {
        const float width = TextWidthDip(chip.label) + 18.0f * s;
        const D2D1_RECT_F rect = D2D1::RectF(right - width, bar.top + (bar.bottom - bar.top - chip_h) * 0.5f,
                                             right, bar.top + (bar.bottom - bar.top + chip_h) * 0.5f);
        footer_chip_rect_[chip.id] = rect;
        if (footer_hover_ == chip.id || chip.on) {
            if (hover.get())
                dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f * s, 6.0f * s), hover.get());
        }
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawTextW(chip.label.data(), static_cast<UINT32>(chip.label.size()), format, rect,
                      text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        right = rect.left - 4.0f * s;
    }
    format->SetTextAlignment(old_alignment);
    format->SetParagraphAlignment(old_paragraph);
}

void QuickPreviewWindow::DrawInfoDrawer(ID2D1DeviceContext* dc,
                                        ID2D1SolidColorBrush* text_brush,
                                        ID2D1SolidColorBrush* secondary_brush) {
    const float drawer_w = InfoDrawerWidth();
    if (!dc || !text_brush || drawer_w <= 0.0f || native_kind_ != NativeKind::Bitmap) return;
    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (!format) return;
    const float s = scale_;
    const D2D1_RECT_F content = ContentRect();
    const D2D1_RECT_F panel = D2D1::RectF(content.right, content.top,
                                          static_cast<float>(compositor_.Width()),
                                          content.bottom);
    if (panel.right <= panel.left || panel.bottom <= panel.top) return;
    ComPtr<ID2D1SolidColorBrush> background, line;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x000000, 0.22f) : D2D1::ColorF(0xFFFFFF, 0.55f),
                              &background);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.08f) : D2D1::ColorF(0x000000, 0.08f),
                              &line);
    if (background.get()) dc->FillRectangle(panel, background.get());
    if (line.get())
        dc->FillRectangle(D2D1::RectF(panel.left, panel.top,
                                      panel.left + (std::max)(1.0f, std::round(s)), panel.bottom),
                          line.get());
    dc->PushAxisAlignedClip(panel, D2D1_ANTIALIAS_MODE_ALIASED);
    const float pad = 16.0f * s, left = panel.left + pad, right = panel.right - pad;
    const auto old_alignment = format->GetTextAlignment();
    const auto old_paragraph = format->GetParagraphAlignment();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float y = panel.top + 12.0f * s;
    const std::wstring info_title = luling::l10n::Get(luling::l10n::StringId::Information);
    dc->DrawTextW(info_title.data(), static_cast<UINT32>(info_title.size()), format,
                  D2D1::RectF(left, y, right, y + 24.0f * s), text_brush,
                  D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    y += 30.0f * s;

    std::vector<std::pair<std::wstring, std::wstring>> rows;
    const uint32_t shown_w = source_w_ ? source_w_ : decoded_w_;
    const uint32_t shown_h = source_h_ ? source_h_ : decoded_h_;
    if (shown_w && shown_h) {
        wchar_t dims[64]{};
        swprintf_s(dims, L"%u \x00D7 %u", shown_w, shown_h);
        rows.emplace_back(luling::l10n::ServiceText(L"\x5C3A\x5BF8"), dims);
    }
    if (const std::wstring ext = ExtensionLabel(item_.path); !ext.empty())
        rows.emplace_back(luling::l10n::Get(luling::l10n::StringId::PreviewFormat), ext);
    ImageStats stats;
    const bool has_stats = thumbnails_.Stats(item_.path, preview_pixels_, item_.modified,
                                             item_.size, stats);
    if (has_stats)
        rows.emplace_back(luling::l10n::Get(luling::l10n::StringId::PreviewAlpha),
                          luling::l10n::Get(stats.has_alpha ? luling::l10n::StringId::PreviewYes
                                                            : luling::l10n::StringId::PreviewNo));
    std::vector<PreviewProperty> properties;
    thumbnails_.Properties(item_.path, item_.attrs, generation_, item_.modified, item_.size,
                           properties);
    for (const PreviewProperty& property : properties) {
        if (property.label == L"\x5C3A\x5BF8" || property.value.empty()) continue;
        rows.emplace_back(luling::l10n::ServiceText(property.label), property.value);
    }
    if (const std::wstring size = luling::format::ByteSize(item_.size, true); !size.empty())
        rows.emplace_back(luling::l10n::Get(luling::l10n::StringId::ColumnSize), size);
    if (item_.modified) {
        const std::wstring when = FormatShortTime(item_.modified);
        if (!when.empty())
            rows.emplace_back(luling::l10n::Get(luling::l10n::StringId::ModifiedTime), when);
    }
    float label_w = 0.0f;
    for (const auto& row : rows) label_w = (std::max)(label_w, TextWidthDip(row.first));
    label_w = (std::min)(label_w, (right - left) * 0.45f);
    const float row_h = 26.0f * s;
    for (const auto& row : rows) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        dc->DrawTextW(row.first.data(), static_cast<UINT32>(row.first.size()), format,
                      D2D1::RectF(left, y, left + label_w, y + row_h),
                      secondary_brush ? secondary_brush : text_brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        dc->DrawTextW(row.second.data(), static_cast<UINT32>(row.second.size()), format,
                      D2D1::RectF(left + label_w + 10.0f * s, y, right, y + row_h), text_brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        y += row_h;
    }

    if (has_stats) {
        y += 14.0f * s;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        const std::wstring histogram =
            luling::l10n::Get(luling::l10n::StringId::PreviewHistogram);
        dc->DrawTextW(histogram.data(), static_cast<UINT32>(histogram.size()), format,
                      D2D1::RectF(left, y, right, y + 22.0f * s),
                      secondary_brush ? secondary_brush : text_brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        y += 26.0f * s;
        const float chart_h = 86.0f * s;
        const D2D1_RECT_F chart = D2D1::RectF(left, y, right, y + chart_h);
        if (chart.bottom <= panel.bottom - 8.0f * s) {
            if (line.get())
                dc->DrawRoundedRectangle(D2D1::RoundedRect(chart, 4.0f * s, 4.0f * s), line.get(),
                                         (std::max)(1.0f, s));
            ComPtr<ID2D1Factory> factory;
            dc->GetFactory(&factory);
            const D2D1_RECT_F plot = D2D1::RectF(chart.left + 4.0f * s, chart.top + 6.0f * s,
                                                 chart.right - 4.0f * s, chart.bottom - 2.0f * s);
            const float alpha = dark_ ? 0.55f : 0.45f;
            const struct { const uint16_t* bins; uint32_t color; } channels[] = {
                {stats.r, 0xE5484Du}, {stats.g, 0x30A46Cu}, {stats.b, 0x3E7BFAu}};
            for (const auto& channel : channels) {
                ComPtr<ID2D1PathGeometry> path;
                ComPtr<ID2D1GeometrySink> sink;
                if (!factory.get() || FAILED(factory->CreatePathGeometry(&path)) ||
                    FAILED(path->Open(&sink)))
                    break;
                sink->BeginFigure(D2D1::Point2F(plot.left, plot.bottom), D2D1_FIGURE_BEGIN_FILLED);
                const float step = (plot.right - plot.left) / 63.0f;
                for (int i = 0; i < 64; ++i) {
                    const float v = static_cast<float>(channel.bins[i]) / 1000.0f;
                    sink->AddLine(D2D1::Point2F(plot.left + step * static_cast<float>(i),
                                                plot.bottom - v * (plot.bottom - plot.top)));
                }
                sink->AddLine(D2D1::Point2F(plot.right, plot.bottom));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (FAILED(sink->Close())) break;
                ComPtr<ID2D1SolidColorBrush> fill;
                dc->CreateSolidColorBrush(D2D1::ColorF(channel.color, alpha), &fill);
                if (fill.get()) dc->FillGeometry(path.get(), fill.get());
            }
        }
    }
    format->SetTextAlignment(old_alignment);
    format->SetParagraphAlignment(old_paragraph);
    dc->PopAxisAlignedClip();
}

void QuickPreviewWindow::DrawHud(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                 ID2D1SolidColorBrush* text_brush) {
    if (!dc || native_kind_ != NativeKind::Bitmap || decoded_w_ == 0) return;
    const float hud_h = kHudHeight * scale_;
    if (content.bottom - content.top < hud_h + 8.0f * scale_) return;
    const D2D1_RECT_F hud = D2D1::RectF(content.left, content.bottom - hud_h,
                                        content.right, content.bottom);
    ComPtr<ID2D1SolidColorBrush> background;
    dc->CreateSolidColorBrush(D2D1::ColorF(0x000000, dark_ ? 0.45f : 0.28f), &background);
    dc->FillRectangle(hud, background.get());

    const uint32_t shown_w = source_w_ ? source_w_ : decoded_w_;
    const uint32_t shown_h = source_h_ ? source_h_ : decoded_h_;
    wchar_t dims[64];
    swprintf_s(dims, L"%u\x00D7%u", shown_w, shown_h);
    std::wstring line = dims;
    if (source_w_ && source_h_ &&
        (source_w_ != decoded_w_ || source_h_ != decoded_h_)) {
        wchar_t preview[64];
        swprintf_s(preview, luling::l10n::Get(luling::l10n::StringId::PreviewDecodedFormat).c_str(),
                   (std::max)(decoded_w_, decoded_h_));
        line += L" \x00B7 ";
        line += preview;
    }
    const std::wstring size = luling::format::ByteSize(item_.size, true);
    if (!size.empty()) {
        line += L" \x00B7 ";
        line += size;
    }
    if (const std::wstring ext = ExtensionLabel(item_.path); !ext.empty()) {
        line += L" \x00B7 ";
        line += ext;
    }
    line += L" \x00B7 ";
    if (image_fit_) {
        line += luling::l10n::Get(luling::l10n::StringId::PreviewFit);
    } else {
        wchar_t percent[16];
        swprintf_s(percent, L"%.0f%%", image_zoom_ * 100.0f);
        line += percent;
    }
    const float pad = 12.0f * scale_;
    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (format) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawTextW(line.data(), static_cast<UINT32>(line.size()), format,
                      D2D1::RectF(hud.left + pad, hud.top, hud.right - pad, hud.bottom),
                      text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void QuickPreviewWindow::EnsureTextLayout(const std::wstring& text, bool hex, float width) {
    if (text.empty() || !compositor_.DwriteFactory() || !preview_text_format_.get()) {
        text_layout_.reset();
        return;
    }
    const std::wstring extension = L"." + ExtensionLabel(item_.path);
    const bool gutter = !hex && line_numbers_ && SyntaxWantsLineNumbers(extension);
    if (text_layout_.get() && text == preview_text_ && hex == text_layout_hex_ &&
        std::abs(width - text_layout_width_) < 0.5f &&
        std::abs(scale_ - text_layout_scale_) < 0.001f && dark_ == text_layout_dark_ &&
        gutter == (text_gutter_ > 0.0f)) return;
    preview_text_ = text;
    text_layout_.reset();
    preview_text_format_->SetWordWrapping(
        hex ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
    text_line_count_ = 1;
    for (const wchar_t c : text) if (c == L'\n') ++text_line_count_;
    if (!text.empty() && text.back() == L'\n') --text_line_count_;
    line_starts_.clear();
    text_gutter_ = 0.0f;
    if (gutter) {
        line_starts_.reserve(text_line_count_);
        line_starts_.push_back(0);
        for (size_t i = 0; i + 1 < text.size(); ++i)
            if (text[i] == L'\n') line_starts_.push_back(static_cast<uint32_t>(i + 1));
        float digit = 8.0f * scale_;
        ComPtr<IDWriteTextLayout> probe;
        if (SUCCEEDED(compositor_.DwriteFactory()->CreateTextLayout(L"0000000000", 10,
                preview_text_format_.get(), 1000.0f, 100.0f, &probe)) && probe.get()) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(probe->GetMetrics(&metrics))) digit = metrics.width / 10.0f;
        }
        const size_t digits = std::to_wstring(line_starts_.size()).size();
        text_gutter_ = (static_cast<float>((std::max)(digits, size_t{2})) + 2.0f) * digit;
    }
    compositor_.DwriteFactory()->CreateTextLayout(
        text.data(), static_cast<UINT32>(text.size()), preview_text_format_.get(),
        (std::max)(1.0f, width - text_gutter_), 100000.0f, &text_layout_);
    syntax_language_ = hex ? std::wstring() : std::wstring(SyntaxLanguageName(extension));
    if (!hex && text_layout_.get() && compositor_.Dc()) {
        for (auto& brush : syntax_brushes_) brush.reset();
        const auto spans = HighlightSyntax(extension, text);
        for (const SyntaxSpan& span : spans) {
            const auto index = static_cast<size_t>(span.token);
            if (index == 0 || index >= std::size(syntax_brushes_)) continue;
            if (!syntax_brushes_[index].get())
                compositor_.Dc()->CreateSolidColorBrush(
                    D2D1::ColorF(SyntaxTokenRgb(span.token, dark_)), &syntax_brushes_[index]);
            if (syntax_brushes_[index].get())
                text_layout_->SetDrawingEffect(syntax_brushes_[index].get(),
                                               DWRITE_TEXT_RANGE{span.start, span.length});
        }
    }
    text_layout_dark_ = dark_;
    text_layout_width_ = width;
    text_layout_scale_ = scale_;
    text_layout_hex_ = hex;
}

bool QuickPreviewWindow::HitTestText(float x, float y, uint32_t& index) {
    if (native_kind_ == NativeKind::Markdown) return markdown_.HitTest(x, y, index);
    if (!text_layout_.get()) return false;
    const D2D1_RECT_F content = ContentRect();
    const float pad = 20.0f * scale_;
    const float origin_x = TextOriginX();
    const float origin_y = content.top + pad - text_scroll_;
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(text_layout_->HitTestPoint(x - origin_x, y - origin_y, &trailing, &inside,
                                          &metrics))) return false;
    index = metrics.textPosition + (trailing ? metrics.length : 0u);
    index = (std::min)(index, static_cast<uint32_t>(preview_text_.size()));
    return true;
}

bool QuickPreviewWindow::HasTextSelection() const noexcept {
    if (native_kind_ == NativeKind::Table) return table_.HasSelection();
    if (native_kind_ == NativeKind::Tree) return tree_.HasCurrent();
    return sel_anchor_ != sel_focus_;
}

void QuickPreviewWindow::CopyTextSelection(bool require_selection) const {
    if (native_kind_ == NativeKind::Table) {
        // Cells as TSV (Excel pastes it back as a range).
        if (require_selection && !table_.HasSelection()) return;
        luling::ops::WriteClipboardText(table_.SelectionText());
        return;
    }
    if (native_kind_ == NativeKind::Tree) {
        // The current node's value (a subtree as JSON/XML), else the source.
        if (require_selection && !tree_.HasCurrent()) return;
        luling::ops::WriteClipboardText(tree_.CurrentValue());
        return;
    }
    if (preview_text_.empty()) return;
    uint32_t a = (std::min)(sel_anchor_, sel_focus_);
    uint32_t b = (std::max)(sel_anchor_, sel_focus_);
    a = (std::min)(a, static_cast<uint32_t>(preview_text_.size()));
    b = (std::min)(b, static_cast<uint32_t>(preview_text_.size()));
    if (b <= a) {
        if (require_selection) return;
        luling::ops::WriteClipboardText(preview_text_);
        return;
    }
    luling::ops::WriteClipboardText(preview_text_.substr(a, b - a));
}

bool QuickPreviewWindow::ShowsCopyButton() const noexcept {
    return native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex;
}

void QuickPreviewWindow::CopyAllText() {
    if (preview_text_.empty()) return;
    luling::ops::WriteClipboardText(preview_text_);
    copy_feedback_ = true;
    SetTimer(hwnd_, kCopyFeedbackTimer, 1400, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::SelectAllText() {
    if (native_kind_ == NativeKind::Table) {
        table_.SelectAll();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (native_kind_ == NativeKind::Tree) return;  // rows are picked one at a time
    sel_anchor_ = 0;
    sel_focus_ = static_cast<uint32_t>(preview_text_.size());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::OpenFind() {
    find_open_ = true;
    if (HasTextSelection() && find_query_.empty()) {
        uint32_t a = (std::min)(sel_anchor_, sel_focus_);
        uint32_t b = (std::max)(sel_anchor_, sel_focus_);
        a = (std::min)(a, static_cast<uint32_t>(preview_text_.size()));
        b = (std::min)(b, static_cast<uint32_t>(preview_text_.size()));
        if (b > a && b - a <= kFindQueryLimit)
            find_query_ = preview_text_.substr(a, b - a);
    }
    UpdateFindMatches();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (!EnsureFindEdit()) return;
    SetWindowTextW(find_edit_, find_query_.c_str());
    LayoutFindEdit();
    ShowWindow(find_edit_, SW_SHOW);
    SetForegroundWindow(GetAncestor(find_edit_, GA_ROOT));
    SetFocus(find_edit_);
    SendMessageW(find_edit_, EM_SETSEL, 0, -1);
    if (compositor_.CustomEditEnabled())
        PaintFindEditLuma(find_edit_, nullptr);
}

void QuickPreviewWindow::CloseFind() {
    find_open_ = false;
    find_query_.clear();
    find_matches_.clear();
    archive_.SetFilter(L"");
    find_index_ = 0;
    if (find_edit_) {
        SetWindowTextW(find_edit_, L"");
        ShowWindow(find_edit_, SW_HIDE);
    }
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

bool QuickPreviewWindow::EnsureFindEdit() {
    if (find_edit_) return true;
    if (!hwnd_) return false;
    if (!find_edit_font_) RecreateFormats();
    find_edit_ = CreateChildEdit(hwnd_);
    if (!find_edit_) return false;
    SetWindowTheme(find_edit_, L"", L"");
    if (find_edit_font_)
        SendMessageW(find_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(find_edit_font_), TRUE);
    if (!find_edit_brush_)
        find_edit_brush_ = CreateSolidBrush(FindEditBrushColor(dark_));
    SendMessageW(find_edit_, EM_SETLIMITTEXT, static_cast<WPARAM>(kFindQueryLimit), 0);
    SendMessageW(find_edit_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(luling::l10n::Get(luling::l10n::StringId::Search).c_str()));
    SetWindowSubclass(find_edit_, FindEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return true;
}

void QuickPreviewWindow::LayoutFindEdit() {
    if (!find_edit_ || !find_open_ || !hwnd_) return;
    const D2D1_RECT_F cell = FindEditCell();
    POINT pt{ static_cast<int>(std::lround(cell.left)),
              static_cast<int>(std::lround(cell.top)) };

    const int w = (std::max)(40, static_cast<int>(std::lround(cell.right - cell.left)));
    const int cell_h = (std::max)(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
    const int line_h = EditLineHeight(find_edit_, find_edit_font_, cell_h);
    const int y = pt.y + (std::max)(0, (cell_h - line_h) / 2);
    RECT cur{};
    GetWindowRect(find_edit_, &cur);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&cur), 2);
    const bool moved = cur.left != pt.x || cur.top != y || cur.right != pt.x + w ||
        cur.bottom != y + line_h;
    // Child coordinates stay local when the preview window moves.
    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
    if (!moved) flags |= SWP_NOMOVE | SWP_NOSIZE | SWP_NOREDRAW;
    SetWindowPos(find_edit_, HWND_TOP, pt.x, y, w, line_h, flags);
    if (moved && compositor_.CustomEditEnabled())
        PaintFindEditLuma(find_edit_, nullptr);
}

void QuickPreviewWindow::SyncFindFromEdit() {
    if (!find_edit_) return;
    wchar_t buf[kFindQueryLimit + 1]{};
    GetWindowTextW(find_edit_, buf, ARRAYSIZE(buf));
    if (find_query_ == buf) return;
    find_query_ = buf;
    if (native_kind_ == NativeKind::Archive) {
        // Archive search filters the tree (matches stay with their folders).
        archive_.SetFilter(find_query_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    UpdateFindMatches();
    if (!find_matches_.empty()) {
        find_index_ = 0;
        ScrollMatchIntoView(find_matches_[0]);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void QuickPreviewWindow::PaintFindEditLuma(HWND hwnd, HDC hdc) {
    if (!hwnd || !compositor_.CustomEditEnabled()) return;
    HideCaret(hwnd);
    IDWriteTextFormat* format = compositor_.AddressFormat();
    if (!format) format = compositor_.TextFormat();
    if (format && compositor_.PresentLumaEdit(hwnd, format, FindEditFg(dark_),
                                              FindEditBg(dark_)))
        return;
    if (!hdc) return;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (!find_edit_brush_)
        find_edit_brush_ = CreateSolidBrush(FindEditBrushColor(dark_));
    FillRect(hdc, &rc, EditBackBrush(find_edit_brush_));
}

LRESULT QuickPreviewWindow::ForwardFindEditKeepLuma(HWND hwnd, UINT message,
                                                    WPARAM wparam, LPARAM lparam) {
    const bool mouse = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK ||
        message == WM_LBUTTONUP || message == WM_MOUSEMOVE || message == WM_CAPTURECHANGED;
    LRESULT result;
    if (mouse) {
        result = compositor_.CallLumaEditMouse(
            hwnd, message, wparam, lparam,
            compositor_.AddressFormat() ? compositor_.AddressFormat()
                                        : compositor_.TextFormat());
        if (message != WM_MOUSEMOVE || GetCapture() == hwnd)
            PaintFindEditLuma(hwnd, nullptr);
    } else {
        SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
        result = DefSubclassProc(hwnd, message, wparam, lparam);
        HideCaret(hwnd);
        PaintFindEditLuma(hwnd, nullptr);
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    }
    return result;
}

void QuickPreviewWindow::DestroyFindEdit() {
    if (find_edit_) {
        if (IsWindow(find_edit_)) {
            RemoveWindowSubclass(find_edit_, FindEditProc, 1);
            DestroyWindow(find_edit_);
        }
        find_edit_ = nullptr;
    }
    if (find_edit_font_) {
        DeleteObject(find_edit_font_);
        find_edit_font_ = nullptr;
    }
    if (find_edit_brush_) {
        DeleteObject(find_edit_brush_);
        find_edit_brush_ = nullptr;
    }
}

LRESULT CALLBACK QuickPreviewWindow::FindEditProc(HWND hwnd, UINT message, WPARAM wparam,
                                                  LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto* self = reinterpret_cast<QuickPreviewWindow*>(data);
    if (!self) return DefSubclassProc(hwnd, message, wparam, lparam);
    const bool luma = SynchronizeChildEditBackend(self->compositor_, hwnd);
    if (luma && (message == WM_PRINT || message == WM_PRINTCLIENT ||
                 message == WM_NCPAINT)) {
        return 0;
    }
    switch (message) {
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            self->CloseFind();
            return 0;
        }
        if (wparam == VK_RETURN || wparam == VK_F3) {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            self->FindNext(shift ? -1 : 1);
            return 0;
        }
        if (luma) return self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam);
        break;
    case WM_CHAR:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) return 0;
        if (luma) {
            const LRESULT result = self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam);
            self->SyncFindFromEdit();
            return result;
        }
        {
            const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
            self->SyncFindFromEdit();
            return result;
        }
    case WM_PASTE:
    case WM_CUT: {
        const LRESULT result = luma
            ? self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam)
            : DefSubclassProc(hwnd, message, wparam, lparam);
        self->SyncFindFromEdit();
        return result;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_CAPTURECHANGED:
        if (luma) return self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam);
        break;
    case WM_IME_COMPOSITION:
    case WM_IME_CHAR:
        if (luma) {
            const LRESULT result = self->ForwardFindEditKeepLuma(hwnd, message, wparam, lparam);
            self->SyncFindFromEdit();
            return result;
        }
        break;
    case WM_PAINT: {
        if (!luma) break;
        self->PaintFindEditLuma(hwnd, nullptr);
        return 0;
    }
    case WM_SETFOCUS: {
        const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        if (luma) {
            HideCaret(hwnd);
            SetTimer(hwnd, kFindEditCaretTimer, GetCaretBlinkTime(), nullptr);
            self->PaintFindEditLuma(hwnd, nullptr);
        }
        if (self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }
    case WM_KILLFOCUS:
        KillTimer(hwnd, kFindEditCaretTimer);
        if (self->hwnd_) InvalidateRect(self->hwnd_, nullptr, FALSE);
        break;
    case WM_TIMER:
        if (wparam == kFindEditCaretTimer) {
            if (GetCapture() != hwnd)
                self->PaintFindEditLuma(hwnd, nullptr);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        if (luma) return 1;
        {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            if (!self->find_edit_brush_)
                self->find_edit_brush_ = CreateSolidBrush(FindEditBrushColor(self->dark_));
            FillRect(reinterpret_cast<HDC>(wparam), &rc, EditBackBrush(self->find_edit_brush_));
            return 1;
        }
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void QuickPreviewWindow::ShowContextMenu(POINT screen) {
    if (!text_menu_.Create(hwnd_, &compositor_, scale_)) return;
    text_menu_.SetTheme(dark_, HexColor(0x0078D4));
    // System preview handlers (video, PDF, Office) sit in a topmost overlay
    // over the content area; a plain popup would open underneath it.
    text_menu_.SetTopmost(native_kind_ == NativeKind::None);
    using luling::l10n::StringId;
    const bool text_kind = native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                           native_kind_ == NativeKind::Markdown || native_kind_ == NativeKind::Table;
    const QuickPreviewItem target = ActionTarget();
    const uint64_t menu_generation = generation_;
    const bool child = target.path != item_.path;
    const bool archive_entry = native_kind_ == NativeKind::Archive &&
        !CanApplyPreviewFileAction(archive_.IsFolderListing(), archive_.HasSelection());
    const auto archive_path = archive_entry ? archive_.SelectedPath() : std::wstring{};
    const bool file_verbs = !archive_entry && owner_ && command_message_ != 0 && !target.path.empty();
    std::vector<FluentMenuItem> items;
    const auto add = [&](int command, StringId label, const wchar_t* glyph,
                         const wchar_t* shortcut, bool enabled = true,
                         bool separator_after = false) {
        FluentMenuItem item;
        item.command = command;
        item.text = luling::l10n::Get(label);
        item.glyph = glyph;
        item.shortcut = shortcut;
        item.enabled = enabled;
        item.separator_after = separator_after;
        items.push_back(std::move(item));
    };
    const auto file_cmd = [](QuickPreviewAction action) {
        return kFileCmdBase + static_cast<int>(action);
    };
    if (archive_entry) {
        add(kArchiveCmdCopyPath, StringId::CopyPath, kLinkGlyph, L"Ctrl+Shift+C");
        items.back().text = luling::l10n::Pick(L"复制包内路径", L"Copy path inside archive");
    }
    if (native_kind_ == NativeKind::Bitmap && !archive_entry) {
        FluentMenuItem rotate;
        rotate.command = kImageCmdRotate;
        rotate.text = luling::l10n::Pick(L"向右旋转", L"Rotate right");
        rotate.glyph = L"\xE7AD";
        rotate.shortcut = L"R";
        items.push_back(std::move(rotate));
        FluentMenuItem info;
        info.command = kImageCmdInfo;
        info.text = image_info_ ? luling::l10n::Pick(L"隐藏图片信息", L"Hide image info")
                                : luling::l10n::Pick(L"图片信息", L"Image info");
        info.glyph = L"\xE946";
        info.shortcut = L"I";
        items.push_back(std::move(info));
    }
    if (text_kind) {
        // Text verbs keep their ids and order; file verbs follow below them.
        add(kTextCmdCopy, StringId::Copy, kCopyGlyph, L"Ctrl+C", HasTextSelection());
        add(kTextCmdSelectAll, StringId::SelectAll, kSelectAllGlyph, L"Ctrl+A", true, !file_verbs);
        const bool line_toggle = native_kind_ == NativeKind::Text &&
            SyntaxWantsLineNumbers(L"." + ExtensionLabel(item_.path));
        add(kTextCmdFind, StringId::Search, kSearchGlyph, L"Ctrl+F", true,
            file_verbs && !line_toggle);
        if (line_toggle) {
            FluentMenuItem item;
            item.command = kTextCmdLineNumbers;
            item.text = line_numbers_ ? (luling::l10n::Pick(L"\x9690\x85CF\x884C\x53F7", L"Hide line numbers"))
                                      : (luling::l10n::Pick(L"\x663E\x793A\x884C\x53F7", L"Show line numbers"));
            item.glyph = L"\xE8FD";
            item.shortcut = L"";
            item.separator_after = file_verbs;
            items.push_back(std::move(item));
        }
    }
    if (file_verbs) {
        const bool writable = !target.read_only;
        add(file_cmd(QuickPreviewAction::Open), StringId::Open, kOpenGlyph, L"Enter");
        if (!text_kind)
            add(file_cmd(QuickPreviewAction::Copy), StringId::Copy, kCopyGlyph, L"Ctrl+C");
        add(file_cmd(QuickPreviewAction::Cut), StringId::Cut, kCutGlyph, L"Ctrl+X", writable);
        add(file_cmd(QuickPreviewAction::CopyPath), StringId::CopyPath, kLinkGlyph,
            L"Ctrl+Shift+C", true, true);
        add(file_cmd(QuickPreviewAction::ToggleStar),
            target.starred ? StringId::Unstar : StringId::Star,
            target.starred ? kStarFilledGlyph : kStarGlyph, L"", true, true);
        if (child) items.back().text = luling::l10n::Pick(L"切换星标", L"Toggle star");
        add(file_cmd(QuickPreviewAction::Rename), StringId::Rename, kRenameGlyph, L"F2", writable);
        add(file_cmd(QuickPreviewAction::Delete), StringId::Delete, kDeleteGlyph, L"Delete",
            writable, true);
        add(file_cmd(QuickPreviewAction::Properties), StringId::Properties, kPropertiesGlyph,
            L"Alt+Enter");
    }
    if (items.empty()) return;
    const int cmd = text_menu_.TrackPopup(screen, std::move(items));
    if (cmd == kTextCmdCopy) CopyTextSelection(true);
    else if (cmd == kArchiveCmdCopyPath) luling::ops::WriteClipboardText(archive_path);
    else if (cmd == kTextCmdSelectAll) SelectAllText();
    else if (cmd == kTextCmdFind) OpenFind();
    else if (cmd == kTextCmdLineNumbers) {
        line_numbers_ = !line_numbers_;
        text_layout_.reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (cmd == kImageCmdRotate) RotateImage();
    else if (cmd == kImageCmdInfo) {
        image_info_ = !image_info_;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (cmd >= kFileCmdBase && menu_generation == generation_ && visible())
        PostAction(static_cast<QuickPreviewAction>(cmd - kFileCmdBase), &target);
}

void QuickPreviewWindow::UpdateFindMatches() {
    find_matches_.clear();
    find_index_ = 0;
    if (find_query_.empty() || preview_text_.empty()) return;
    const size_t n = preview_text_.size();
    const size_t q = find_query_.size();
    if (q > n) return;
    for (size_t i = 0; i + q <= n; ++i) {
        bool match = true;
        for (size_t j = 0; j < q; ++j) {
            if (std::towlower(preview_text_[i + j]) != std::towlower(find_query_[j])) {
                match = false;
                break;
            }
        }
        if (match) find_matches_.push_back(static_cast<uint32_t>(i));
    }
}

void QuickPreviewWindow::ScrollMatchIntoView(uint32_t start) {
    if (native_kind_ == NativeKind::Markdown) {
        markdown_.Reveal(start);
        return;
    }
    if (native_kind_ == NativeKind::Table) {
        table_.Reveal(start);
        return;
    }
    if (native_kind_ == NativeKind::Tree) {
        tree_.Reveal(start);
        return;
    }
    if (!text_layout_.get()) return;
    FLOAT x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(text_layout_->HitTestTextPosition(start, FALSE, &x, &y, &metrics))) return;
    const D2D1_RECT_F content = ContentRect();
    const float pad = 20.0f * scale_;
    const float view = (std::max)(1.0f, content.bottom - content.top - pad * 2.0f);
    if (y < text_scroll_) text_scroll_ = (std::max)(0.0f, y);
    else if (y + metrics.height > text_scroll_ + view)
        text_scroll_ = y + metrics.height - view;
}

void QuickPreviewWindow::FindNext(int direction) {
    if (find_matches_.empty()) return;
    if (direction >= 0) {
        find_index_ = (find_index_ + 1) % static_cast<uint32_t>(find_matches_.size());
    } else {
        find_index_ = (find_index_ + static_cast<uint32_t>(find_matches_.size()) - 1) %
                      static_cast<uint32_t>(find_matches_.size());
    }
    ScrollMatchIntoView(find_matches_[find_index_]);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

Theme QuickPreviewWindow::CurrentTheme() const {
    HIGHCONTRASTW value{sizeof(value)};
    const bool high_contrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
        (value.dwFlags & HCF_HIGHCONTRASTON);
    return high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, HexColor(0x0078D4));
}

void QuickPreviewWindow::DrawFindBar(ID2D1DeviceContext* dc, const D2D1_RECT_F& bar) {
    if (!dc) return;
    const bool high_contrast = [] {
        HIGHCONTRASTW value{sizeof(value)};
        return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
            (value.dwFlags & HCF_HIGHCONTRASTON);
    }();
    const Theme theme = CurrentTheme();
    find_painter_.SetCompositor(&compositor_);
    find_painter_.SetScale(scale_);
    if (!find_painter_.BeginFrame(theme, high_contrast)) return;
    ComPtr<ID2D1SolidColorBrush> fill;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x1F1F1F) : D2D1::ColorF(0xEDEDED), &fill);
    dc->FillRectangle(bar, fill.get());
    fluent::TextFieldSpec field{};
    field.bounds = FindFieldRect();
    field.leading_glyph = kSearchGlyph;
    field.compact_leading_glyph = true;
    field.suppress_text = true;
    field.hosted_edit = true;
    field.state.focused = find_edit_ && GetFocus() == find_edit_;
    field.state.enabled = true;
    std::wstring count_text;
    if (!find_query_.empty()) {
        wchar_t count[32]{};
        if (native_kind_ == NativeKind::Archive) swprintf_s(count, L"%u", archive_.FilterHits());
        else if (find_matches_.empty()) swprintf_s(count, L"0/0");
        else swprintf_s(count, L"%u/%u", find_index_ + 1,
                        static_cast<uint32_t>(find_matches_.size()));
        count_text = count;
        field.trailing_badge = count_text;
    }
    find_painter_.DrawTextField(field);
}

void QuickPreviewWindow::DrawTextPreview(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                          ID2D1SolidColorBrush* text_brush) {
    if (!text_layout_.get() || !dc) return;
    const float pad = 20.0f * scale_;
    const float origin_x = TextOriginX();
    const float origin_y = content.top + pad - text_scroll_;
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(text_layout_->GetMetrics(&metrics))) {
        // Leave room below the last line for the status pill (DrawTextStatus).
        const float view = (std::max)(1.0f, content.bottom - content.top - pad - 40.0f * scale_);
        const float max_scroll = (std::max)(0.0f, metrics.height - view);
        text_scroll_ = std::clamp(text_scroll_, 0.0f, max_scroll);
    }
    if (find_open_ && !find_query_.empty() && !find_matches_.empty()) {
        ComPtr<ID2D1SolidColorBrush> match_brush;
        ComPtr<ID2D1SolidColorBrush> current_brush;
        dc->CreateSolidColorBrush(D2D1::ColorF(0xC19C00, 0.28f), &match_brush);
        dc->CreateSolidColorBrush(D2D1::ColorF(0xCA5010, 0.40f), &current_brush);
        const uint32_t length = static_cast<uint32_t>(find_query_.size());
        for (size_t i = 0; i < find_matches_.size(); ++i) {
            FillHitRange(dc, text_layout_.get(), find_matches_[i], length, origin_x, origin_y,
                         i == find_index_ ? current_brush.get() : match_brush.get());
        }
    }
    const uint32_t a = (std::min)(sel_anchor_, sel_focus_);
    const uint32_t b = (std::max)(sel_anchor_, sel_focus_);
    if (b > a) {
        ComPtr<ID2D1SolidColorBrush> sel;
        dc->CreateSolidColorBrush(D2D1::ColorF(0x0078D4, 0.35f), &sel);
        FillHitRange(dc, text_layout_.get(), a, b - a, origin_x, origin_y, sel.get());
    }
    DrawLineNumbers(dc, content, origin_y);
    dc->DrawTextLayout(D2D1::Point2F(origin_x, origin_y), text_layout_.get(), text_brush,
                       D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

// Status pill centred at the bottom of the text view, as in the preview mockup:
//   C++  |  UTF-8  |  245 lines \x00B7 8.4 KB
void QuickPreviewWindow::DrawTextStatus(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                        bool hex, uint32_t bytes_read, bool truncated,
                                        uint32_t encoding, ID2D1SolidColorBrush* text_brush) {
    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (!dc || !format || !compositor_.DwriteFactory()) return;
    std::vector<std::wstring> parts;
    if (hex) {
        parts.push_back(luling::l10n::Pick(L"\x5341\x516D\x8FDB\x5236", L"Hex"));
    } else {
        parts.push_back(!syntax_language_.empty() ? syntax_language_
                                                  : std::wstring(luling::l10n::Pick(L"\x7EAF\x6587\x672C", L"Plain text")));
        std::wstring name;
        switch (static_cast<ipc::PreviewTextEncoding>(encoding)) {
        case ipc::PreviewTextEncoding::Utf8: name = L"UTF-8"; break;
        case ipc::PreviewTextEncoding::Utf8Bom: name = L"UTF-8 BOM"; break;
        case ipc::PreviewTextEncoding::Utf16Le: name = L"UTF-16 LE"; break;
        case ipc::PreviewTextEncoding::Utf16Be: name = L"UTF-16 BE"; break;
        case ipc::PreviewTextEncoding::Ansi:
            switch (GetACP()) {
            case 936: name = L"GBK"; break;
            case 950: name = L"Big5"; break;
            case 932: name = L"Shift-JIS"; break;
            case 949: name = L"EUC-KR"; break;
            case 1252: name = L"Windows-1252"; break;
            default: name = L"ANSI " + std::to_wstring(GetACP()); break;
            }
            break;
        default: break;
        }
        if (!name.empty()) parts.push_back(std::move(name));
    }
    std::wstring size = hex ? std::wstring() : std::to_wstring(text_line_count_) +
        (luling::l10n::Pick(L" \x884C \x00B7 ", L" lines \x00B7 "));
    size += luling::format::ByteSize(bytes_read, true);
    if (truncated) size += luling::l10n::Pick(L" \x00B7 \x5DF2\x622A\x65AD", L" \x00B7 truncated");
    parts.push_back(std::move(size));
    DrawStatusPill(dc, content, parts, text_brush);
}

void QuickPreviewWindow::DrawStatusPill(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                        const std::vector<std::wstring>& parts,
                                        ID2D1SolidColorBrush* text_brush) {
    IDWriteTextFormat* format = compositor_.SmallFormat();
    if (!dc || !format || !compositor_.DwriteFactory() || parts.empty()) return;
    std::wstring label;
    std::vector<UINT32> separators;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) { separators.push_back(static_cast<UINT32>(label.size() + 2)); label += L"   |   "; }
        label += parts[i];
    }
    const auto text_alignment = format->GetTextAlignment();
    const auto paragraph_alignment = format->GetParagraphAlignment();
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    ComPtr<IDWriteTextLayout> layout;
    compositor_.DwriteFactory()->CreateTextLayout(label.data(), static_cast<UINT32>(label.size()),
        format, 4000.0f, 100.0f, &layout);
    format->SetTextAlignment(text_alignment);
    format->SetParagraphAlignment(paragraph_alignment);
    if (!layout.get()) return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<ID2D1SolidColorBrush> dim, fill, line;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x777777) : D2D1::ColorF(0xA0A0A0), &dim);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x2B2B2B, 0.94f) : D2D1::ColorF(0xFFFFFF, 0.96f), &fill);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x454545) : D2D1::ColorF(0xD0D0D0), &line);
    if (dim.get())
        for (const UINT32 at : separators) layout->SetDrawingEffect(dim.get(), DWRITE_TEXT_RANGE{at + 1, 1});
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return;
    const float pill_h = 26.0f * scale_;
    const float pill_w = (std::min)(metrics.width + 28.0f * scale_, content.right - content.left - 16.0f * scale_);
    const float cx = (content.left + content.right) * 0.5f;
    const D2D1_RECT_F pill = D2D1::RectF(cx - pill_w * 0.5f, content.bottom - pill_h - 10.0f * scale_,
                                         cx + pill_w * 0.5f, content.bottom - 10.0f * scale_);
    const auto rounded = D2D1::RoundedRect(pill, pill_h * 0.5f, pill_h * 0.5f);
    if (fill.get()) dc->FillRoundedRectangle(rounded, fill.get());
    if (line.get()) dc->DrawRoundedRectangle(rounded, line.get(), 1.0f);
    dc->PushAxisAlignedClip(pill, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->DrawTextLayout(D2D1::Point2F(cx - metrics.width * 0.5f,
                                     (pill.top + pill.bottom) * 0.5f - metrics.height * 0.5f),
                       layout.get(), text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    dc->PopAxisAlignedClip();
}

// Enter / double-click on a row of a folder listing opens that item.
bool QuickPreviewWindow::OpenListingSelection() {
    if (!owner_ || !archive_.IsFolderListing()) return false;
    const std::wstring relative = archive_.SelectedPath();
    if (relative.empty()) return false;
    if (command_message_) {
        const auto target = ActionTarget();
        PostAction(QuickPreviewAction::Open, &target);
        return true;
    }
    if (!open_message_) return false;
    open_path_ = item_.path;
    if (!open_path_.empty() && open_path_.back() != L'\\') open_path_ += L'\\';
    open_path_ += relative;
    PostMessageW(owner_, open_message_, 1, 0);
    return true;
}

float QuickPreviewWindow::TextOriginX() const noexcept {
    return 20.0f * scale_ + text_gutter_;
}

void QuickPreviewWindow::DrawLineNumbers(ID2D1DeviceContext* dc, const D2D1_RECT_F& content,
                                         float origin_y) {
    if (text_gutter_ <= 0.0f || line_starts_.empty() || !gutter_format_.get() ||
        !text_layout_.get())
        return;
    const float pad = 20.0f * scale_;
    const float right = pad + text_gutter_ - 12.0f * scale_;
    ComPtr<ID2D1SolidColorBrush> number_brush, rule_brush;
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0x858585) : D2D1::ColorF(0x9A9A9A), &number_brush);
    dc->CreateSolidColorBrush(dark_ ? D2D1::ColorF(0xFFFFFF, 0.08f) : D2D1::ColorF(0x000000, 0.08f),
                              &rule_brush);
    if (!number_brush.get()) return;
    if (rule_brush.get()) {
        const float x = right + 6.0f * scale_;
        dc->FillRectangle(D2D1::RectF(x, content.top, x + 1.0f, content.bottom), rule_brush.get());
    }
    auto line_y = [&](size_t line) {
        FLOAT x = 0, y = 0;
        DWRITE_HIT_TEST_METRICS metrics{};
        text_layout_->HitTestTextPosition(line_starts_[line], FALSE, &x, &y, &metrics);
        return std::pair<float, float>{y, metrics.height};
    };
    // First line whose top is inside the view (layout coordinates grow monotonically).
    const float view_top = content.top - origin_y;
    size_t lo = 0, hi = line_starts_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const auto [y, h] = line_y(mid);
        if (y + h < view_top) lo = mid + 1;
        else hi = mid;
    }
    for (size_t line = lo; line < line_starts_.size(); ++line) {
        const auto [y, h] = line_y(line);
        const float top = origin_y + y;
        if (top > content.bottom) break;
        const std::wstring label = std::to_wstring(line + 1);
        dc->DrawTextW(label.data(), static_cast<UINT32>(label.size()), gutter_format_.get(),
                      D2D1::RectF(pad * 0.25f, top, right, top + (std::max)(h, 1.0f)),
                      number_brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

bool QuickPreviewWindow::ClientPoint(LPARAM lparam, POINT& out) const {
    out = POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    return true;
}

HCURSOR QuickPreviewWindow::ContentCursor(POINT client) const {
    if (OverCodecButton(client)) return LoadCursorW(nullptr, IDC_HAND);
    if (HasPlayback() && client.y >= PlaybackRect().top)
        return LoadCursorW(nullptr, IDC_HAND);
    if (find_open_ && FindBarHeight() > 0.0f) {
        const D2D1_RECT_F field = FindFieldRect();
        if (client.x >= field.left && client.x < field.right &&
            client.y >= field.top && client.y < field.bottom)
            return LoadCursorW(nullptr, IDC_IBEAM);
    }
    const D2D1_RECT_F content = ContentRect();
    if (client.x < content.left || client.x >= content.right ||
        client.y < content.top || client.y >= content.bottom)
        return LoadCursorW(nullptr, IDC_ARROW);
    if (native_kind_ == NativeKind::Markdown &&
        markdown_.IsClickable(static_cast<float>(client.x), static_cast<float>(client.y)))
        return LoadCursorW(nullptr, IDC_HAND);
    if (native_kind_ == NativeKind::Markdown) {
        // Links open with Ctrl+click; a plain click still selects text.
        const bool link = (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            !markdown_.LinkAt(static_cast<float>(client.x), static_cast<float>(client.y)).empty();
        return LoadCursorW(nullptr, link ? IDC_HAND : IDC_IBEAM);
    }
    if (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex)
        return LoadCursorW(nullptr, IDC_IBEAM);
    if (native_kind_ == NativeKind::Table) return LoadCursorW(nullptr, IDC_ARROW);
    if (native_kind_ == NativeKind::Pages)
        return LoadCursorW(nullptr, panning_ ? IDC_SIZEALL : IDC_ARROW);
    if (panning_ || CanPanImage()) return LoadCursorW(nullptr, IDC_SIZEALL);
    if (video_panning_ || CanPanVideo()) return LoadCursorW(nullptr, IDC_SIZEALL);
    return LoadCursorW(nullptr, IDC_ARROW);
}

void QuickPreviewWindow::DrawChromeButtons(ID2D1DeviceContext* dc,
                                           ID2D1SolidColorBrush* text_brush) {
    if (!dc || !text_brush) return;
    ComPtr<ID2D1SolidColorBrush> hover_brush;
    D2D1_COLOR_F hover_color = text_brush->GetColor();
    hover_color.a = dark_ ? 0.10f : 0.08f;
    dc->CreateSolidColorBrush(hover_color, &hover_brush);
    if (ShowsCopyButton()) {
        const D2D1_RECT_F rect = ChromeButtonRect(ChromeButton::Copy);
        const float inset = 6.0f * scale_;
        const float radius = 4.0f * scale_;
        const D2D1_ROUNDED_RECT pill{
            D2D1::RectF(rect.left + inset, rect.top + inset,
                        rect.right - inset, rect.bottom - inset), radius, radius};
        ComPtr<ID2D1SolidColorBrush> chip;
        D2D1_COLOR_F chip_color = text_brush->GetColor();
        const bool hovered = chrome_hover_ == ChromeButton::Copy;
        chip_color.a = dark_ ? (hovered ? 0.16f : 0.08f) : (hovered ? 0.14f : 0.06f);
        dc->CreateSolidColorBrush(chip_color, &chip);
        if (chip.get()) dc->FillRoundedRectangle(pill, chip.get());
        if (IDWriteTextFormat* format = compositor_.SmallFormat()) {
            const wchar_t* label = copy_feedback_
                ? luling::l10n::Pick(L"\x5DF2\x590D\x5236", L"Copied")
                : luling::l10n::Pick(L"\x590D\x5236", L"Copy");
            const auto text_alignment = format->GetTextAlignment();
            const auto paragraph_alignment = format->GetParagraphAlignment();
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawTextW(label, static_cast<UINT32>(wcslen(label)), format, pill.rect,
                          text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                          DWRITE_MEASURING_MODE_NATURAL);
            format->SetTextAlignment(text_alignment);
            format->SetParagraphAlignment(paragraph_alignment);
        }
    }
    // "用照片打开" — the shell's default app for this extension, with the
    // position counter ("6 / 12") between it and the arrows.
    if (!open_label_.empty()) {
        const D2D1_RECT_F rect = OpenButtonRect();
        const float radius = 5.0f * scale_;
        ComPtr<ID2D1SolidColorBrush> chip;
        D2D1_COLOR_F chip_color = text_brush->GetColor();
        chip_color.a = dark_ ? (chrome_hover_ == ChromeButton::Open ? 0.14f : 0.06f)
                             : (chrome_hover_ == ChromeButton::Open ? 0.10f : 0.04f);
        dc->CreateSolidColorBrush(chip_color, &chip);
        if (chip.get())
            dc->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), chip.get());
        const float glyph = 20.0f * scale_;
        const float gap = 6.0f * scale_;
        const float chevron = 12.0f * scale_;
        const float label_w = std::min(TextWidthDip(open_label_),
                                       rect.right - rect.left - glyph - gap - chevron - 16.0f * scale_);
        const float start = rect.left + (rect.right - rect.left - glyph - gap - label_w - chevron) * 0.5f;
        const OpenWithIconCache::Badge app_icon =
            open_icons_.AppIconFor(item_.name, static_cast<uint32_t>(20.0f * scale_));
        if (app_icon.bitmap) {
            const float top = rect.top + (rect.bottom - rect.top - glyph) * 0.5f;
            dc->DrawBitmap(app_icon.bitmap, D2D1::RectF(start, top, start + glyph, top + glyph),
                           1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else if (!DrawLegacyIcon(dc, compositor_.DwriteFactory(), kOpenGlyph,
                                   D2D1::RectF(start, rect.top, start + glyph, rect.bottom),
                                   text_brush)) {
            if (IDWriteTextFormat* icon = close_format_.get()) {
                dc->DrawTextW(kOpenGlyph, 1, icon,
                              D2D1::RectF(start, rect.top, start + glyph, rect.bottom), text_brush,
                              D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
        }
        if (IDWriteTextFormat* format = compositor_.SmallFormat()) {
            const auto old_alignment = format->GetTextAlignment();
            const auto old_paragraph = format->GetParagraphAlignment();
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawTextW(open_label_.data(), static_cast<UINT32>(open_label_.size()), format,
                          D2D1::RectF(start + glyph + gap, rect.top,
                                      start + glyph + gap + label_w, rect.bottom),
                          text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            format->SetTextAlignment(old_alignment);
            format->SetParagraphAlignment(old_paragraph);
        }
        // The chevron says the button opens a chooser, not just the default.
        const D2D1_RECT_F arrow = D2D1::RectF(rect.right - chevron - 4.0f * scale_, rect.top,
                                              rect.right - 4.0f * scale_, rect.bottom);
        if (!DrawLegacyIcon(dc, compositor_.DwriteFactory(), L"\xE70D", arrow, text_brush)) {
            if (IDWriteTextFormat* icon = close_format_.get()) {
                dc->DrawTextW(L"\xE70D", 1, icon, arrow, text_brush,
                              D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            }
        }
    }
    const D2D1_RECT_F counter = CounterRect();
    if (counter.right > counter.left) {
        wchar_t position[32]{};
        swprintf_s(position, L"%d / %d", item_.position, item_.count);
        if (IDWriteTextFormat* format = compositor_.SmallFormat()) {
            const auto old_alignment = format->GetTextAlignment();
            const auto old_paragraph = format->GetParagraphAlignment();
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawTextW(position, static_cast<UINT32>(wcslen(position)), format, counter,
                          text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
            format->SetTextAlignment(old_alignment);
            format->SetParagraphAlignment(old_paragraph);
        }
    }
    struct Spec {
        ChromeButton button;
        const wchar_t* glyph;
    };
    const Spec specs[] = {
        {ChromeButton::Prev, kPrevGlyph},
        {ChromeButton::Next, kNextGlyph},
        {ChromeButton::More, kMoreGlyph},
    };
    const float inset_x = 3.0f * scale_;
    const float inset_y = 6.0f * scale_;
    const float radius = 4.0f * scale_;
    for (const Spec& spec : specs) {
        const D2D1_RECT_F rect = ChromeButtonRect(spec.button);
        if (chrome_hover_ == spec.button && hover_brush.get()) {
            const D2D1_ROUNDED_RECT pill{
                D2D1::RectF(rect.left + inset_x, rect.top + inset_y,
                            rect.right - inset_x, rect.bottom - inset_y),
                radius, radius};
            dc->FillRoundedRectangle(pill, hover_brush.get());
        }
        if (!DrawLegacyIcon(dc, compositor_.DwriteFactory(), spec.glyph, rect, text_brush))
            dc->DrawTextW(spec.glyph, 1,
                      close_format_.get() ? close_format_.get() : compositor_.IconFormat(), rect,
                      text_brush, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                      DWRITE_MEASURING_MODE_NATURAL);
    }
}

void QuickPreviewWindow::SchedulePreviewDeadline() {
    uint64_t deadline = thumbnails_.TakeRetryDeadline();
    const auto handler = handler_.NextDeadline();
    if (handler && (!deadline || handler < deadline)) deadline = handler;
    if (!hwnd_ || closing_ || !visible()) deadline = 0;
    if (deadline == preview_deadline_) return;
    KillTimer(hwnd_, kPreviewDeadlineTimer);
    preview_deadline_ = 0;
    if (deadline) {
        const auto now = GetTickCount64();
        const auto wait = deadline > now ? deadline - now : 1;
        if (SetTimer(hwnd_, kPreviewDeadlineTimer,
            static_cast<UINT>((std::min)(wait, uint64_t{USER_TIMER_MAXIMUM})), nullptr))
            preview_deadline_ = deadline;
    }
}

void QuickPreviewWindow::Render() {
    thumbnails_.BeginFrame();
    if (!compositor_.Dc()) return;
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        RecreateFormats();
        thumbnails_.SetDeviceContext(compositor_.Dc());
    }
    Resize();
    auto* dc = compositor_.Dc();
    open_icons_.SetDeviceContext(dc);
    open_icons_.SetNotifyWindow(hwnd_);
    dc->BeginDraw();
    const bool high_contrast = [] {
        HIGHCONTRASTW value{sizeof(value)};
        return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) &&
            (value.dwFlags & HCF_HIGHCONTRASTON);
    }();
    const D2D1_COLOR_F background = high_contrast
        ? D2D1::ColorF(GetSysColor(COLOR_WINDOW))
        : dark_ ? D2D1::ColorF(0x151515) : D2D1::ColorF(0xF7F7F7);
    const D2D1_COLOR_F foreground = high_contrast
        ? D2D1::ColorF(GetSysColor(COLOR_WINDOWTEXT))
        : dark_ ? D2D1::ColorF(0xF4F4F4) : D2D1::ColorF(0x202020);
    const D2D1_COLOR_F secondary = high_contrast ? foreground
        : dark_ ? D2D1::ColorF(0xB8B8B8) : D2D1::ColorF(0x606060);
    dc->Clear(background);
    ComPtr<ID2D1SolidColorBrush> text_brush;
    ComPtr<ID2D1SolidColorBrush> secondary_brush;
    dc->CreateSolidColorBrush(foreground, &text_brush);
    dc->CreateSolidColorBrush(secondary, &secondary_brush);
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float pad = 20.0f * scale_;
    const float header = kTitleBarHeight * scale_;
    // Header: name on the first line, type · dimensions · size · modified on
    // the second, right of it the open-with button, the position counter and
    // the chrome buttons.
    float title_right = width - (kCloseButtonWidth + kChromeButtonWidth * 3.0f + 8.0f +
                                 (ShowsCopyButton() ? kCopyButtonWidth + 6.0f : 0.0f)) * scale_;
    // The open-with chip is the leftmost header control, left of the counter and
    // the tree/source toggle; the title must stop before whichever is present.
    if (const D2D1_RECT_F open_rect = OpenButtonRect(); open_rect.right > open_rect.left) {
        title_right = open_rect.left - 8.0f * scale_;
    } else if (markdown_shown_) {
        title_right = MarkdownToggleRect(-1).left - 8.0f * scale_;
    }
    const D2D1_RECT_F title_rect = typography::SnapVerticalBounds(D2D1::RectF(
        pad, 4.0f * scale_, title_right, 26.0f * scale_));
    dc->DrawTextW(item_.name.data(), static_cast<UINT32>(item_.name.size()),
                  compositor_.HeaderFormat(), title_rect, text_brush.get(),
                  D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    if (IDWriteTextFormat* meta_format = compositor_.SmallFormat()) {
        const std::wstring meta = MetaLine();
        if (!meta.empty()) {
            dc->DrawTextW(meta.data(), static_cast<UINT32>(meta.size()), meta_format,
                          D2D1::RectF(pad, 27.0f * scale_, title_right, 46.0f * scale_),
                          secondary_brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                          DWRITE_MEASURING_MODE_NATURAL);
        }
    }
    const D2D1_RECT_F close_rect = D2D1::RectF(width - kCloseButtonWidth * scale_, 0,
                                               width, header);
    if (close_hover_) {
        ComPtr<ID2D1SolidColorBrush> close_background;
        dc->CreateSolidColorBrush(D2D1::ColorF(0xC42B1C), &close_background);
        dc->FillRectangle(close_rect, close_background.get());
    }
    ComPtr<ID2D1SolidColorBrush> close_brush;
    dc->CreateSolidColorBrush(close_hover_ ? D2D1::ColorF(0xFFFFFF) : foreground,
                              &close_brush);
    static constexpr wchar_t close_glyph[] = L"\xE711";
    if (!DrawLegacyIcon(dc, compositor_.DwriteFactory(), close_glyph, close_rect, close_brush.get()))
        dc->DrawTextW(close_glyph, 1,
                  close_format_.get() ? close_format_.get() : compositor_.IconFormat(), close_rect,
                  close_brush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                  DWRITE_MEASURING_MODE_NATURAL);
    DrawChromeButtons(dc, text_brush.get());
    const D2D1_RECT_F notice_rect = D2D1::RectF(0, header + FindBarHeight(), width, height);
    preview_notice_height_ = DrawPreviewNotice(dc, compositor_.DwriteFactory(), compositor_.SmallFormat(),
        preview_notice_, notice_rect, scale_, CurrentTheme().text, CurrentTheme().fill_hover);
    const D2D1_RECT_F full_content = D2D1::RectF(0, header, width, height);
    const bool offline = OfflinePlaceholder();
    const bool use_handler = !offline && !safe_mode_ && !video_.active() &&
        !(item_.attrs & FILE_ATTRIBUTE_DIRECTORY) && PreviewHandlerHost::CanHost(item_.path) &&
        !(kMarkdownRender && SyntaxLanguageName(L"." + ExtensionLabel(item_.path)) == L"Markdown") &&
        !(kTableView && TableFileKind(item_.path) == 2 && !table_handler_) &&
        !(kDocView && IsDocxFile(item_.path) && !doc_handler_) &&
        !(kDocView && ExtensionLabel(item_.path) == L"EPUB");
    handler_.Sync(hwnd_, full_content, item_.path, item_.attrs, generation_, item_.modified,
                  item_.size, dark_, background, foreground, use_handler,
                  handler_immediate_);
    handler_immediate_ = false;
    markdown_shown_ = false;
    toggle_kind_ = ToggleKind::Markdown;
    if (use_handler && kTableView && TableFileKind(item_.path) == 2) {
        // "Table | System" stays available over the Office preview handler.
        markdown_shown_ = true;
        toggle_kind_ = ToggleKind::TableHandler;
    } else if (use_handler && kDocView && IsDocxFile(item_.path)) {
        markdown_shown_ = true;
        toggle_kind_ = ToggleKind::DocHandler;
    }
    codec_store_rect_ = codec_open_rect_ = codec_pack_rect_ = D2D1_RECT_F{};
    std::wstring status;
    if (offline) {
        native_kind_ = NativeKind::None;
        status = luling::l10n::Get(luling::l10n::StringId::PreviewCloudOnly);
    } else if (video_.active()) {
        native_kind_ = NativeKind::None;
        const auto video = video_.Snapshot();
        const bool audio = VideoPreview::IsAudio(item_.path);
        const bool codec_card = CodecCardVisible(video);
        LayoutVideo(dc, video, !audio && !codec_card && video.ready && SUCCEEDED(video.error) &&
                                   visible());
        if (codec_card) DrawCodecCard(dc, ContentRect(), video, text_brush.get());
        if (audio && SUCCEEDED(video.error))
            DrawAudio(dc, video, text_brush.get(), secondary_brush.get());
        if (codec_card) {
            // The card explains the missing picture.
        } else if (FAILED(video.error))
            status = video.unsupported_audio
                ? l10n::HantText(l10n::Pick(L"系统无法解码此音频，请用默认应用打开。",
                                          L"Windows cannot decode this audio. Open it in the default app."))
                : luling::l10n::Get(luling::l10n::StringId::PreviewCannotRender);
        else if (!video.ready && !audio)
            status = luling::l10n::Get(luling::l10n::StringId::PreviewLoading);
    } else if (use_handler) {
        native_kind_ = NativeKind::None;
        const auto state = handler_.state();
        if (state == PreviewHandlerHost::State::Loading ||
            state == PreviewHandlerHost::State::Idle)
            status = luling::l10n::Get(luling::l10n::StringId::PreviewLoading);
        else if (state == PreviewHandlerHost::State::Failed)
            status = luling::l10n::Get(luling::l10n::StringId::PreviewHostUnavailable);
        if (markdown_shown_) DrawMarkdownToggle(dc, text_brush.get());
    } else if (pages_count_ > 1) {
        native_kind_ = NativeKind::Pages;
        DrawPages(dc, text_brush.get(), secondary_brush.get());
    } else {
        std::wstring text;
        std::wstring error;
        preview::Integrity integrity;
        bool truncated = false;
        uint32_t bytes_read = 0;
        uint32_t reported_frame_count = 1, reported_delay_ms = 0, reported_loop_count = 0;
        uint32_t text_encoding = 0;
        const D2D1_RECT_F content = ContentRect();
        const bool folder = (item_.attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        // A folder listing does not depend on the window size.
        const uint32_t want = folder ? ipc::kPreviewDefaultPixelSize : RequestedPixelSize(content);
        const bool icon = kImageExtras && IsIconFile(item_.path);
        D2D1_RECT_F draw = ImageDest(content);
        if (icon && image_fit_ && decoded_w_ && decoded_h_) {
            // Icons show their real pixels (shrunk only when they do not fit).
            const float dw = static_cast<float>(decoded_w_), dh = static_cast<float>(decoded_h_);
            const float fit = (std::min)({(content.right - content.left) / dw, (content.bottom - content.top) / dh, 1.0f});
            const float cx = std::round((content.left + content.right - dw * fit) * 0.5f);
            const float cy = std::round((content.top + content.bottom - dh * fit) * 0.5f);
            draw = D2D1::RectF(cx, cy, cx + std::round(dw * fit), cy + std::round(dh * fit));
        }
        const bool icon_unsized = icon && image_fit_ && (!decoded_w_ || !decoded_h_);
        dc->PushAxisAlignedClip(content, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        if (table_.TakePendingSheetRequest(sheet_request_)) {
            preview_text_.clear();
            find_matches_.clear();
            preview_notice_.clear();
        }
        const uint32_t requested = TableFileKind(item_.path) == 2 ? sheet_request_ :
            icon ? icon_request_ : waiting_for_frame_ ? requested_frame_ : frame_index_;
        if (icon && !icon_sizes_.empty() && draw.right - draw.left < 8192.0f && draw.bottom - draw.top < 8192.0f) {
            // Transparent icon pixels sit on a checkerboard (mockup ⑥).
            ComPtr<ID2D1SolidColorBrush> cell;
            dc->CreateSolidColorBrush(D2D1::ColorF(0x808080, 0.18f), &cell);
            const float step = 8.0f * scale_;
            int row = 0;
            if (cell.get())
                for (float y = draw.top; y < draw.bottom; y += step, ++row)
                    for (float x = draw.left + (row % 2 ? step : 0.0f); x < draw.right; x += step * 2.0f)
                        dc->FillRectangle(D2D1::RectF(x, y, (std::min)(x + step, draw.right),
                                                      (std::min)(y + step, draw.bottom)), cell.get());
        }
        // Pictures draw over their matte (checkerboard behind transparency).
        if (!icon && decoded_w_ && decoded_h_) DrawImageBackdrop(dc, draw);
        auto draw_at = [&](uint32_t pixels, uint32_t frame) {
            if (image_turns_ == 0 ||
                (native_kind_ != NativeKind::Bitmap && native_kind_ != NativeKind::None)) {
                return thumbnails_.Draw(dc, draw, item_.path, item_.attrs, pixels,
                    generation_, item_.modified, item_.size, 1.0f, &text, &truncated,
                    &bytes_read, true, &error, nullptr, nullptr, nullptr, nullptr,
                    frame, &reported_frame_count, &reported_delay_ms, &reported_loop_count,
                    &decoded_w_, &decoded_h_, &source_w_, &source_h_, nullptr, &text_encoding,
                    false, nullptr, nullptr, &integrity);
            }
            // A quarter turn round the picture's centre: draw into the swapped
            // box and rotate it back into `draw`, so fit, zoom and pan follow.
            D2D1_MATRIX_3X2_F saved{};
            dc->GetTransform(&saved);
            const D2D1_POINT_2F centre = D2D1::Point2F((draw.left + draw.right) * 0.5f,
                                                       (draw.top + draw.bottom) * 0.5f);
            const float half_w = (draw.right - draw.left) * 0.5f;
            const float half_h = (draw.bottom - draw.top) * 0.5f;
            dc->SetTransform(D2D1::Matrix3x2F::Rotation(90.0f * image_turns_, centre) * saved);
            const D2D1_RECT_F target = D2D1::RectF(centre.x - half_h, centre.y - half_w,
                                                   centre.x + half_h, centre.y + half_w);
            const auto turned = thumbnails_.Draw(dc, target, item_.path, item_.attrs, pixels,
                generation_, item_.modified, item_.size, 1.0f, &text, &truncated,
                &bytes_read, true, &error, nullptr, nullptr, nullptr, nullptr,
                frame, &reported_frame_count, &reported_delay_ms, &reported_loop_count,
                &decoded_w_, &decoded_h_, &source_w_, &source_h_, nullptr, &text_encoding,
                false, nullptr, nullptr, &integrity);
            dc->SetTransform(saved);
            return turned;
        };
        PreviewDrawResult result = draw_at(want, requested);
        if (result == PreviewDrawResult::Pending && preview_pixels_ != 0 &&
            preview_pixels_ != want) {
            result = draw_at(preview_pixels_, requested);
        } else if (result != PreviewDrawResult::Pending) {
            preview_pixels_ = want;
        }
        if (folder && result == PreviewDrawResult::Archive &&
            text.rfind(L"PULSEARC\t1\tDIR\t-\t2", 0) == 0) {
            // The quick pass ran out of time: the full count replaces it when ready.
            std::wstring full_text, full_error;
            bool full_truncated = false;
            uint32_t full_bytes = 0;
            if (thumbnails_.Draw(dc, draw, item_.path, item_.attrs, want, generation_,
                    item_.modified, item_.size, 1.0f, &full_text, &full_truncated, &full_bytes,
                    true, &full_error, nullptr, nullptr, nullptr, nullptr, 1,
                    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, nullptr, false, nullptr, nullptr, &integrity) ==
                PreviewDrawResult::Archive) {
                text = std::move(full_text);
                error = std::move(full_error);
            }
        }
        if (result != PreviewDrawResult::Pending) {
            // The picture cards explain these failures themselves.
            const bool codec_card = kImageExtras && result == PreviewDrawResult::Failed &&
                (error.rfind(L"image-codec-missing:", 0) == 0 || error == L"image-pack-missing");
            const auto notice = codec_card ? std::wstring{} : PreviewNotice(error, text, integrity);
            if (notice != preview_notice_) {
                preview_notice_ = notice;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        bool committed_frame = false;
        if (result == PreviewDrawResult::Bitmap && reported_frame_count > 1 &&
            reported_delay_ms == 0) {
            // Paged document (PDF): switch to the page reader from the next frame.
            pages_count_ = reported_frame_count;
            page_aspect_.assign(pages_count_, 0.0f);
            if (source_w_ && source_h_)
                page_aspect_[0] = static_cast<float>(source_h_) / static_cast<float>(source_w_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (result == PreviewDrawResult::Bitmap && reported_frame_count > 1 &&
                   (kImageExtras || ExtensionLabel(item_.path) == L"GIF")) {
            if (frame_count_ <= 1) InvalidateRect(hwnd_, nullptr, FALSE);
            frame_count_ = reported_frame_count;
            frame_delay_ms_ = std::clamp(reported_delay_ms, 20u, 2000u);
            loop_count_ = reported_loop_count;
            if (waiting_for_frame_) {
                frame_index_ = requested_frame_;
                waiting_for_frame_ = false;
                committed_frame = true;
            }
            if (!animation_started_) {
                animation_started_ = true;
                animation_active_ = true;
                committed_frame = true;
            }
            if (committed_frame && animation_active_ && !playback_scrub_pending_)
                SetTimer(hwnd_, kAnimationTimer, frame_delay_ms_, nullptr);
        } else if (result == PreviewDrawResult::Pending && animation_started_) {
            std::wstring ignored_text;
            std::wstring ignored_error;
            bool ignored_truncated = false;
            uint32_t ignored_bytes = 0;
            uint32_t ignored_count = 1, ignored_delay = 0, ignored_loop = 0;
            const auto fallback = thumbnails_.Draw(dc, draw, item_.path, item_.attrs,
                preview_pixels_ ? preview_pixels_ : want, generation_,
                item_.modified, item_.size, 1.0f, &ignored_text, &ignored_truncated,
                &ignored_bytes, true, &ignored_error, nullptr, nullptr, nullptr, nullptr,
                frame_index_, &ignored_count, &ignored_delay, &ignored_loop,
                &decoded_w_, &decoded_h_, &source_w_, &source_h_);
            // Keep the displayed frame's HUD while the next frame is decoding.
            if (fallback == PreviewDrawResult::Bitmap) result = fallback;
            status.clear();
        } else if (result != PreviewDrawResult::Pending) {
            ResetAnimation();
        }
        if (result == PreviewDrawResult::Bitmap) {
            native_kind_ = NativeKind::Bitmap;
            if (icon) {
                ParseIconSizes(text);
                DrawIconSizes(dc, content, text_brush.get());
                if (icon_unsized) InvalidateRect(hwnd_, nullptr, FALSE);  // lay out at the real size
            }
        } else if (result == PreviewDrawResult::Archive &&
                   archive_.SetPayload(text, item_.name, item_.size)) {
            native_kind_ = NativeKind::Archive;
            archive_.Draw(dc, &compositor_, content, CurrentTheme(), scale_, true);
        } else if (result == PreviewDrawResult::Markdown && markdown_.SetPayload(text, item_.path) &&
                   (!markdown_.DocFormat().empty() ||
                    !(markdown_.IsNotebook() ? notebook_source_ : markdown_source_))) {
            // DOCX / EPUB have no source view; DOCX may switch to the handler.
            const bool document = !markdown_.DocFormat().empty();
            markdown_shown_ = !document ||
                (markdown_.DocFormat() == L"docx" && PreviewHandlerHost::CanHost(item_.path));
            native_kind_ = NativeKind::Markdown;
            if (markdown_.IsNotebook()) toggle_kind_ = ToggleKind::NotebookSource;
            if (document) toggle_kind_ = ToggleKind::DocHandler;
            if (preview_text_ != markdown_.PlainText()) {
                preview_text_ = markdown_.PlainText();
                text_layout_.reset();
                sel_anchor_ = sel_focus_ = 0;
                if (find_open_) UpdateFindMatches();
            }
            std::vector<MarkdownView::Highlight> marks;
            marks.reserve(find_matches_.size());
            for (size_t i = 0; i < find_matches_.size(); ++i)
                marks.push_back({find_matches_[i], static_cast<uint32_t>(find_query_.size()),
                                 i == find_index_});
            if (markdown_.Draw(dc, compositor_.DwriteFactory(), content, CurrentTheme(), dark_,
                               scale_, &thumbnails_, generation_,
                               (std::min)(sel_anchor_, sel_focus_),
                               (std::max)(sel_anchor_, sel_focus_), marks))
                InvalidateRect(hwnd_, nullptr, FALSE);
            if (markdown_.IsNotebook()) {
                // Jupyter · Python 3  |  12 个单元格  |  86 KB
                std::vector<std::wstring> parts;
                parts.push_back(markdown_.NotebookKernel().empty()
                                    ? std::wstring(L"Jupyter")
                                    : L"Jupyter \x00B7 " + markdown_.NotebookKernel());
                parts.push_back(std::to_wstring(markdown_.NotebookCells()) +
                                (luling::l10n::Pick(L" \x4E2A\x5355\x5143\x683C", L" cells")));
                std::wstring size = luling::format::ByteSize(bytes_read, true);
                if (truncated) size += luling::l10n::Pick(L" \x00B7 \x5DF2\x622A\x65AD", L" \x00B7 truncated");
                parts.push_back(std::move(size));
                DrawStatusPill(dc, content, parts, text_brush.get());
            } else if (markdown_.DocFormat() == L"docx") {
                // Word 文档  |  约 1,240 字  |  38 KB
                const bool zh = luling::l10n::IsChinese();
                std::vector<std::wstring> parts;
                parts.push_back(luling::l10n::Pick(L"Word \x6587\x6863", L"Word document"));
                const std::wstring words = GroupedNumber(markdown_.WordCount());
                parts.push_back(zh ? luling::l10n::Cn(L"\x7EA6 ") + words + luling::l10n::Cn(L" \x5B57") : L"about " + words + L" words");
                std::wstring size = luling::format::ByteSize(item_.size ? item_.size : bytes_read, true);
                if (truncated) size += luling::l10n::Pick(L" \x00B7 \x5DF2\x622A\x65AD", L" \x00B7 truncated");
                parts.push_back(std::move(size));
                DrawStatusPill(dc, content, parts, text_brush.get());
            } else if (markdown_.DocFormat() == L"epub") {
                // EPUB  |  第 1 / 27 章  |  author
                const bool zh = luling::l10n::IsChinese();
                std::vector<std::wstring> parts;
                parts.push_back(L"EPUB");
                const int count = (std::max)(1, markdown_.SectionCount());
                const std::wstring at = std::to_wstring(markdown_.SectionIndex() + 1) + L" / " + std::to_wstring(count);
                parts.push_back(zh ? luling::l10n::Cn(L"\x7B2C ") + at + luling::l10n::Cn(L" \x7AE0") : L"Chapter " + at);
                std::wstring who = markdown_.DocAuthor();
                if (who.size() > 40) who = who.substr(0, 39) + L"\x2026";
                if (!who.empty()) parts.push_back(std::move(who));
                if (truncated) parts.push_back(luling::l10n::Pick(L"\x5DF2\x622A\x65AD", L"truncated"));
                DrawStatusPill(dc, content, parts, text_brush.get());
            }
        } else if (result == PreviewDrawResult::Markdown && markdown_.SetPayload(text, item_.path)) {
            // Source mode: the highlighted text view with its status pill.
            markdown_shown_ = true;
            if (markdown_.IsNotebook()) toggle_kind_ = ToggleKind::NotebookSource;
            native_kind_ = NativeKind::Text;
            const bool changed = preview_text_ != markdown_.Source();
            EnsureTextLayout(markdown_.Source(), false, (std::max)(1.0f, width - pad * 2.0f));
            if (changed) {
                sel_anchor_ = sel_focus_ = 0;
                if (find_open_) UpdateFindMatches();
            }
            DrawTextPreview(dc, content, text_brush.get());
            DrawTextStatus(dc, content, false, bytes_read, truncated, text_encoding,
                           text_brush.get());
        } else if (result == PreviewDrawResult::Table && table_.SetPayload(text)) {
            const bool sheet = table_.IsSpreadsheet();
            if (kTableView && (!sheet || PreviewHandlerHost::CanHost(item_.path))) {
                markdown_shown_ = true;
                toggle_kind_ = sheet ? ToggleKind::TableHandler : ToggleKind::TableSource;
            }
            if (!sheet && (!kTableView || table_source_)) {
                // Source mode: the text view with its status pill.
                native_kind_ = NativeKind::Text;
                const bool changed = preview_text_ != table_.Source();
                EnsureTextLayout(table_.Source(), false, (std::max)(1.0f, width - pad * 2.0f));
                if (changed) {
                    sel_anchor_ = sel_focus_ = 0;
                    if (find_open_) UpdateFindMatches();
                }
                DrawTextPreview(dc, content, text_brush.get());
                DrawTextStatus(dc, content, false, bytes_read, truncated, text_encoding,
                               text_brush.get());
            } else {
                native_kind_ = NativeKind::Table;
                if (preview_text_ != table_.PlainText()) {
                    preview_text_ = table_.PlainText();
                    text_layout_.reset();
                    sel_anchor_ = sel_focus_ = 0;
                    if (find_open_) UpdateFindMatches();
                }
                std::vector<TableView::Highlight> marks;
                marks.reserve(find_matches_.size());
                for (size_t i = 0; i < find_matches_.size(); ++i)
                    marks.push_back({find_matches_[i], static_cast<uint32_t>(find_query_.size()),
                                     i == find_index_});
                table_.Draw(dc, compositor_.DwriteFactory(), content, CurrentTheme(), dark_, scale_,
                            background, marks);
                const auto parts = table_.StatusParts(text_encoding);
                if (!parts.empty()) DrawStatusPill(dc, content, parts, text_brush.get());
            }
        } else if (result == PreviewDrawResult::Tree && tree_.SetPayload(text)) {
            // The Tree | Source pill only when there is a tree to go back to.
            markdown_shown_ = kTreeView && !tree_.HasError();
            toggle_kind_ = ToggleKind::TreeSource;
            if (!kTreeView || tree_.HasError() || tree_source_) {
                native_kind_ = NativeKind::Text;
                const bool changed = preview_text_ != tree_.Source();
                EnsureTextLayout(tree_.Source(), false, (std::max)(1.0f, width - pad * 2.0f));
                if (changed) {
                    sel_anchor_ = sel_focus_ = 0;
                    if (find_open_) UpdateFindMatches();
                }
                DrawTextPreview(dc, content, text_brush.get());
                if (tree_.HasError() && kTreeView) {
                    // Malformed: the source, with where it broke in the pill.
                    std::vector<std::wstring> parts;
                    parts.push_back(tree_.IsXml() ? L"XML" : L"JSON");
                    parts.push_back(tree_.ErrorText());
                    parts.push_back(luling::format::ByteSize(bytes_read, true));
                    DrawStatusPill(dc, content, parts, text_brush.get());
                } else {
                    DrawTextStatus(dc, content, false, bytes_read, truncated, text_encoding,
                                   text_brush.get());
                }
            } else {
                native_kind_ = NativeKind::Tree;
                if (preview_text_ != tree_.PlainText()) {
                    preview_text_ = tree_.PlainText();
                    text_layout_.reset();
                    sel_anchor_ = sel_focus_ = 0;
                    if (find_open_) UpdateFindMatches();
                }
                std::vector<TreeView::Highlight> marks;
                marks.reserve(find_matches_.size());
                for (size_t i = 0; i < find_matches_.size(); ++i)
                    marks.push_back({find_matches_[i], static_cast<uint32_t>(find_query_.size()),
                                     i == find_index_});
                tree_.Draw(dc, compositor_.DwriteFactory(), content, CurrentTheme(), dark_, scale_, marks);
                auto parts = tree_.StatusParts();
                std::wstring size = luling::format::ByteSize(bytes_read, true);
                if (truncated) size += luling::l10n::Pick(L" \x00B7 \x5DF2\x622A\x65AD", L" \x00B7 truncated");
                parts.push_back(std::move(size));
                DrawStatusPill(dc, content, parts, text_brush.get());
            }
        } else if (result == PreviewDrawResult::Text || result == PreviewDrawResult::Hex ||
                   result == PreviewDrawResult::Archive) {
            native_kind_ = result == PreviewDrawResult::Hex ? NativeKind::Hex : NativeKind::Text;
            EnsureTextLayout(text, result == PreviewDrawResult::Hex,
                             (std::max)(1.0f, width - pad * 2.0f));
            DrawTextPreview(dc, content, text_brush.get());
            DrawTextStatus(dc, content, result == PreviewDrawResult::Hex, bytes_read, truncated,
                           text_encoding, text_brush.get());
        } else if (TableFileKind(item_.path) == 2 && table_.HasData() &&
                   (result == PreviewDrawResult::Pending || result == PreviewDrawResult::Failed)) {
            native_kind_ = NativeKind::Table;
            if (result == PreviewDrawResult::Failed) table_.FailPendingSheetRequest(sheet_request_);
            table_.Draw(dc, compositor_.DwriteFactory(), content, CurrentTheme(), dark_, scale_, background, {});
        } else if (result == PreviewDrawResult::Pending && !animation_started_) {
            native_kind_ = NativeKind::None;
            status = luling::l10n::Get(luling::l10n::StringId::PreviewLoading);
        } else if (result == PreviewDrawResult::Failed && kImageExtras &&
                   error.rfind(L"image-codec-missing:", 0) == 0) {
            native_kind_ = NativeKind::None;
            DrawImageCodecCard(dc, content, error.substr(20), text_brush.get());
        } else if (result == PreviewDrawResult::Failed && kImageExtras && error == L"image-pack-missing") {
            native_kind_ = NativeKind::None;
            DrawImageCodecCard(dc, content, L"pack", text_brush.get());
        } else if (result == PreviewDrawResult::Failed) {
            native_kind_ = NativeKind::None;
            status = luling::l10n::Get(error == L"path-unavailable"
                ? luling::l10n::StringId::PreviewFileUnavailable
                : luling::l10n::StringId::PreviewCannotRender);
        }
        dc->PopAxisAlignedClip();
        if (native_kind_ == NativeKind::Bitmap) {
            DrawImageFooter(dc, text_brush.get(), secondary_brush.get());
            DrawInfoDrawer(dc, text_brush.get(), secondary_brush.get());
        }
        if (markdown_shown_) DrawMarkdownToggle(dc, text_brush.get());
        if (FindBarHeight() > 0.0f) {
            DrawFindBar(dc, FindBarRect());
        }
    }
    DrawPlayback(dc, text_brush.get());
    if (!status.empty()) {
        compositor_.TextFormat()->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        compositor_.TextFormat()->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawTextW(status.data(), static_cast<UINT32>(status.size()),
            compositor_.TextFormat(), ContentRect(), secondary_brush.get());
        compositor_.TextFormat()->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        compositor_.TextFormat()->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    SchedulePreviewDeadline();
    const HRESULT result = dc->EndDraw();
    if (result == D2DERR_RECREATE_TARGET || result == DXGI_ERROR_DEVICE_REMOVED ||
        result == DXGI_ERROR_DEVICE_RESET) compositor_.NotifyDeviceLost(result);
    else {
        compositor_.Present();
        if (zoom_pending_) {
            // Start after the first frame of the new item is on screen.
            zoom_pending_ = false;
            if (!compositor_.PlayZoom(static_cast<float>(zoom_origin_.x),
                                      static_cast<float>(zoom_origin_.y), true,
                                      kZoomOpenSeconds))
                compositor_.ClearZoom();
        }
    }
    if (find_open_ && (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                       native_kind_ == NativeKind::Archive || native_kind_ == NativeKind::Markdown ||
                       native_kind_ == NativeKind::Table || native_kind_ == NativeKind::Tree))
        LayoutFindEdit();
    else if (find_edit_)
        ShowWindow(find_edit_, SW_HIDE);
}

LRESULT CALLBACK QuickPreviewWindow::WndProc(HWND hwnd, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<QuickPreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<QuickPreviewWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT QuickPreviewWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_MOUSEMOVE) pointer_tick_ = GetTickCount64();
    switch (message) {
    case kOpenWithMenuMessage:
        ShowOpenWithMenu();
        return 0;
    case WM_NCCALCSIZE:
        if (wparam) return 0;
        break;
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &point);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int border = (std::max)(6, static_cast<int>(8.0f * scale_));
        const bool left = point.x < border;
        const bool right = point.x >= client.right - border;
        const bool top = point.y < border;
        const bool bottom = point.y >= client.bottom - border;
        if (!IsZoomed(hwnd_)) {
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        const int close_left = client.right - static_cast<int>(kCloseButtonWidth * scale_);
        const int chrome_left = close_left - static_cast<int>(
            (kChromeButtonWidth * 3.0f + (ShowsCopyButton() ? kCopyButtonWidth + 6.0f : 0.0f)) * scale_);
        if (point.y >= 0 && point.y < static_cast<int>(kTitleBarHeight * scale_)) {
            if (point.x >= close_left) return HTCLOSE;
            if (point.x >= chrome_left) return HTCLIENT;  // prev / next / more buttons
            // The open-with split button and the position counter sit left of
            // the chrome buttons: they are controls, not title-bar drag area.
            const D2D1_RECT_F open_rect = OpenButtonRect();
            if (open_rect.right > open_rect.left &&
                point.x >= static_cast<LONG>(open_rect.left) &&
                point.x < static_cast<LONG>(open_rect.right))
                return HTCLIENT;
            const D2D1_RECT_F counter_rect = CounterRect();
            if (counter_rect.right > counter_rect.left &&
                point.x >= static_cast<LONG>(counter_rect.left) &&
                point.x < static_cast<LONG>(counter_rect.right))
                return HTCLIENT;
            if (HitMarkdownToggle(point) >= 0) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lparam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            SetCursor(ContentCursor(point));
            return TRUE;
        }
        break;
    }
    case WM_CREATE:
        scale_ = static_cast<float>(luling::compat::WindowDpi(hwnd_)) / 96.0f;
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.RecreateTextFormats(scale_);
        RecreateFormats();
        thumbnails_.SetDeviceContext(compositor_.Dc());
        thumbnails_.SetNotifyWindow(hwnd_);
        markdown_.SetNotifyWindow(hwnd_);
        handler_.SetNotifyWindow(hwnd_);
        return 0;
    case WM_SIZE:
        Resize();
        if (find_open_) LayoutFindEdit();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOVE:
        compositor_.UpdateTextRenderingParams(
            MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
        handler_.Reposition();
        if (find_open_) LayoutFindEdit();
        return 0;
    case WM_DPICHANGED: {
        scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
        compositor_.RecreateTextFormats(scale_);
        RecreateFormats();
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        if (find_open_) LayoutFindEdit();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd_, &paint);
        Render();
        EndPaint(hwnd_, &paint);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &cursor);
        if (PlaybackWheel(cursor, steps)) {
            return 0;
        }
        if (PlaybackZoom(cursor, steps)) {
            return 0;
        }
        if (PagesWheel(cursor, steps, (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) != 0,
                       (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0)) {
            // handled by the page reader
        } else if (native_kind_ == NativeKind::Bitmap) {
            ZoomAt(static_cast<float>(cursor.x), static_cast<float>(cursor.y),
                   std::pow(1.15f, steps));
        } else if (native_kind_ == NativeKind::Archive) {
            archive_.Scroll(steps);
            archive_.Hover(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        } else if (native_kind_ == NativeKind::Markdown) {
            if ((GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0) markdown_.ScrollHorizontal(steps);
            else markdown_.ScrollAt(static_cast<float>(cursor.x), static_cast<float>(cursor.y), steps);
        } else if (native_kind_ == NativeKind::Table) {
            table_.Scroll(steps, (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0);
            KillTimer(hwnd_, kTableTipTimer);
        } else if (native_kind_ == NativeKind::Tree) {
            tree_.Scroll(steps);
            tree_.Hover(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
        } else {
            text_scroll_ = (std::max)(0.0f, text_scroll_ - steps * 56.0f * scale_);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        // Tilt wheel / touchpad sideways: the table grid scrolls horizontally.
        if (native_kind_ != NativeKind::Table && native_kind_ != NativeKind::Markdown) break;
        const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
        const bool changed = native_kind_ == NativeKind::Table ? table_.Scroll(-steps, true) : markdown_.ScrollHorizontal(-steps);
        if (changed) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT point{};
        ClientPoint(lparam, point);
        if (PlaybackMouseDown(point)) return 0;
        if (const ChromeButton button = HitChromeButton(point); button != ChromeButton::None) {
            if (GetFocus() != hwnd_) SetFocus(hwnd_);  // provider may hold focus
            if (button == ChromeButton::Open) {
                // Split button: the label opens the default app now; the
                // chevron half waits for the release to show the chooser.
                const D2D1_RECT_F open_rect = OpenButtonRect();
                if (point.x >= open_rect.right - 26.0f * scale_) return 0;
            }
            ActivateChromeButton(button);
            return 0;
        }
        if (const int chip = HitFooterChip(point); chip != 0) {
            if (GetFocus() != hwnd_) SetFocus(hwnd_);
            ActivateFooterChip(chip);
            return 0;
        }
        if (CodecCardClick(point)) return 0;
        if (const int size = HitIconSize(point); size >= 0) {
            if (static_cast<uint32_t>(size + 1) != icon_request_ && size != icon_selected_) {
                icon_request_ = static_cast<uint32_t>(size + 1);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (const int segment = HitMarkdownToggle(point); segment >= 0) {
            bool& second = ToggleSecond();
            if ((segment == 1) != second) {
                second = segment == 1;
                selecting_ = false;
                sel_anchor_ = sel_focus_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (find_open_ && FindBarHeight() > 0.0f) {
            const D2D1_RECT_F bar = FindBarRect();
            if (point.x >= bar.left && point.x < bar.right &&
                point.y >= bar.top && point.y < bar.bottom) {
                if (find_edit_) SetFocus(find_edit_);
                return 0;
            }
        }
        const D2D1_RECT_F content = ContentRect();
        if (point.x < content.left || point.x >= content.right ||
            point.y < content.top || point.y >= content.bottom) return 0;
        if (native_kind_ == NativeKind::Archive) {
            if (archive_.Click(static_cast<float>(point.x), static_cast<float>(point.y)))
                InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Table) {
            KillTimer(hwnd_, kTableTipTimer);
            if (table_.MouseDown(static_cast<float>(point.x), static_cast<float>(point.y),
                                 (wparam & MK_SHIFT) != 0)) {
                if (table_.Dragging()) SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (native_kind_ == NativeKind::Tree) {
            if (tree_.MouseDown(static_cast<float>(point.x), static_cast<float>(point.y))) {
                if (tree_.Dragging()) SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (native_kind_ == NativeKind::Markdown &&
            markdown_.Click(static_cast<float>(point.x), static_cast<float>(point.y))) {
            // Contents entry or next-chapter link: the click is spent.
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (AudioMouseDown(point)) return 0;
        if (PagesMouseDown(point)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        SetCapture(hwnd_);
        if (native_kind_ == NativeKind::Bitmap && CanPanImage()) {
            panning_ = true;
            pan_anchor_ = point;
            pan_start_x_ = pan_x_;
            pan_start_y_ = pan_y_;
        } else if (CanPanVideo()) {
            video_panning_ = true;
            video_pan_anchor_ = point;
            video_pan_start_x_ = video_pan_x_;
            video_pan_start_y_ = video_pan_y_;
        } else if (native_kind_ == NativeKind::Markdown && (wparam & MK_CONTROL) &&
                   !markdown_.LinkAt(static_cast<float>(point.x),
                                     static_cast<float>(point.y)).empty()) {
            ReleaseCapture();
            const std::wstring target =
                markdown_.LinkAt(static_cast<float>(point.x), static_cast<float>(point.y));
            // Only web and mail links; local paths and other schemes stay inert.
            if (_wcsnicmp(target.c_str(), L"http://", 7) == 0 ||
                _wcsnicmp(target.c_str(), L"https://", 8) == 0 ||
                _wcsnicmp(target.c_str(), L"mailto:", 7) == 0)
                ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                   native_kind_ == NativeKind::Markdown) {
            uint32_t index = 0;
            if (HitTestText(static_cast<float>(point.x), static_cast<float>(point.y), index)) {
                selecting_ = true;
                sel_anchor_ = sel_focus_ = index;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT point{};
        ClientPoint(lparam, point);
        if (PlaybackMouseDown(point)) return 0;
        if (const ChromeButton button = HitChromeButton(point); button != ChromeButton::None) {
            ActivateChromeButton(button);  // rapid clicks keep stepping through files
            return 0;
        }
        const D2D1_RECT_F content = ContentRect();
        if (native_kind_ == NativeKind::Archive) {
            // Folder listing: double-click opens the item in the main window.
            if (archive_.IsFolderListing() && archive_.HasSelection() &&
                archive_.Contains(static_cast<float>(point.x), static_cast<float>(point.y)) &&
                OpenListingSelection())
                return 0;
            // Second click of a fast pair keeps toggling like a single click.
            if (archive_.Click(static_cast<float>(point.x), static_cast<float>(point.y)))
                InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Pages) {
            const PagesLayout pages = ComputePagesLayout();
            if (point.x >= pages.view.left && point.x < pages.view.right &&
                point.y >= pages.view.top && point.y < pages.view.bottom) {
                TogglePagesFit();
                InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                PagesMouseDown(point);  // fast clicks in the strip keep jumping
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (native_kind_ == NativeKind::Bitmap &&
            point.x >= content.left && point.x < content.right &&
            point.y >= content.top && point.y < content.bottom) {
            ToggleFitActual();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT point{};
        ClientPoint(lparam, point);
        if (playback_drag_) {
            SeekPlayback(static_cast<float>(point.x));
            return 0;
        }
        if (PlaybackHover(point)) InvalidateRect(hwnd_, nullptr, FALSE);
        if (const ChromeButton hover = (panning_ || selecting_) ? ChromeButton::None
                                                                 : HitChromeButton(point);
            hover != chrome_hover_) {
            chrome_hover_ = hover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (const int chip = (panning_ || selecting_) ? 0 : HitFooterChip(point);
            chip != footer_hover_) {
            footer_hover_ = chip;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (!mouse_tracking_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0};
            mouse_tracking_ = TrackMouseEvent(&tracking) != FALSE;
        }
        if (native_kind_ == NativeKind::Archive && !panning_ && !selecting_ &&
            archive_.Hover(static_cast<float>(point.x), static_cast<float>(point.y)))
            InvalidateRect(hwnd_, nullptr, FALSE);
        if (native_kind_ == NativeKind::Table) {
            const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
            if (table_.Dragging()) {
                if (table_.MouseMove(x, y)) InvalidateRect(hwnd_, nullptr, FALSE);
            } else {
                if (table_.Hover(x, y)) InvalidateRect(hwnd_, nullptr, FALSE);
                // Full text of a clipped cell once the pointer rests on it.
                if (table_.TipArmed()) SetTimer(hwnd_, kTableTipTimer, 500, nullptr);
                else KillTimer(hwnd_, kTableTipTimer);
            }
        }
        if (native_kind_ == NativeKind::Tree) {
            const float x = static_cast<float>(point.x), y = static_cast<float>(point.y);
            if (tree_.Dragging() ? tree_.MouseMove(x, y) : tree_.Hover(x, y))
                InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (video_panning_) {
            video_pan_x_ = video_pan_start_x_ + static_cast<float>(point.x - video_pan_anchor_.x);
            video_pan_y_ = video_pan_start_y_ + static_cast<float>(point.y - video_pan_anchor_.y);
            ClampVideoPan(VideoFrameRect(video_.Snapshot()), video_.zoom());
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (panning_ && native_kind_ == NativeKind::Pages) {
            pages_pan_x_ = pan_start_x_ - static_cast<float>(point.x - pan_anchor_.x);
            pages_scroll_ = pan_start_y_ - static_cast<float>(point.y - pan_anchor_.y);
            ClampPages(ComputePagesLayout());
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (panning_) {
            pan_x_ = pan_start_x_ + static_cast<float>(point.x - pan_anchor_.x);
            pan_y_ = pan_start_y_ + static_cast<float>(point.y - pan_anchor_.y);
            const D2D1_RECT_F content = ContentRect();
            ClampPan(content.right - content.left, content.bottom - content.top);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (selecting_) {
            uint32_t index = 0;
            if (HitTestText(static_cast<float>(point.x), static_cast<float>(point.y), index)) {
                sel_focus_ = index;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        mouse_tracking_ = false;
        if (archive_.Leave()) InvalidateRect(hwnd_, nullptr, FALSE);
        KillTimer(hwnd_, kTableTipTimer);
        if (table_.Leave()) InvalidateRect(hwnd_, nullptr, FALSE);
        if (tree_.Leave()) InvalidateRect(hwnd_, nullptr, FALSE);
        if (PlaybackHover(POINT{-1, -1})) InvalidateRect(hwnd_, nullptr, FALSE);
        if (chrome_hover_ != ChromeButton::None) {
            chrome_hover_ = ChromeButton::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (footer_hover_ != 0) {
            footer_hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (message == WM_LBUTTONUP) {
            POINT up{};
            ClientPoint(lparam, up);
            if (HitChromeButton(up) == ChromeButton::Open) {
                const D2D1_RECT_F open_rect = OpenButtonRect();
                if (up.x >= open_rect.right - 26.0f * scale_) {
                    // Deferred once more so the popup's modal loop starts after
                    // every message of this click has been dispatched.
                    PostMessageW(hwnd_, kOpenWithMenuMessage, 0, 0);
                }
                return 0;
            }
        }
        if (message == WM_LBUTTONUP && playback_drag_)
            SeekPlayback(static_cast<float>(GET_X_LPARAM(lparam)));
        EndPlaybackDrag(message == WM_LBUTTONUP);
        panning_ = false;
        selecting_ = false;
        video_panning_ = false;
        if (table_.Dragging()) {
            table_.MouseUp();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (tree_.Dragging()) {
            tree_.MouseUp();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (message == WM_LBUTTONUP) ReleaseCapture();
        return 0;
    case WM_RBUTTONUP: {
        POINT point{};
        ClientPoint(lparam, point);
        if (find_open_ && FindBarHeight() > 0.0f) {
            const D2D1_RECT_F bar = FindBarRect();
            if (point.x >= bar.left && point.x < bar.right &&
                point.y >= bar.top && point.y < bar.bottom) {
                if (find_edit_) SetFocus(find_edit_);
                return 0;
            }
        }
        const D2D1_RECT_F content = ContentRect();
        const bool in_content = point.x >= content.left && point.x < content.right &&
            point.y >= content.top && point.y < content.bottom;
        if (native_kind_ == NativeKind::Archive && in_content) {
            archive_.SelectAt(static_cast<float>(point.x), static_cast<float>(point.y));
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if ((native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
             native_kind_ == NativeKind::Markdown) && in_content) {
            uint32_t index = 0;
            if (HitTestText(static_cast<float>(point.x), static_cast<float>(point.y), index)) {
                const uint32_t a = (std::min)(sel_anchor_, sel_focus_);
                const uint32_t b = (std::max)(sel_anchor_, sel_focus_);
                if (!(b > a && index >= a && index < b))
                    sel_anchor_ = sel_focus_ = index;
            }
        }
        POINT screen = point;
        ClientToScreen(hwnd_, &screen);
        ShowContextMenu(screen);
        return 0;
    }
    case WM_CONTEXTMENU: {
        // Also reached from the preview-handler overlay (forwarded by post);
        // never keep a cross-thread sender waiting on the modal menu.
        if (InSendMessage()) ReplyMessage(0);
        POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (screen.x == -1 && screen.y == -1) {
            const D2D1_RECT_F content = ContentRect();
            screen.x = static_cast<LONG>((content.left + content.right) * 0.5f);
            screen.y = static_cast<LONG>((content.top + content.bottom) * 0.5f);
            ClientToScreen(hwnd_, &screen);
        } else {
            POINT client = screen;
            ScreenToClient(hwnd_, &client);
            const D2D1_RECT_F content = ContentRect();
            if (client.x < content.left || client.x >= content.right ||
                client.y < content.top || client.y >= content.bottom)
                break;  // caption right-click keeps the system menu
            if (native_kind_ == NativeKind::Archive) {
                archive_.SelectAt(static_cast<float>(client.x), static_cast<float>(client.y));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        ShowContextMenu(screen);
        return 0;
    }
    case WM_TIMER:
        if (wparam == kPreviewDeadlineTimer) {
            KillTimer(hwnd_, kPreviewDeadlineTimer);
            if (preview_deadline_) {
                preview_deadline_ = 0;
                if (!closing_ && visible()) InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (wparam == kTableTipTimer) {
            KillTimer(hwnd_, kTableTipTimer);
            if (native_kind_ == NativeKind::Table && table_.ShowTip())
                InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wparam == kCopyFeedbackTimer) {
            KillTimer(hwnd_, kCopyFeedbackTimer);
            if (copy_feedback_) {
                copy_feedback_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (wparam == kZoomCloseTimer) {
            if (closing_) FinishClose(false);
            else KillTimer(hwnd_, kZoomCloseTimer);
            return 0;
        }
        if (wparam == 9) {
            TickPlaybackSeek();
            return 0;
        }
        if (wparam == 8 && video_.active()) {
            if (visible() && !IsIconic(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (wparam == kAnimationTimer && animation_active_ && visible()) {
            if (frame_count_ > 1 && !waiting_for_frame_ && !playback_scrub_pending_) {
                if (frame_index_ + 1 >= frame_count_) {
                    if (loop_count_ != 0 && completed_loops_ >= loop_count_) {
                        animation_active_ = false;
                        KillTimer(hwnd_, kAnimationTimer);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                    requested_frame_ = 0;
                    ++completed_loops_;
                } else {
                    requested_frame_ = frame_index_ + 1;
                }
                waiting_for_frame_ = true;
                KillTimer(hwnd_, kAnimationTimer);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_NCMOUSEMOVE:
        if (const bool hovered = wparam == HTCLOSE; hovered != close_hover_) {
            close_hover_ = hovered;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
            TrackMouseEvent(&tracking);
        }
        return 0;
    case WM_NCMOUSELEAVE:
        if (close_hover_) {
            close_hover_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_NCLBUTTONDOWN:
        if (wparam == HTCLOSE) {
            Close();
            return 0;
        }
        break;
    case WM_NCLBUTTONDBLCLK:
        if (wparam == HTCAPTION) {
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize.x = static_cast<LONG>(480.0f * scale_);
        limits->ptMinTrackSize.y = static_cast<LONG>(320.0f * scale_);
        return 0;
    }
    case WM_COMMAND:
        if (find_edit_ && lparam == reinterpret_cast<LPARAM>(find_edit_) &&
            HIWORD(wparam) == EN_CHANGE) {
            SyncFindFromEdit();
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT: {
        if (find_edit_ && reinterpret_cast<HWND>(lparam) == find_edit_) {
            if (!find_edit_brush_) find_edit_brush_ = CreateSolidBrush(FindEditBrushColor(dark_));
            SetTextColor(reinterpret_cast<HDC>(wparam), EditTextColor(dark_));
            SetBkColor(reinterpret_cast<HDC>(wparam), EditBackColor(dark_));
            SetBkMode(reinterpret_cast<HDC>(wparam), OPAQUE);
            return reinterpret_cast<LRESULT>(EditBackBrush(find_edit_brush_));
        }
        break;
    }
    case WM_CHAR:
        if (find_open_ && find_edit_ && wparam >= 32 && wparam != 127) {
            if (GetFocus() != find_edit_) SetFocus(find_edit_);
            SendMessageW(find_edit_, WM_CHAR, wparam, lparam);
        }
        return 0;
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wparam == VK_ESCAPE) {
            if (find_open_) CloseFind();
            else Close();
            return 0;
        }
        if (!find_open_ && !ctrl && native_kind_ == NativeKind::Bitmap) {
            if (wparam == 'R') { RotateImage(); return 0; }
            if (wparam == 'I') {
                image_info_ = !image_info_;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (!find_open_ && HasPlayback() && !ctrl) {
            if (wparam == VK_SPACE) {
                if (!(lparam & (1LL << 30))) TogglePlayback();
                return 0;
            }
            if (wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD) {
                StepPlayback(wparam == VK_OEM_COMMA ? -1 : 1);
                return 0;
            }
            if (wparam == VK_HOME || wparam == VK_END) {
                if (video_.active()) video_.Play(false);
                animation_active_ = false;
                KillTimer(hwnd_, kAnimationTimer);
                const auto track = PlaybackTrackRect();
                SeekPlayback(wparam == VK_HOME ? track.left : track.right);
                return 0;
            }
        }
        if (wparam == VK_SPACE && !find_open_) {
            Close();
            return 0;
        }
        const bool text_kind = native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
                               native_kind_ == NativeKind::Markdown ||
                               native_kind_ == NativeKind::Table ||
                               native_kind_ == NativeKind::Tree;
        if (native_kind_ == NativeKind::Tree && !find_open_ && ctrl && shift && wparam == 'C' &&
            tree_.HasCurrent()) {
            // The node's path ($.items[3].name) rather than the file's.
            luling::ops::WriteClipboardText(tree_.CurrentPath());
            return 0;
        }
        if (!find_open_ && command_message_) {
            // File verbs on the previewed entry (same keys as the main list).
            QuickPreviewAction action = QuickPreviewAction::None;
            if (ctrl && shift && wparam == 'C') action = QuickPreviewAction::CopyPath;
            else if (ctrl && wparam == 'C' && !text_kind) action = QuickPreviewAction::Copy;
            else if (ctrl && wparam == 'X') action = QuickPreviewAction::Cut;
            else if (wparam == VK_DELETE) action = QuickPreviewAction::Delete;
            else if (wparam == VK_F2) action = QuickPreviewAction::Rename;
            if (action != QuickPreviewAction::None) {
                PostAction(action);
                return 0;
            }
        }
        if (native_kind_ == NativeKind::Archive && !find_open_) {
            // With a row selected, Left/Right fold the tree; otherwise they
            // keep stepping through files. Paging keys scroll the tree.
            const bool fold = (wparam == VK_LEFT || wparam == VK_RIGHT) && archive_.HasSelection();
            const bool page = wparam == VK_PRIOR || wparam == VK_NEXT ||
                              ((wparam == VK_HOME || wparam == VK_END) && !HasPlayback());
            if (fold || page) {
                archive_.Key(static_cast<UINT>(wparam));
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (native_kind_ == NativeKind::Archive && find_open_ && wparam == VK_RETURN)
            return 0;
        if (native_kind_ == NativeKind::Table && !find_open_ &&
            table_.Key(static_cast<UINT>(wparam), shift, ctrl)) {
            KillTimer(hwnd_, kTableTipTimer);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Tree && !find_open_ && !ctrl &&
            !(HasPlayback() && (wparam == VK_HOME || wparam == VK_END)) &&
            tree_.Key(static_cast<UINT>(wparam))) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (native_kind_ == NativeKind::Markdown && !find_open_ && !ctrl &&
            (wparam == VK_PRIOR || wparam == VK_NEXT || wparam == VK_LEFT || wparam == VK_RIGHT ||
             ((wparam == VK_HOME || wparam == VK_END) && !HasPlayback()))) {
            markdown_.Key(static_cast<UINT>(wparam));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (ctrl && wparam == 'F' &&
            (native_kind_ == NativeKind::Text || native_kind_ == NativeKind::Hex ||
             native_kind_ == NativeKind::Archive || native_kind_ == NativeKind::Markdown ||
             native_kind_ == NativeKind::Table || native_kind_ == NativeKind::Tree)) {
            OpenFind();
            return 0;
        }
        if (ctrl && wparam == 'C' && text_kind) {
            CopyTextSelection(false);
            copy_feedback_ = true;
            SetTimer(hwnd_, kCopyFeedbackTimer, 1400, nullptr);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (ctrl && wparam == 'A' && text_kind) {
            SelectAllText();
            return 0;
        }
        if ((wparam == VK_F3 || (find_open_ && wparam == VK_RETURN)) && text_kind) {
            if (!find_open_) OpenFind();
            FindNext(shift ? -1 : 1);
            return 0;
        }
        if (!find_open_ && !ctrl && PagesKey(wparam)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (!find_open_ && native_kind_ == NativeKind::Bitmap) {
            if (wparam == '1' || wparam == VK_NUMPAD1) {
                SetFitMode();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == '2' || wparam == VK_NUMPAD2) {
                SetActualPixels();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (wparam == VK_RETURN && native_kind_ == NativeKind::Archive && !find_open_ &&
            OpenListingSelection())
            return 0;
        if (wparam == VK_RETURN && owner_ && open_message_ && !find_open_)
            PostMessageW(owner_, open_message_, 0, 0);
        else if ((wparam == VK_LEFT || wparam == VK_UP) && owner_ && navigate_message_)
            PostMessageW(owner_, navigate_message_, static_cast<WPARAM>(-1), 0);
        else if ((wparam == VK_RIGHT || wparam == VK_DOWN) && owner_ && navigate_message_)
            PostMessageW(owner_, navigate_message_, 1, 0);
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wparam == VK_RETURN && (lparam & (1 << 29)) && !find_open_) {
            PostAction(QuickPreviewAction::Properties);  // Alt+Enter → properties
            return 0;
        }
        break;
    case WM_ACTIVATE: {
        const bool active = LOWORD(wparam) != WA_INACTIVE;
        handler_.NotifyAppActivate(active);
        if (!active) {
            HWND other = reinterpret_cast<HWND>(lparam);
            if (find_edit_ && other != find_edit_ && other != hwnd_) {
                const HWND other_owner = other ? GetWindow(other, GW_OWNER) : nullptr;
                if (other_owner != hwnd_)
                    ShowWindow(find_edit_, SW_HIDE);
            }
        } else if (find_open_) {
            LayoutFindEdit();
        }
        return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        ApplyWindowEffect(hwnd_, effect_, dark_);
        if (find_edit_brush_) {
            DeleteObject(find_edit_brush_);
            find_edit_brush_ = nullptr;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        Close();
        return 0;
    case WM_DESTROY:
        ResetPlayback();
        ResetAnimation();
        handler_.Reset();
        thumbnails_.Reset();
        DestroyFindEdit();
        close_format_.reset();
        preview_text_format_.reset();
        text_layout_.reset();
        compositor_.Shutdown();
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

} // namespace luling::ui
