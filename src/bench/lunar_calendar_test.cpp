#include "../app/lunar_calendar.h"

#include <cstdio>
#include <string>

using namespace luling::calendar;

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 2026: the year the card showed "立冬15天—今天—小雪" on 小雪 itself.
    Check(Describe(2026, 11, 22).solar_term == L"小雪", "小雪 falls on 2026-11-22");
    Check(Describe(2026, 11, 21).solar_term.empty(), "2026-11-21 is not a term day");
    Check(Describe(2026, 11, 23).solar_term.empty(), "2026-11-23 is not a term day");
    Check(Describe(2026, 11, 7).solar_term == L"立冬", "立冬 falls on 2026-11-07");
    Check(Describe(2026, 12, 7).solar_term == L"大雪", "大雪 falls on 2026-12-07");
    Check(Describe(2026, 4, 5).solar_term == L"清明", "清明 falls on 2026-04-05");

    // Every term has the description the term-day card now shows.
    for (int month = 1; month <= 12; ++month) {
        for (int day = 1; day <= 31; ++day) {
            const std::wstring term = SolarTerm(2026, month, day);
            if (term.empty()) continue;
            if (SolarTermDescription(term).empty()) {
                std::printf("[FAIL] no description for %ls\n", term.c_str());
                ++failures;
            }
        }
    }

    // All 24 terms of 2026 resolve to exactly one day each.
    int found = 0;
    for (int month = 1; month <= 12; ++month)
        for (int day = 1; day <= 31; ++day)
            if (!SolarTerm(2026, month, day).empty()) ++found;
    Check(found == 24, "2026 has exactly 24 term days");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
