#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace luling::app {

struct ScheduleItem {
    std::wstring id;
    std::wstring title;
    std::wstring notes;
    int year = 0, month = 0, day = 0;
    int hour = 0, minute = 0;
    int repeat = 0;      // 0 once, 1 daily, 2 weekly, 3 monthly
    std::wstring fired;  // "yyyy-mm-dd" of the last reminder
};

// A small local schedule list stored as UTF-8 under %LOCALAPPDATA%\Luling.
class ScheduleStore {
public:
    // False in screenshots/isolated tests: nothing is read from or written to
    // the user's data directory.
    bool persist = true;
    bool Load();
    bool Save() const;

    const std::vector<ScheduleItem>& Items() const noexcept { return items_; }
    std::wstring Add(const std::wstring& title, const std::wstring& notes,
                     int year, int month, int day, int hour, int minute, int repeat);
    bool Update(const ScheduleItem& item);
    bool Remove(const std::wstring& id);

    // True when an item occurs on that date (calendar marking).
    bool HasOn(int year, int month, int day) const;
    // The earliest item whose time has passed today and that has not fired yet.
    const ScheduleItem* Due(const SYSTEMTIME& now) const;
    bool MarkFired(const std::wstring& id, const SYSTEMTIME& now);

private:
    std::wstring Path() const;
    std::vector<ScheduleItem> items_;
};

} // namespace luling::app
