#include "lunar_calendar.h"
#include "lunar_table.inc"

#include <cstdint>

namespace luling::calendar {
namespace {

constexpr int kFirstYear = 1901;
constexpr int kLastYear = 2100;

// Days since 1970-01-01 (Howard Hinnant's civil-days algorithm).
int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

int LunarYearDays(int index) {
    const uint32_t bits = kLunarMonthBits[index];
    int days = 0;
    for (int i = 0; i < 12; ++i) days += ((bits >> i) & 1) ? 30 : 29;
    if (((bits >> 12) & 0xF) != 0) days += ((bits >> 16) & 1) ? 30 : 29;
    return days;
}

int LunarMonthDays(int lunar_year, int month, bool leap) {
    if (lunar_year < kFirstYear || lunar_year > kLastYear || month < 1 || month > 12) return 29;
    const uint32_t bits = kLunarMonthBits[lunar_year - kFirstYear];
    if (leap) return ((bits >> 16) & 1) ? 30 : 29;
    return ((bits >> (month - 1)) & 1) ? 30 : 29;
}

void AddDays(int year, int month, int day, int count, int& out_year, int& out_month, int& out_day) {
    int64_t z = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) + count;
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    out_day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    out_month = static_cast<int>(mp + (mp < 10 ? 3 : -9));
    out_year = static_cast<int>(yoe) + static_cast<int>(era) * 400 + (out_month <= 2 ? 1 : 0);
}

const wchar_t* const kMonthNames[13] = {
    L"", L"正月", L"二月", L"三月", L"四月", L"五月", L"六月",
    L"七月", L"八月", L"九月", L"十月", L"冬月", L"腊月",
};

const wchar_t* const kDayNames[31] = {
    L"", L"初一", L"初二", L"初三", L"初四", L"初五", L"初六", L"初七", L"初八", L"初九", L"初十",
    L"十一", L"十二", L"十三", L"十四", L"十五", L"十六", L"十七", L"十八", L"十九", L"二十",
    L"廿一", L"廿二", L"廿三", L"廿四", L"廿五", L"廿六", L"廿七", L"廿八", L"廿九", L"三十",
};

const wchar_t* const kGan[10] = {L"甲", L"乙", L"丙", L"丁", L"戊", L"己", L"庚", L"辛", L"壬", L"癸"};
const wchar_t* const kZhi[12] = {L"子", L"丑", L"寅", L"卯", L"辰", L"巳",
                                 L"午", L"未", L"申", L"酉", L"戌", L"亥"};
const wchar_t* const kZodiac[12] = {L"鼠", L"牛", L"虎", L"兔", L"龙", L"蛇",
                                    L"马", L"羊", L"猴", L"鸡", L"狗", L"猪"};

const wchar_t* const kTermNames[24] = {
    L"小寒", L"大寒", L"立春", L"雨水", L"惊蛰", L"春分",
    L"清明", L"谷雨", L"立夏", L"小满", L"芒种", L"夏至",
    L"小暑", L"大暑", L"立秋", L"处暑", L"白露", L"秋分",
    L"寒露", L"霜降", L"立冬", L"小雪", L"大雪", L"冬至",
};

// C values of the 21st-century solar-term formula, one per half-month.
constexpr double kTermC[24] = {
    5.4055, 20.12, 3.87, 18.73, 5.63, 20.646,
    4.81, 20.1, 5.52, 21.04, 5.678, 21.37,
    7.108, 22.83, 7.5, 23.13, 7.646, 23.042,
    8.318, 23.438, 7.438, 22.36, 7.18, 21.94,
};

std::wstring LunarFestival(const LunarDate& date) {
    if (date.leap) return {};  // leap months never carry the festival
    if (date.month == 1 && date.day == 1) return L"春节";
    if (date.month == 1 && date.day == 15) return L"元宵节";
    if (date.month == 2 && date.day == 2) return L"龙抬头";
    if (date.month == 5 && date.day == 5) return L"端午节";
    if (date.month == 7 && date.day == 7) return L"七夕";
    if (date.month == 7 && date.day == 15) return L"中元节";
    if (date.month == 8 && date.day == 15) return L"中秋节";
    if (date.month == 9 && date.day == 9) return L"重阳节";
    if (date.month == 12 && date.day == 8) return L"腊八节";
    if (date.month == 12 && date.day == 23) return L"小年";
    if (date.month == 12 && date.day == LunarMonthDays(date.year, 12, false)) return L"除夕";
    return {};
}

const wchar_t* SolarFestival(int month, int day) {
    if (month == 1 && day == 1) return L"元旦";
    if (month == 2 && day == 14) return L"情人节";
    if (month == 3 && day == 8) return L"妇女节";
    if (month == 3 && day == 12) return L"植树节";
    if (month == 4 && day == 1) return L"愚人节";
    if (month == 5 && day == 1) return L"劳动节";
    if (month == 5 && day == 4) return L"青年节";
    if (month == 6 && day == 1) return L"儿童节";
    if (month == 7 && day == 1) return L"建党节";
    if (month == 8 && day == 1) return L"建军节";
    if (month == 9 && day == 10) return L"教师节";
    if (month == 10 && day == 1) return L"国庆节";
    if (month == 12 && day == 25) return L"圣诞节";
    return nullptr;
}

} // namespace

