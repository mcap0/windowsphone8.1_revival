// Text helpers for the assistant (plain C++11): URI building for the apps it drives and alarm time parsing.
#pragma once

#include <string>

namespace assist {

// UTF-8 percent-encoding of everything except A-Z a-z 0-9 - _ . ~ (like Python's quote(s, safe="")).
std::string PercentEncode(const std::string& utf8);
// Google results page, opened by the system browser (IE).
std::string GoogleSearchUrl(const std::string& query);

struct LocalTime {
    int year, month, day, hour, minute, second;
    int weekday;  // 0 = Sunday
};

// "7", "7:30", "07.30", "19:05" -> hour/minute.
bool ParseHourMinute(const std::string& text, int& hour, int& minute);
// When the alarm rings: day = "" (today if still ahead, else tomorrow), "oggi", "domani", "dopodomani" or
// "YYYY-MM-DD". Returns false for an unknown day or a moment already past.
bool AlarmMoment(const LocalTime& now, int hour, int minute, const std::string& day, LocalTime& when);
// Days from 1970-01-01 (civil calendar), used for date arithmetic and weekday.
long long DaysFromCivil(int y, int m, int d);
void CivilFromDays(long long z, int& y, int& m, int& d);

// "Oggi è martedì 21 ottobre 2026, ore 13:20" for the system prompt.
std::string ItalianNow(const LocalTime& now);

}  // namespace assist
