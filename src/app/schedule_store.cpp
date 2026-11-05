#include "schedule_store.h"

#include "session.h"
#include "../common/utf8_file.h"

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace luling::app {
namespace {

int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int WeekdayOf(int y, int m, int d) {
    return static_cast<int>((DaysFromCivil(y, static_cast<unsigned>(m),
                                           static_cast<unsigned>(d)) + 4) % 7);
}

std::wstring EscapeField(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') out += L"\\r";
        else out += c;
    }
    return out;
}

std::wstring UnescapeField(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != L'\\' || i + 1 >= in.size()) {
            out += in[i];
            continue;
        }
        const wchar_t next = in[++i];
        if (next == L't') out += L'\t';
        else if (next == L'n') out += L'\n';
        else if (next == L'r') out += L'\r';
        else out += next;
    }
    return out;
}

std::wstring DateKey(int y, int m, int d) {
    wchar_t text[24]{};
    swprintf_s(text, L"%04d-%02d-%02d", y, m, d);
    return text;
}

bool ParseInt(const std::wstring& text, int& value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const long parsed = wcstol(text.c_str(), &end, 10);
    if (!end || *end != L'\0') return false;
    value = static_cast<int>(parsed);
    return true;
}

bool OccursOn(const ScheduleItem& item, int year, int month, int day) {
    const int64_t date = static_cast<int64_t>(year) * 10000 + month * 100 + day;
    const int64_t start = static_cast<int64_t>(item.year) * 10000 + item.month * 100 + item.day;
    if (date < start) return false;
    switch (item.repeat) {
    case 1: return true;
    case 2: return WeekdayOf(year, month, day) == WeekdayOf(item.year, item.month, item.day);
    case 3: return day == item.day;
    default: return date == start;
    }
}

} // namespace

std::wstring ScheduleStore::Path() const {
    const std::wstring dir = GetLulingDataDir();
    if (dir.empty()) return {};
    return dir + L"\\schedule.dat";
}

bool ScheduleStore::Load() {
    items_.clear();
    if (!persist) return true;
    const std::wstring path = Path();
    if (path.empty()) return false;
    std::wstring text;
    if (!ReadUtf8File(path, text)) return true;  // absent on first run
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(start, end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        start = end + 1;
        if (line.empty() || line[0] == L'#') continue;
        std::vector<std::wstring> fields;
        size_t pos = 0;
        while (true) {
            const size_t at = line.find(L'\t', pos);
            fields.push_back(line.substr(pos, at == std::wstring::npos ? std::wstring::npos : at - pos));
            if (at == std::wstring::npos) break;
            pos = at + 1;
        }
        if (fields.size() < 6) continue;
        ScheduleItem item;
        item.id = fields[0];
        int y = 0, m = 0, d = 0, hour = 0, minute = 0;
        if (!ParseInt(fields[1].substr(0, 4), y) || !ParseInt(fields[1].substr(5, 2), m) ||
            !ParseInt(fields[1].substr(8, 2), d) ||
            !ParseInt(fields[2].substr(0, 2), hour) || !ParseInt(fields[2].substr(3, 2), minute))
            continue;
        int repeat = 0;
        ParseInt(fields[3], repeat);
        item.year = y;
        item.month = m;
        item.day = d;
        item.hour = hour;
        item.minute = minute;
        item.repeat = std::clamp(repeat, 0, 3);
        item.fired = fields[4];
        item.title = UnescapeField(fields[5]);
        if (fields.size() > 6) item.notes = UnescapeField(fields[6]);
        if (!item.id.empty() && !item.title.empty()) items_.push_back(std::move(item));
    }
    // One-off items drop out once their day has passed; repeating ones stay.
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const int today = now.wYear * 10000 + now.wMonth * 100 + now.wDay;
    const size_t before = items_.size();
    items_.erase(std::remove_if(items_.begin(), items_.end(), [today](const ScheduleItem& item) {
        return item.repeat == 0 && item.year * 10000 + item.month * 100 + item.day < today;
    }), items_.end());
    if (items_.size() != before) Save();
    return true;
}

bool ScheduleStore::Save() const {
    if (!persist) return true;
    const std::wstring path = Path();
    if (path.empty()) return false;
    std::wstring text = L"# luling schedule v1\n";
    for (const auto& item : items_) {
        wchar_t time[16]{};
        swprintf_s(time, L"%02d:%02d", item.hour, item.minute);
        text += item.id + L"\t" + DateKey(item.year, item.month, item.day) + L"\t" + time +
                L"\t" + std::to_wstring(item.repeat) + L"\t" + item.fired + L"\t" +
                EscapeField(item.title) + L"\t" + EscapeField(item.notes) + L"\n";
    }
    return WriteUtf8FileAtomic(path, text);
}

std::wstring ScheduleStore::Add(const std::wstring& title, const std::wstring& notes,
                                int year, int month, int day, int hour, int minute, int repeat) {
    static std::atomic<unsigned> counter{0};
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const uint64_t stamp = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    wchar_t id[48]{};
    swprintf_s(id, L"%016llx-%u", static_cast<unsigned long long>(stamp), counter.fetch_add(1));
    ScheduleItem item;
    item.id = id;
    item.title = title;
    item.notes = notes;
    item.year = year;
    item.month = month;
    item.day = day;
    item.hour = std::clamp(hour, 0, 23);
    item.minute = std::clamp(minute, 0, 59);
    item.repeat = std::clamp(repeat, 0, 3);
    items_.push_back(std::move(item));
    Save();
    return items_.back().id;
}

bool ScheduleStore::Update(const ScheduleItem& item) {
    for (auto& existing : items_) {
        if (existing.id != item.id) continue;
        existing = item;
        existing.repeat = std::clamp(existing.repeat, 0, 3);
        return Save();
    }
    return false;
}

bool ScheduleStore::Remove(const std::wstring& id) {
    const auto it = std::find_if(items_.begin(), items_.end(),
                                 [&](const ScheduleItem& item) { return item.id == id; });
    if (it == items_.end()) return false;
    items_.erase(it);
    return Save();
}

bool ScheduleStore::HasOn(int year, int month, int day) const {
    for (const auto& item : items_)
        if (OccursOn(item, year, month, day)) return true;
    return false;
}

const ScheduleItem* ScheduleStore::Due(const SYSTEMTIME& now) const {
    const std::wstring today = DateKey(now.wYear, now.wMonth, now.wDay);
    const int minutes = now.wHour * 60 + now.wMinute;
    const ScheduleItem* best = nullptr;
    int best_minutes = 24 * 60;
    for (const auto& item : items_) {
        if (item.fired == today) continue;
        if (!OccursOn(item, now.wYear, now.wMonth, now.wDay)) continue;
        const int at = item.hour * 60 + item.minute;
        if (at > minutes || at >= best_minutes) continue;
        best = &item;
        best_minutes = at;
    }
    return best;
}

bool ScheduleStore::MarkFired(const std::wstring& id, const SYSTEMTIME& now) {
    for (auto& item : items_) {
        if (item.id != id) continue;
        item.fired = DateKey(now.wYear, now.wMonth, now.wDay);
        return Save();
    }
    return false;
}

} // namespace luling::app