bool ToLunar(int year, int month, int day, LunarDate& out) {
    if (year < kFirstYear || year > kLastYear || month < 1 || month > 12 || day < 1 || day > 31)
        return false;
    const int64_t target = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    const int64_t start = DaysFromCivil(kFirstYear, kLunarSpring[0] >> 8, kLunarSpring[0] & 0xFF);
    if (target < start) return false;
    int64_t offset = target - start;
    int index = 0;
    while (index < 200 && offset >= LunarYearDays(index)) {
        offset -= LunarYearDays(index);
        ++index;
    }
    if (index >= 200) return false;
    out.year = kFirstYear + index;
    const uint32_t bits = kLunarMonthBits[index];
    const int leap = (bits >> 12) & 0xF;
    for (int m = 1; m <= 12; ++m) {
        int len = LunarMonthDays(out.year, m, false);
        if (offset < len) {
            out.month = m;
            out.day = static_cast<int>(offset) + 1;
            out.leap = false;
            return true;
        }
        offset -= len;
        if (leap == m) {
            len = LunarMonthDays(out.year, m, true);
            if (offset < len) {
                out.month = m;
                out.day = static_cast<int>(offset) + 1;
                out.leap = true;
                return true;
            }
            offset -= len;
        }
    }
    return false;
}

std::wstring LunarMonthName(const LunarDate& date) {
    if (date.month < 1 || date.month > 12) return {};
    std::wstring name = date.leap ? L"闰" : L"";
    name += kMonthNames[date.month];
    return name;
}

std::wstring LunarDayName(int day) {
    if (day < 1 || day > 30) return {};
    return kDayNames[day];
}

std::wstring LunarText(const LunarDate& date) {
    if (date.month < 1 || date.month > 12 || date.day < 1 || date.day > 30) return {};
    return LunarMonthName(date) + LunarDayName(date.day);
}

std::wstring GanzhiYear(int lunar_year) {
    if (lunar_year < kFirstYear || lunar_year > kLastYear) return {};
    const int index = (lunar_year - 4) % 60;
    return std::wstring(kGan[index % 10]) + kZhi[index % 12] + L"年";
}

std::wstring Zodiac(int lunar_year) {
    if (lunar_year < kFirstYear || lunar_year > kLastYear) return {};
    return kZodiac[(lunar_year - 4) % 12];
}

std::wstring SolarTerm(int year, int month, int day) {
    if (year < 2000 || year > 2099 || month < 1 || month > 12) return {};
    const int y = year % 100;
    for (int half = 0; half < 2; ++half) {
        const int term = (month - 1) * 2 + half;
        const int leap_years = (month <= 2) ? (y - 1) / 4 : y / 4;
        const int term_day = static_cast<int>(y * 0.2422 + kTermC[term]) - leap_years;
        if (term_day == day) return kTermNames[term];
    }
    return {};
}

