#include "schedule_panel.h"

#include "lunar_calendar.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <string>
#include <vector>

namespace luling::app {
namespace {

constexpr wchar_t kClassName[] = L"LulingSchedulePanel";
constexpr int kWinW = 420;
constexpr int kPad = 16;
constexpr int kListTop = 46;
constexpr int kRowH = 36;
constexpr int kRowGap = 4;
constexpr int kMaxRows = 4;
constexpr int kEmptyH = 40;
constexpr int kEditH = 30;
constexpr int kAddH = 34;

int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int WeekdayMon(int y, int m, int d) {
    return static_cast<int>((DaysFromCivil(y, static_cast<unsigned>(m),
                                           static_cast<unsigned>(d)) + 3) % 7);
}

int DaysInMonth(int y, int m) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return days[m - 1];
}

HFONT MakeFont(int dip_size, bool bold, float scale) {
    LOGFONTW lf{};
    lf.lfHeight = -static_cast<LONG>(dip_size * scale + 0.5f);
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    return CreateFontIndirectW(&lf);
}

void Text(HDC dc, HFONT font, COLORREF color, const std::wstring& text,
          int left, int top, int right, int bottom, UINT flags) {
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT rc{ left, top, right, bottom };
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc,
              flags | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

void FillRectColor(HDC dc, const RECT& rc, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rc, brush);
    DeleteObject(brush);
}

void RoundFill(HDC dc, const RECT& rc, COLORREF color, int radius) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void RoundFrame(HDC dc, const RECT& rc, COLORREF color, int radius) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
}

COLORREF BgColor(bool dark) { return dark ? RGB(30, 30, 32) : RGB(249, 249, 250); }
COLORREF SurfaceColor(bool dark) { return dark ? RGB(43, 43, 46) : RGB(240, 240, 243); }
COLORREF TextColor(bool dark) { return dark ? RGB(240, 240, 240) : RGB(28, 28, 30); }
COLORREF SubColor(bool dark) { return dark ? RGB(158, 158, 164) : RGB(104, 104, 112); }
COLORREF AccentColor(bool dark) { return dark ? RGB(0, 187, 224) : RGB(0, 138, 168); }
COLORREF DangerColor(bool dark) { return dark ? RGB(255, 120, 120) : RGB(190, 40, 40); }

bool ParseIntField(const std::wstring& text, int& value) {
    wchar_t* end = nullptr;
    const long parsed = wcstol(text.c_str(), &end, 10);
    if (!end || *end != L'\0') return false;
    value = static_cast<int>(parsed);
    return true;
}

std::wstring EditText(HWND edit) {
    const int len = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(edit, text.data(), len + 1);
    text.resize(static_cast<size_t>(GetWindowTextLengthW(edit)));
    return text;
}

const wchar_t* RepeatName(int repeat) {
    switch (repeat) {
    case 1: return L"每天";
    case 2: return L"每周";
    case 3: return L"每月";
    default: return L"不重复";
    }
}

std::wstring DateText(int year, int month, int day) {
    wchar_t text[24]{};
    swprintf_s(text, L"%04d-%02d-%02d", year, month, day);
    return text;
}

std::wstring TimeText(int hour, int minute) {
    wchar_t text[8]{};
    swprintf_s(text, L"%02d:%02d", hour, minute);
    return text;
}

std::wstring ItemWhen(const ScheduleItem& item) {
    wchar_t text[32]{};
    swprintf_s(text, L"%02d-%02d %02d:%02d", item.month, item.day, item.hour, item.minute);
    return text;
}

std::vector<const ScheduleItem*> VisibleItems(const ScheduleStore& store) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const int today = now.wYear * 10000 + now.wMonth * 100 + now.wDay;
    std::vector<const ScheduleItem*> ordered;
    for (const auto& item : store.Items()) {
        if (item.repeat == 0 && item.year * 10000 + item.month * 100 + item.day < today) continue;
        ordered.push_back(&item);
    }
    std::sort(ordered.begin(), ordered.end(), [](const ScheduleItem* a, const ScheduleItem* b) {
        wchar_t ka[32]{}, kb[32]{};
        swprintf_s(ka, L"%04d%02d%02d%02d%02d", a->year, a->month, a->day, a->hour, a->minute);
        swprintf_s(kb, L"%04d%02d%02d%02d%02d", b->year, b->month, b->day, b->hour, b->minute);
        return std::wstring(ka) < std::wstring(kb);
    });
    return ordered;
}

std::wstring LunarCell(int year, int month, int day) {
    const calendar::DayInfo info = calendar::Describe(year, month, day);
    if (!info.festival.empty()) return info.festival;
    if (!info.solar_term.empty()) return info.solar_term;
    if (!info.has_lunar) return {};
    if (info.lunar.day == 1) return calendar::LunarMonthName(info.lunar);
    return calendar::LunarDayName(info.lunar.day);
}

// ---- Generic list popup ----------------------------------------------------

struct ListPopup {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    std::vector<std::wstring> items;
    int selected = -1;
    int scroll = 0;
    bool done = false;
    bool accepted = false;
    bool shown = false;
    bool dark = false;
    float scale = 1.0f;
    HFONT font = nullptr;
    int width = 0;
    int visible_rows = 0;
    int row_h = 0;
};

constexpr wchar_t kListClass[] = L"LulingSchedulePickList";

