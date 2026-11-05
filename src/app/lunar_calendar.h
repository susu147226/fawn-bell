#pragma once
#include <string>

namespace luling::calendar {

struct LunarDate {
    int year = 0;    // lunar year
    int month = 1;   // 1..12
    int day = 1;     // 1..30
    bool leap = false;
};

// Gregorian to lunar. False outside 1901-02-19 .. 2100-12-31.
bool ToLunar(int year, int month, int day, LunarDate& out);

// Chinese names used by the title card.
std::wstring LunarMonthName(const LunarDate& date);  // 正月 / 闰四月 / 腊月
std::wstring LunarDayName(int day);                  // 初一 .. 三十
std::wstring LunarText(const LunarDate& date);       // 闰四月初三
std::wstring GanzhiYear(int lunar_year);             // 甲辰年
std::wstring Zodiac(int lunar_year);                 // 龙

// Solar term ("立冬") for that day, empty outside 2000-2099.
std::wstring SolarTerm(int year, int month, int day);
// One-line description for a term name ("立冬": "冬季开始，万物收藏").
std::wstring SolarTermDescription(const std::wstring& term);

// Lunar festival first (春节 / 中秋 / 除夕), then 清明, then solar ones.
std::wstring Festival(int year, int month, int day);

// Next festival on or after that day, within a year. days = 0 means today.
bool NextFestival(int year, int month, int day, std::wstring& name, int& days);
// Nearest term before today and the next term on/after today (0 = today).
bool SolarTermsAround(int year, int month, int day, std::wstring& previous,
                      int& previous_days, std::wstring& next, int& next_days);

struct DayInfo {
    int year = 0, month = 0, day = 0;
    int weekday = 0;  // 0 = Sunday
    bool has_lunar = false;
    LunarDate lunar;
    std::wstring lunar_text;
    std::wstring ganzhi;
    std::wstring zodiac;
    std::wstring solar_term;
    std::wstring festival;
};

DayInfo Describe(int year, int month, int day);

} // namespace luling::calendar