std::wstring Festival(int year, int month, int day) {
    LunarDate lunar;
    if (ToLunar(year, month, day, lunar)) {
        const std::wstring name = LunarFestival(lunar);
        if (!name.empty()) return name;
    }
    if (SolarTerm(year, month, day) == L"清明") return L"清明节";
    if (const wchar_t* name = SolarFestival(month, day)) return name;
    return {};
}

std::wstring SolarTermDescription(const std::wstring& term) {
    if (term == L"小寒") return L"一年中最冷时段的开始";
    if (term == L"大寒") return L"寒气最盛，年味渐浓";
    if (term == L"立春") return L"万物复苏，春意萌动";
    if (term == L"雨水") return L"降雨增多，草木萌发";
    if (term == L"惊蛰") return L"春雷始鸣，蛰虫苏醒";
    if (term == L"春分") return L"昼夜等长，春暖花开";
    if (term == L"清明") return L"天清地明，踏青祭祖";
    if (term == L"谷雨") return L"雨生百谷，播种时节";
    if (term == L"立夏") return L"夏季开始，万物繁茂";
    if (term == L"小满") return L"麦粒渐满，雨水增多";
    if (term == L"芒种") return L"有芒作物成熟，忙收忙种";
    if (term == L"夏至") return L"白昼最长，盛夏来临";
    if (term == L"小暑") return L"天气转热，尚未酷热";
    if (term == L"大暑") return L"一年中最热的时段";
    if (term == L"立秋") return L"秋季开始，暑热渐退";
    if (term == L"处暑") return L"暑气终止，秋凉初显";
    if (term == L"白露") return L"昼夜温差大，露水凝白";
    if (term == L"秋分") return L"昼夜等长，秋色平分";
    if (term == L"寒露") return L"露水寒凉，秋意更浓";
    if (term == L"霜降") return L"开始降霜，深秋将尽";
    if (term == L"立冬") return L"冬季开始，万物收藏";
    if (term == L"小雪") return L"开始降雪，天寒地冻";
    if (term == L"大雪") return L"降雪增多，冰封大地";
    if (term == L"冬至") return L"白昼最短，数九开始";
    return {};
}

bool NextFestival(int year, int month, int day, std::wstring& name, int& days) {
    for (int n = 0; n <= 400; ++n) {
        int y = 0, m = 0, d = 0;
        AddDays(year, month, day, n, y, m, d);
        const std::wstring festival = Festival(y, m, d);
        if (festival.empty()) continue;
        name = festival;
        days = n;
        return true;
    }
    return false;
}

bool SolarTermsAround(int year, int month, int day, std::wstring& previous,
                      int& previous_days, std::wstring& next, int& next_days) {
    previous.clear();
    next.clear();
    previous_days = 0;
    next_days = 0;
    for (int n = 1; n <= 40 && previous.empty(); ++n) {
        int y = 0, m = 0, d = 0;
        AddDays(year, month, day, -n, y, m, d);
        previous = SolarTerm(y, m, d);
        previous_days = n;
    }
    if (previous.empty()) previous_days = 0;
    for (int n = 0; n <= 40 && next.empty(); ++n) {
        int y = 0, m = 0, d = 0;
        AddDays(year, month, day, n, y, m, d);
        next = SolarTerm(y, m, d);
        next_days = n;
    }
    if (next.empty()) next_days = 0;
    return !previous.empty() || !next.empty();
}

DayInfo Describe(int year, int month, int day) {
    DayInfo info;
    info.year = year;
    info.month = month;
    info.day = day;
    if (month >= 1 && month <= 12 && day >= 1 && day <= 31)
        info.weekday = static_cast<int>((DaysFromCivil(year, static_cast<unsigned>(month),
                                                       static_cast<unsigned>(day)) + 4) % 7);
    info.has_lunar = ToLunar(year, month, day, info.lunar);
    if (info.has_lunar) {
        info.lunar_text = LunarText(info.lunar);
        info.ganzhi = GanzhiYear(info.lunar.year);
        info.zodiac = Zodiac(info.lunar.year);
    }
    info.solar_term = SolarTerm(year, month, day);
    info.festival = Festival(year, month, day);
    return info;
}

} // namespace luling::calendar