int DipF(float scale, float dip) { return static_cast<int>(dip * scale + 0.5f); }

LRESULT CALLBACK ListProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<ListPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        s = static_cast<ListPopup*>(create->lpCreateParams);
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC target = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        FillRectColor(dc, client, s->dark ? RGB(43, 43, 46) : RGB(250, 250, 250));
        const int row_h = s->row_h;
        const int count = static_cast<int>(s->items.size());
        for (int row = 0; row < s->visible_rows; ++row) {
            const int index = s->scroll + row;
            if (index >= count) break;
            const int top = row * row_h;
            RECT rc{ 2, top, client.right - 2, top + row_h - 2 };
            if (index == s->selected) RoundFill(dc, rc, AccentColor(s->dark), DipF(s->scale, 6));
            Text(dc, s->font, index == s->selected
                     ? (s->dark ? RGB(12, 12, 12) : RGB(255, 255, 255)) : TextColor(s->dark),
                 s->items[static_cast<size_t>(index)], DipF(s->scale, 10), top,
                 client.right - DipF(s->scale, 8), top + row_h - 2, DT_LEFT);
        }
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (s->shown && LOWORD(wparam) == WA_INACTIVE) s->done = true;
        return 0;
    case WM_LBUTTONDOWN: {
        const int y = GET_Y_LPARAM(lparam);
        const int index = s->scroll + y / s->row_h;
        if (index >= 0 && index < static_cast<int>(s->items.size())) {
            s->selected = index;
            s->accepted = true;
            s->done = true;
        } else {
            s->done = true;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const int max_scroll = std::max(0, static_cast<int>(s->items.size()) - s->visible_rows);
        s->scroll = std::clamp(s->scroll - (delta > 0 ? 1 : -1), 0, max_scroll);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) s->done = true;
        else if (wparam == VK_RETURN && s->selected >= 0) {
            s->accepted = true;
            s->done = true;
        }
        return 0;
    case WM_CLOSE:
        s->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int PickListItem(HWND owner, POINT anchor, std::vector<std::wstring> items, int selected,
                 bool dark, float scale, int width_dip, int visible_rows) {
    ListPopup state;
    state.owner = owner;
    state.items = std::move(items);
    state.selected = selected;
    state.dark = dark;
    state.scale = scale;
    state.visible_rows = visible_rows;
    state.row_h = DipF(scale, 32);
    state.width = DipF(scale, static_cast<float>(width_dip));
    state.font = MakeFont(12, false, scale);
    if (state.selected >= state.visible_rows)
        state.scroll = std::min(state.selected - 1,
            std::max(0, static_cast<int>(state.items.size()) - state.visible_rows));

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = ListProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kListClass;
    RegisterClassExW(&wc);

    const int height = state.row_h * state.visible_rows + DipF(scale, 4);
    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    const int x = std::clamp(anchor.x, mi.rcWork.left, mi.rcWork.right - state.width);
    const int y = std::clamp(anchor.y, mi.rcWork.top, mi.rcWork.bottom - height);
    state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kListClass, L"", WS_POPUP,
                                 x, y, state.width, height, owner, nullptr,
                                 GetModuleHandleW(nullptr), &state);
    if (!state.hwnd) {
        DeleteObject(state.font);
        return -1;
    }
    HRGN region = CreateRoundRectRgn(0, 0, state.width + 1, height + 1, DipF(scale, 10),
                                     DipF(scale, 10));
    SetWindowRgn(state.hwnd, region, TRUE);
    ShowWindow(state.hwnd, SW_SHOW);
    state.shown = true;
    SetForegroundWindow(state.hwnd);
    SetFocus(state.hwnd);

    MSG msg{};
    while (!state.done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(state.hwnd);
    DeleteObject(state.font);
    return state.accepted ? state.selected : -1;
}

// ---- Calendar popup --------------------------------------------------------

struct CalendarPopup {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    int year = 0, month = 0, day = 0;
    int view_year = 0, view_month = 0;
    bool done = false;
    bool accepted = false;
    bool shown = false;
    bool dark = false;
    float scale = 1.0f;
    HFONT head = nullptr, body = nullptr, small = nullptr;
};

constexpr wchar_t kCalendarClass[] = L"LulingSchedulePickDate";
constexpr int kCalCellW = 44;
constexpr int kCalCellH = 40;

void PaintCalendar(CalendarPopup& s, HDC target) {
    RECT client{};
    GetClientRect(s.hwnd, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    FillRectColor(dc, client, s.dark ? RGB(43, 43, 46) : RGB(250, 250, 250));

    const int head_h = DipF(s.scale, 40);
    const int week_h = DipF(s.scale, 22);
    wchar_t head[24]{};
    swprintf_s(head, L"%d年%d月", s.view_year, s.view_month);
    Text(dc, s.head, TextColor(s.dark), head, DipF(s.scale, 12), 0,
         client.right - DipF(s.scale, 70), head_h, DT_LEFT);
    Text(dc, s.body, SubColor(s.dark), L"\x2039", client.right - DipF(s.scale, 66), 0,
         client.right - DipF(s.scale, 38), head_h, DT_CENTER);
    Text(dc, s.body, SubColor(s.dark), L"\x203A", client.right - DipF(s.scale, 34), 0,
         client.right - DipF(s.scale, 6), head_h, DT_CENTER);

    static const wchar_t* const kWeek[7] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
    const int cell_w = DipF(s.scale, static_cast<float>(kCalCellW));
    const int cell_h = DipF(s.scale, static_cast<float>(kCalCellH));
    const int grid_left = (client.right - cell_w * 7) / 2;
    for (int i = 0; i < 7; ++i)
        Text(dc, s.small, SubColor(s.dark), kWeek[i], grid_left + i * cell_w, head_h,
             grid_left + (i + 1) * cell_w, head_h + week_h, DT_CENTER);

    const int first_weekday = WeekdayMon(s.view_year, s.view_month, 1);
    const int days = DaysInMonth(s.view_year, s.view_month);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    for (int day = 1; day <= days; ++day) {
        const int slot = first_weekday + day - 1;
        const int col = slot % 7;
        const int row = slot / 7;
        const int left = grid_left + col * cell_w;
        const int top = head_h + week_h + row * cell_h;
        RECT rc{ left + 2, top + 1, left + cell_w - 2, top + cell_h - 2 };
        const bool selected = s.year == s.view_year && s.month == s.view_month && s.day == day;
        const bool today = now.wYear == s.view_year && now.wMonth == s.view_month &&
                           now.wDay == day;
        if (selected) RoundFill(dc, rc, AccentColor(s.dark), DipF(s.scale, 8));
        else if (today) RoundFrame(dc, rc, AccentColor(s.dark), DipF(s.scale, 8));
        wchar_t num[8]{};
        swprintf_s(num, L"%d", day);
        Text(dc, s.body, selected ? (s.dark ? RGB(12, 12, 12) : RGB(255, 255, 255))
                                  : TextColor(s.dark),
             num, left, top - DipF(s.scale, 4), left + cell_w, top + cell_h / 2, DT_CENTER);
        Text(dc, s.small, selected ? (s.dark ? RGB(12, 12, 12) : RGB(255, 255, 255))
                                   : SubColor(s.dark),
             LunarCell(s.view_year, s.view_month, day), left, top + cell_h / 2 - DipF(s.scale, 5),
             left + cell_w, top + cell_h - 2, DT_CENTER);
    }

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void ShiftMonth(CalendarPopup& s, int delta) {
    int month = s.view_month + delta;
    int year = s.view_year;
    while (month < 1) { month += 12; --year; }
    while (month > 12) { month -= 12; ++year; }
    if (year < 1901 || year > 2100) return;
    s.view_year = year;
    s.view_month = month;
}

LRESULT CALLBACK CalendarProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<CalendarPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        s = static_cast<CalendarPopup*>(create->lpCreateParams);
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC target = BeginPaint(hwnd, &ps);
        PaintCalendar(*s, target);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (s->shown && LOWORD(wparam) == WA_INACTIVE) s->done = true;
        return 0;
    case WM_LBUTTONDOWN: {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int head_h = DipF(s->scale, 40);
        if (y < head_h) {
            if (x >= client.right - DipF(s->scale, 66) && x < client.right - DipF(s->scale, 38))
                ShiftMonth(*s, -1);
            else if (x >= client.right - DipF(s->scale, 34)) ShiftMonth(*s, 1);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        const int cell_w = DipF(s->scale, static_cast<float>(kCalCellW));
        const int cell_h = DipF(s->scale, static_cast<float>(kCalCellH));
        const int week_h = DipF(s->scale, 22);
        const int grid_left = (client.right - cell_w * 7) / 2;
        const int col = (x - grid_left) / cell_w;
        const int row = (y - head_h - week_h) / cell_h;
        if (col >= 0 && col < 7 && row >= 0 && row < 6) {
            const int day = row * 7 + col - WeekdayMon(s->view_year, s->view_month, 1) + 1;
            if (day >= 1 && day <= DaysInMonth(s->view_year, s->view_month)) {
                s->year = s->view_year;
                s->month = s->view_month;
                s->day = day;
                s->accepted = true;
                s->done = true;
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) s->done = true;
        return 0;
    case WM_CLOSE:
        s->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool PickDate(HWND owner, POINT anchor, int& year, int& month, int& day, bool dark, float scale) {
    CalendarPopup state;
    state.owner = owner;
    state.year = year;
    state.month = month;
    state.day = day;
    state.view_year = year;
    state.view_month = month;
    state.dark = dark;
    state.scale = scale;
    state.head = MakeFont(14, true, scale);
    state.body = MakeFont(12, false, scale);
    state.small = MakeFont(9, false, scale);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = CalendarProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kCalendarClass;
    RegisterClassExW(&wc);

    const int width = DipF(scale, static_cast<float>(kCalCellW * 7 + 16));
    const int height = DipF(scale, 40 + 22 + kCalCellH * 6 + 8);
    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    const int x = std::clamp(anchor.x, mi.rcWork.left, mi.rcWork.right - width);
    const int y = std::clamp(anchor.y, mi.rcWork.top, mi.rcWork.bottom - height);
    state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kCalendarClass, L"", WS_POPUP,
                                 x, y, width, height, owner, nullptr,
                                 GetModuleHandleW(nullptr), &state);
    if (!state.hwnd) {
        DeleteObject(state.head);
        DeleteObject(state.body);
        DeleteObject(state.small);
        return false;
    }
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, DipF(scale, 12),
                                     DipF(scale, 12));
    SetWindowRgn(state.hwnd, region, TRUE);
    ShowWindow(state.hwnd, SW_SHOW);
    state.shown = true;
    SetForegroundWindow(state.hwnd);
    SetFocus(state.hwnd);

    MSG msg{};
    while (!state.done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    const bool accepted = state.accepted;
    if (accepted) {
        year = state.year;
        month = state.month;
        day = state.day;
    }
    DestroyWindow(state.hwnd);
    DeleteObject(state.head);
    DeleteObject(state.body);
    DeleteObject(state.small);
    return accepted;
}

// ---- Time popup (hour and minute side by side) ------------------------------

struct TimePopup {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    int hour = 0, minute = 0;
    int hour_scroll = 0, minute_scroll = 0;
    bool done = false, accepted = false, shown = false, dark = false;
    float scale = 1.0f;
    HFONT head = nullptr, body = nullptr;
    int row_h = 0;
};

constexpr wchar_t kTimeClass[] = L"LulingSchedulePickTime";

void PaintTime(TimePopup& s, HDC target) {
    RECT client{};
    GetClientRect(s.hwnd, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    FillRectColor(dc, client, s.dark ? RGB(43, 43, 46) : RGB(250, 250, 250));

    const int head_h = DipF(s.scale, 28);
    const int col_w = client.right / 2;
    const int button_h = DipF(s.scale, 42);
    const int list_top = head_h;
    const int list_bottom = client.bottom - button_h;
    const int rows = std::max(1, (list_bottom - list_top) / s.row_h);
    Text(dc, s.head, SubColor(s.dark), L"时", 0, 0, col_w, head_h, DT_CENTER);
    Text(dc, s.head, SubColor(s.dark), L"分", col_w, 0, client.right, head_h, DT_CENTER);
    FillRectColor(dc, RECT{ col_w, list_top, col_w + 1, list_bottom },
                  s.dark ? RGB(70, 70, 74) : RGB(210, 210, 214));

    auto column = [&](int total, int selected, int scroll, int left, int right) {
        for (int row = 0; row < rows; ++row) {
            const int index = scroll + row;
            if (index >= total) break;
            const int top = list_top + row * s.row_h;
            RECT rc{ left + 3, top + 1, right - 3, top + s.row_h - 2 };
            if (index == selected) RoundFill(dc, rc, AccentColor(s.dark), DipF(s.scale, 6));
            wchar_t text[8]{};
            swprintf_s(text, L"%02d", index);
            Text(dc, s.body, index == selected
                     ? (s.dark ? RGB(12, 12, 12) : RGB(255, 255, 255)) : TextColor(s.dark),
                 text, left, top, right, top + s.row_h - 2, DT_CENTER);
        }
    };
    column(24, s.hour, s.hour_scroll, 0, col_w);
    column(60, s.minute, s.minute_scroll, col_w + 1, client.right);

    RECT btn{ DipF(s.scale, 12), client.bottom - button_h + 4,
              client.right - DipF(s.scale, 12), client.bottom - DipF(s.scale, 6) };
    RoundFill(dc, btn, AccentColor(s.dark), DipF(s.scale, 8));
    Text(dc, s.body, s.dark ? RGB(12, 12, 12) : RGB(255, 255, 255), L"确定",
         btn.left, btn.top, btn.right, btn.bottom, DT_CENTER);

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

int TimeRowHeight(float scale) { return DipF(scale, 30); }

LRESULT CALLBACK TimeProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<TimePopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        s = static_cast<TimePopup*>(create->lpCreateParams);
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC target = BeginPaint(hwnd, &ps);
        PaintTime(*s, target);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (s->shown && LOWORD(wparam) == WA_INACTIVE) s->done = true;
        return 0;
    case WM_LBUTTONDOWN: {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int button_h = DipF(s->scale, 42);
        if (y >= client.bottom - button_h) {
            s->accepted = true;
            s->done = true;
            return 0;
        }
        const int head_h = DipF(s->scale, 28);
        const int row = (y - head_h) / s->row_h;
        if (row >= 0) {
            const int col_w = client.right / 2;
            if (x < col_w) {
                const int index = s->hour_scroll + row;
                if (index >= 0 && index < 24) s->hour = index;
            } else {
                const int index = s->minute_scroll + row;
                if (index >= 0 && index < 60) s->minute = index;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        RECT client{};
        GetClientRect(hwnd, &client);
        POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ScreenToClient(hwnd, &pt);
        const int head_h = DipF(s->scale, 28);
        const int button_h = DipF(s->scale, 42);
        const int rows = std::max(1, static_cast<int>((client.bottom - head_h - button_h) / s->row_h));
        const int step = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1;
        if (pt.x < client.right / 2)
            s->hour_scroll = std::clamp(s->hour_scroll + step, 0, std::max(0, 24 - rows));
        else
            s->minute_scroll = std::clamp(s->minute_scroll + step, 0, std::max(0, 60 - rows));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) s->done = true;
        else if (wparam == VK_RETURN) {
            s->accepted = true;
            s->done = true;
        }
        return 0;
    case WM_CLOSE:
        s->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool PickTime(HWND owner, POINT anchor, int& hour, int& minute, bool dark, float scale) {
    TimePopup state;
    state.owner = owner;
    state.hour = hour;
    state.minute = minute;
    state.dark = dark;
    state.scale = scale;
    state.row_h = TimeRowHeight(scale);
    state.head = MakeFont(11, true, scale);
    state.body = MakeFont(12, false, scale);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = TimeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTimeClass;
    RegisterClassExW(&wc);

    const int width = DipF(scale, 200);
    const int visible_rows = 6;
    const int height = DipF(scale, 28) + state.row_h * visible_rows + DipF(scale, 42);
    state.hour_scroll = std::clamp(hour - 2, 0, std::max(0, 24 - visible_rows));
    state.minute_scroll = std::clamp(minute - 2, 0, std::max(0, 60 - visible_rows));
    HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    const int x = std::clamp(anchor.x, mi.rcWork.left, mi.rcWork.right - width);
    const int y = std::clamp(anchor.y, mi.rcWork.top, mi.rcWork.bottom - height);
    state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTimeClass, L"", WS_POPUP,
                                 x, y, width, height, owner, nullptr,
                                 GetModuleHandleW(nullptr), &state);
    if (!state.hwnd) {
        DeleteObject(state.head);
        DeleteObject(state.body);
        return false;
    }
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, DipF(scale, 12),
                                     DipF(scale, 12));
    SetWindowRgn(state.hwnd, region, TRUE);
    ShowWindow(state.hwnd, SW_SHOW);
    state.shown = true;
    SetForegroundWindow(state.hwnd);
    SetFocus(state.hwnd);

    MSG msg{};
    while (!state.done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (state.accepted) {
        hour = state.hour;
        minute = state.minute;
    }
    DestroyWindow(state.hwnd);
    DeleteObject(state.head);
    DeleteObject(state.body);
    return state.accepted;
}

// ---- Schedule panel --------------------------------------------------------

struct PanelState {
    ScheduleStore* store = nullptr;
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND title_edit = nullptr;
    HWND notes_edit = nullptr;
    HFONT font_title = nullptr;
    HFONT font_body = nullptr;
    HFONT font_small = nullptr;
    HBRUSH edit_brush = nullptr;
    float scale = 1.0f;
    bool dark = false;
    int repeat = 0;
    int sel_year = 0, sel_month = 0, sel_day = 0;
    int sel_hour = 0, sel_minute = 0;
    int scroll = 0;
    int list_h = 0;
    int win_h = 0;
    bool shown = false;
    bool done = false;
    std::wstring error;
};

int Dip(const PanelState& s, float dip) { return static_cast<int>(dip * s.scale + 0.5f); }

int ListHeight(const PanelState& s) {
    const int count = static_cast<int>(VisibleItems(*s.store).size());
    if (count == 0) return Dip(s, static_cast<float>(kEmptyH));
    const int rows = std::min(count, kMaxRows);
    return Dip(s, static_cast<float>(rows * (kRowH + kRowGap) - kRowGap));
}

int DividerY(const PanelState& s) { return Dip(s, static_cast<float>(kListTop)) + s.list_h + Dip(s, 8); }
int TitleLabelY(const PanelState& s) { return DividerY(s) + Dip(s, 8); }
int TitleY(const PanelState& s) { return TitleLabelY(s) + Dip(s, 14); }
int NotesLabelY(const PanelState& s) { return TitleY(s) + Dip(s, static_cast<float>(kEditH)) + Dip(s, 6); }
int NotesY(const PanelState& s) { return NotesLabelY(s) + Dip(s, 14); }
int SelectY(const PanelState& s) { return NotesY(s) + Dip(s, 52) + Dip(s, 8); }
int AddY(const PanelState& s) { return SelectY(s) + Dip(s, static_cast<float>(kEditH)) + Dip(s, 10); }
int HintY(const PanelState& s) { return AddY(s) + Dip(s, static_cast<float>(kAddH)) + Dip(s, 8); }

RECT DateRect(const PanelState& s, int x, int w, int y) {
    return RECT{ x, y, x + w * 40 / 100, y + Dip(s, static_cast<float>(kEditH)) };
}
RECT TimeRect(const PanelState& s, int x, int w, int y) {
    const int split = x + w * 40 / 100;
    return RECT{ split + Dip(s, 6), y, split + Dip(s, 6) + w * 26 / 100, y + Dip(s, static_cast<float>(kEditH)) };
}
RECT RepeatRect(const PanelState& s, int x, int w, int y) {
    const int split = x + w * 40 / 100;
    return RECT{ split + Dip(s, 6) + w * 26 / 100 + Dip(s, 6), y, x + w,
                 y + Dip(s, static_cast<float>(kEditH)) };
}

void Relayout(PanelState& s) {
    s.list_h = ListHeight(s);
    s.win_h = HintY(s) + Dip(s, 24);
    const int width = Dip(s, static_cast<float>(kWinW));
    SetWindowPos(s.hwnd, nullptr, 0, 0, width, s.win_h, SWP_NOMOVE | SWP_NOZORDER);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, s.win_h + 1, Dip(s, 12), Dip(s, 12));
    SetWindowRgn(s.hwnd, region, TRUE);
    MoveWindow(s.title_edit, Dip(s, static_cast<float>(kPad)), TitleY(s),
               width - Dip(s, static_cast<float>(kPad * 2)), Dip(s, static_cast<float>(kEditH)), TRUE);
    MoveWindow(s.notes_edit, Dip(s, static_cast<float>(kPad)), NotesY(s),
               width - Dip(s, static_cast<float>(kPad * 2)), Dip(s, 52), TRUE);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void Refresh(PanelState& s) { InvalidateRect(s.hwnd, nullptr, FALSE); }

void Commit(PanelState& s) {
    const std::wstring title = EditText(s.title_edit);
    if (title.empty()) {
        s.error = L"请先填写日程标题。";
        Refresh(s);
        return;
    }
    s.error.clear();
    s.store->Add(title, EditText(s.notes_edit), s.sel_year, s.sel_month, s.sel_day,
                 s.sel_hour, s.sel_minute, s.repeat);
    SetWindowTextW(s.title_edit, L"");
    SetWindowTextW(s.notes_edit, L"");
    const int total = static_cast<int>(VisibleItems(*s.store).size());
    s.scroll = std::max(0, total - kMaxRows);
    SetFocus(s.title_edit);
    Relayout(s);
}

void Paint(PanelState& s) {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(s.hwnd, &ps);
    RECT client{};
    GetClientRect(s.hwnd, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    FillRectColor(dc, client, BgColor(s.dark));

    const int left = Dip(s, static_cast<float>(kPad));
    const int right = Dip(s, static_cast<float>(kWinW - kPad));
    Text(dc, s.font_title, TextColor(s.dark), L"日程", left, 0, right - Dip(s, 34),
         Dip(s, 40), DT_LEFT);
    Text(dc, s.font_title, SubColor(s.dark), L"\x00D7", right - Dip(s, 26), 0,
         right - Dip(s, 4), Dip(s, 40), DT_CENTER | DT_RIGHT);

    const auto ordered = VisibleItems(*s.store);
    const int first = Dip(s, static_cast<float>(kListTop));
    const int row_h = Dip(s, static_cast<float>(kRowH));
    const int row_gap = Dip(s, static_cast<float>(kRowGap));
    if (ordered.empty()) {
        Text(dc, s.font_body, SubColor(s.dark), L"还没有日程。在下面填写一条吧。", left, first,
             right, first + Dip(s, static_cast<float>(kEmptyH)), DT_LEFT);
    } else {
        s.scroll = std::clamp(s.scroll, 0, std::max(0, static_cast<int>(ordered.size()) - kMaxRows));
        for (int row = 0; row < kMaxRows; ++row) {
            const size_t index = static_cast<size_t>(s.scroll + row);
            if (index >= ordered.size()) break;
            const ScheduleItem& item = *ordered[index];
            const int top = first + row * (row_h + row_gap);
            RECT card{ left, top, right, top + row_h };
            RoundFill(dc, card, SurfaceColor(s.dark), Dip(s, 8));
            Text(dc, s.font_small, SubColor(s.dark), ItemWhen(item), left + Dip(s, 12), top,
                 left + Dip(s, 96), top + row_h, DT_LEFT);
            std::wstring title = item.title;
            if (item.repeat != 0) title += std::wstring(L"  —  ") + RepeatName(item.repeat);
            if (item.notes.empty()) {
                Text(dc, s.font_body, TextColor(s.dark), title, left + Dip(s, 102), top,
                     right - Dip(s, 34), top + row_h, DT_LEFT);
            } else {
                Text(dc, s.font_body, TextColor(s.dark), title, left + Dip(s, 102), top + 2,
                     right - Dip(s, 34), top + Dip(s, 22), DT_LEFT);
                Text(dc, s.font_small, SubColor(s.dark), item.notes, left + Dip(s, 102),
                     top + Dip(s, 20), right - Dip(s, 34), top + row_h - 1, DT_LEFT);
            }
            Text(dc, s.font_body, SubColor(s.dark), L"\x00D7", right - Dip(s, 30), top,
                 right - Dip(s, 6), top + row_h, DT_CENTER | DT_RIGHT);
        }
    }

    const int divider = DividerY(s);
    FillRectColor(dc, RECT{ left, divider, right, divider + 1 },
                  s.dark ? RGB(60, 60, 64) : RGB(222, 222, 226));
    Text(dc, s.font_small, SubColor(s.dark), L"标题", left, TitleLabelY(s), right,
         TitleLabelY(s) + Dip(s, 14), DT_LEFT);
    Text(dc, s.font_small, SubColor(s.dark), L"描述", left, NotesLabelY(s), right,
         NotesLabelY(s) + Dip(s, 14), DT_LEFT);

    // Selector row: date, time, repeat.
    const int select_y = SelectY(s);
    const int select_w = right - left;
    auto selector = [&](const RECT& rc, const std::wstring& text, bool accent) {
        RoundFill(dc, rc, s.dark ? RGB(56, 56, 60) : RGB(226, 226, 232), Dip(s, 8));
        Text(dc, s.font_body, accent ? AccentColor(s.dark) : TextColor(s.dark), text,
             rc.left + Dip(s, 10), rc.top, rc.right - Dip(s, 10), rc.bottom, DT_LEFT);
    };
    selector(DateRect(s, left, select_w, select_y), DateText(s.sel_year, s.sel_month, s.sel_day), false);
    selector(TimeRect(s, left, select_w, select_y), TimeText(s.sel_hour, s.sel_minute), false);
    selector(RepeatRect(s, left, select_w, select_y), RepeatName(s.repeat), false);

    RECT add_rc{ left, AddY(s), right, AddY(s) + Dip(s, static_cast<float>(kAddH)) };
    RoundFill(dc, add_rc, AccentColor(s.dark), Dip(s, 8));
    Text(dc, s.font_body, s.dark ? RGB(12, 12, 12) : RGB(255, 255, 255), L"添加日程",
         add_rc.left, add_rc.top, add_rc.right, add_rc.bottom, DT_CENTER);

    if (!s.error.empty())
        Text(dc, s.font_small, DangerColor(s.dark), s.error, left, HintY(s), right,
             HintY(s) + Dip(s, 20), DT_LEFT);

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(s.hwnd, &ps);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                          UINT_PTR, DWORD_PTR ref) {
    auto* s = reinterpret_cast<PanelState*>(ref);
    if (msg == WM_KEYDOWN && wparam == VK_RETURN && hwnd == s->title_edit) {
        Commit(*s);
        return 0;
    }
    if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
        s->done = true;
        return 0;
    }
    if (msg == WM_KEYDOWN && wparam == VK_TAB) {
        // A multi-line EDIT eats Tab; move between the two editors ourselves.
        HWND next = hwnd == s->title_edit ? s->notes_edit : s->title_edit;
        SetFocus(next);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<PanelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        s = static_cast<PanelState*>(create->lpCreateParams);
        s->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, msg, wparam, lparam);

    switch (msg) {
    case WM_PAINT:
        Paint(*s);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, TextColor(s->dark));
        SetBkColor(dc, SurfaceColor(s->dark));
        return reinterpret_cast<LRESULT>(s->edit_brush);
    }
    case WM_NCHITTEST: {
        const POINT pt{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        POINT client = pt;
        ScreenToClient(hwnd, &client);
        // The close button sits inside the drag strip; leave it clickable.
        if (client.y < Dip(*s, 42) && client.x < Dip(*s, static_cast<float>(kWinW - 44)))
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            s->done = true;
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        if (y < Dip(*s, 42) && x >= Dip(*s, static_cast<float>(kWinW - 44))) {
            s->done = true;
            return 0;
        }
        const int left = Dip(*s, static_cast<float>(kPad));
        const int right = Dip(*s, static_cast<float>(kWinW - kPad));
        const int first = Dip(*s, static_cast<float>(kListTop));
        const int row_h = Dip(*s, static_cast<float>(kRowH));
        const int row_gap = Dip(*s, static_cast<float>(kRowGap));
        const auto ordered = VisibleItems(*s->store);
        if (y >= first && y < first + s->list_h) {
            const int row = (y - first) / (row_h + row_gap);
            const int index = s->scroll + row;
            if (x >= Dip(*s, static_cast<float>(kWinW - 44)) && index >= 0 &&
                index < static_cast<int>(ordered.size())) {
                s->store->Remove(ordered[static_cast<size_t>(index)]->id);
                Relayout(*s);
                return 0;
            }
        }
        const int select_y = SelectY(*s);
        const int select_w = right - left;
        if (y >= select_y && y <= select_y + Dip(*s, static_cast<float>(kEditH))) {
            const RECT date_rc = DateRect(*s, left, select_w, select_y);
            const RECT time_rc = TimeRect(*s, left, select_w, select_y);
            const RECT repeat_rc = RepeatRect(*s, left, select_w, select_y);
            if (x >= date_rc.left && x < date_rc.right) {
                POINT pt{ date_rc.left, select_y + Dip(*s, static_cast<float>(kEditH)) + 4 };
                ClientToScreen(hwnd, &pt);
                int year = s->sel_year, month = s->sel_month, day = s->sel_day;
                if (PickDate(hwnd, pt, year, month, day, s->dark, s->scale)) {
                    s->sel_year = year;
                    s->sel_month = month;
                    s->sel_day = day;
                    Refresh(*s);
                }
                return 0;
            }
            if (x >= time_rc.left && x < time_rc.right) {
                POINT pt{ time_rc.left, select_y + Dip(*s, static_cast<float>(kEditH)) + 4 };
                ClientToScreen(hwnd, &pt);
                int hour = s->sel_hour, minute = s->sel_minute;
                if (PickTime(hwnd, pt, hour, minute, s->dark, s->scale)) {
                    s->sel_hour = hour;
                    s->sel_minute = minute;
                    Refresh(*s);
                }
                return 0;
            }
            if (x >= repeat_rc.left && x < repeat_rc.right) {
                std::vector<std::wstring> items{L"不重复", L"每天", L"每周", L"每月"};
                POINT pt{ repeat_rc.left, select_y + Dip(*s, static_cast<float>(kEditH)) + 4 };
                ClientToScreen(hwnd, &pt);
                const int picked = PickListItem(hwnd, pt, items, s->repeat, s->dark, s->scale, 120, 4);
                if (picked >= 0) {
                    s->repeat = picked;
                    Refresh(*s);
                }
                return 0;
            }
        }
        if (x >= left && x <= right && y >= AddY(*s) && y <= AddY(*s) + Dip(*s, static_cast<float>(kAddH))) {
            Commit(*s);
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const int count = static_cast<int>(VisibleItems(*s->store).size());
        s->scroll = std::clamp(s->scroll - (delta > 0 ? 1 : -1), 0, std::max(0, count - kMaxRows));
        Refresh(*s);
        return 0;
    }
    case WM_CLOSE:
        s->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void ShowSchedulePanel(HWND owner, ScheduleStore& store, float scale, bool dark,
                       POINT screen_anchor) {
    PanelState state;
    state.store = &store;
    state.owner = owner;
    state.scale = scale > 0.0f ? scale : 1.0f;
    state.dark = dark;
    state.font_title = MakeFont(15, true, state.scale);
    state.font_body = MakeFont(13, false, state.scale);
    state.font_small = MakeFont(11, false, state.scale);
    state.edit_brush = CreateSolidBrush(SurfaceColor(state.dark));

    SYSTEMTIME now{};
    GetLocalTime(&now);
    state.sel_year = now.wYear;
    state.sel_month = now.wMonth;
    state.sel_day = now.wDay;
    state.sel_hour = std::min(23, now.wHour + 1);
    state.sel_minute = 0;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    const int width = Dip(state, static_cast<float>(kWinW));
    state.list_h = ListHeight(state);
    state.win_h = HintY(state) + Dip(state, 24);
    HMONITOR monitor = MonitorFromPoint(screen_anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    const int x = std::clamp(screen_anchor.x, mi.rcWork.left, mi.rcWork.right - width);
    const int y = std::clamp(screen_anchor.y, mi.rcWork.top, mi.rcWork.bottom - state.win_h);

    state.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kClassName, L"日程",
                                 WS_POPUP, x, y, width, state.win_h, owner, nullptr,
                                 GetModuleHandleW(nullptr), &state);
    if (!state.hwnd) {
        DeleteObject(state.font_title);
        DeleteObject(state.font_body);
        DeleteObject(state.font_small);
        DeleteObject(state.edit_brush);
        return;
    }
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, state.win_h + 1, Dip(state, 12),
                                     Dip(state, 12));
    SetWindowRgn(state.hwnd, region, TRUE);

    state.title_edit = CreateWindowExW(0, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                       0, 0, 0, Dip(state, static_cast<float>(kEditH)),
                                       state.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    state.notes_edit = CreateWindowExW(0, L"EDIT", L"",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                                           ES_AUTOVSCROLL | ES_WANTRETURN,
                                       0, 0, 0, Dip(state, 52), state.hwnd, nullptr,
                                       GetModuleHandleW(nullptr), nullptr);
    for (HWND edit : {state.title_edit, state.notes_edit}) {
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(state.font_body), TRUE);
        SetWindowTheme(edit, L"", L"");
        SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(&state));
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
    }
    MoveWindow(state.title_edit, Dip(state, static_cast<float>(kPad)), TitleY(state),
               width - Dip(state, static_cast<float>(kPad * 2)),
               Dip(state, static_cast<float>(kEditH)), TRUE);
    MoveWindow(state.notes_edit, Dip(state, static_cast<float>(kPad)), NotesY(state),
               width - Dip(state, static_cast<float>(kPad * 2)), Dip(state, 52), TRUE);

    EnableWindow(owner, FALSE);
    ShowWindow(state.hwnd, SW_SHOW);
    state.shown = true;
    SetForegroundWindow(state.hwnd);
    SetFocus(state.title_edit);

    // A click outside the panel (or its pickers) closes it. Focus changes are
    // ignored here: an open picker takes activation, and closing the panel on
    // that would swallow the value the picker is about to return.
    auto owns = [&state](HWND w) {
        if (!w) return true;
        if (w == state.hwnd || IsChild(state.hwnd, w)) return true;
        wchar_t cls[64]{};
        GetClassNameW(w, cls, 64);
        return wcscmp(cls, kListClass) == 0 || wcscmp(cls, kCalendarClass) == 0 ||
               wcscmp(cls, kTimeClass) == 0;
    };
    // The owner is disabled while this runs, so a click on it never reaches the
    // message queue. Poll the physical button so any click outside the card and
    // its pickers still closes the panel. Bit 0 reports presses since the last
    // call, which catches clicks that start and end between two ticks.
    constexpr UINT_PTR kOutsidePoll = 0x5D1;
    const ULONGLONG opened_at = GetTickCount64();
    SetTimer(state.hwnd, kOutsidePoll, 30, nullptr);
    MSG msg{};
    while (!state.done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;
        if (msg.message == WM_TIMER && msg.wParam == kOutsidePoll) {
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x0001) != 0 ||
                (GetAsyncKeyState(VK_RBUTTON) & 0x0001) != 0) {
                // The click that opened the panel is still in the button state;
                // ignore anything that lands before the panel settles.
                if (GetTickCount64() - opened_at < 250) continue;
                POINT pt{};
                HWND under = GetCursorPos(&pt) ? WindowFromPoint(pt) : nullptr;
                bool owned = false;
                for (HWND h = under; h && !owned; h = GetParent(h)) owned = owns(h);
                if (!owned) state.done = true;
            }
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            state.done = true;
            continue;
        }
        // Enter in the title commits; Enter in the multi-line notes inserts a line.
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN &&
            GetFocus() == state.title_edit) {
            Commit(state);
            continue;
        }
        // Tab moves between the two editors.
        if (IsDialogMessageW(state.hwnd, &msg)) continue;
        const bool click = msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                           msg.message == WM_MBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN ||
                           msg.message == WM_NCRBUTTONDOWN || msg.message == WM_NCMBUTTONDOWN;
        if (click && !owns(msg.hwnd)) state.done = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(state.hwnd, kOutsidePoll);

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    DestroyWindow(state.hwnd);
    DeleteObject(state.font_title);
    DeleteObject(state.font_body);
    DeleteObject(state.font_small);
    DeleteObject(state.edit_brush);
}

} // namespace luling::app
