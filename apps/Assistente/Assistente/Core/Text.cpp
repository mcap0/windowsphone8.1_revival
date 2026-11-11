#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "Text.h"

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif

#include <cstdio>
#include <cstdlib>

namespace assist {

std::string PercentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string GoogleSearchUrl(const std::string& query) {
    // the query itself: spaces as '+', like urlencode({"q": ...})
    std::string q;
    std::string enc = PercentEncode(query);
    for (size_t i = 0; i < enc.size(); ++i) {
        if (enc.compare(i, 3, "%20") == 0) {
            q += '+';
            i += 2;
        } else {
            q += enc[i];
        }
    }
    return "https://www.google.com/search?q=" + q;
}

bool ParseHourMinute(const std::string& text, int& hour, int& minute) {
    const char* p = text.c_str();
    while (*p == ' ') ++p;
    char* end = nullptr;
    long h = std::strtol(p, &end, 10);
    if (end == p) return false;
    long m = 0;
    if (*end == ':' || *end == '.') {
        const char* q = end + 1;
        m = std::strtol(q, &end, 10);
        if (end == q) return false;
    }
    while (*end == ' ') ++end;
    if (*end != '\0') return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    hour = static_cast<int>(h);
    minute = static_cast<int>(m);
    return true;
}

long long DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

void CivilFromDays(long long z, int& y, int& m, int& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yy = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(yy + (m <= 2));
}

bool AlarmMoment(const LocalTime& now, int hour, int minute, const std::string& day, LocalTime& when) {
    long long today = DaysFromCivil(now.year, now.month, now.day);
    long long target = today;
    long long nowMinutes = now.hour * 60 + now.minute;
    long long alarmMinutes = hour * 60 + minute;
    if (day.empty()) {
        if (alarmMinutes <= nowMinutes) target = today + 1;
    } else if (day == "oggi") {
        target = today;
    } else if (day == "domani") {
        target = today + 1;
    } else if (day == "dopodomani") {
        target = today + 2;
    } else {
        int y = 0, m = 0, d = 0;
        if (sscanf(day.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return false;
        target = DaysFromCivil(y, m, d);
    }
    if (target < today || (target == today && alarmMinutes <= nowMinutes)) return false;
    CivilFromDays(target, when.year, when.month, when.day);
    when.hour = hour;
    when.minute = minute;
    when.second = 0;
    long long wd = (target + 4) % 7;  // 1970-01-01 was a Thursday
    when.weekday = static_cast<int>(wd < 0 ? wd + 7 : wd);
    return true;
}

std::string ItalianNow(const LocalTime& now) {
    static const char* days[] = {"domenica", "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato"};
    static const char* months[] = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio",
                                   "agosto", "settembre", "ottobre", "novembre", "dicembre"};
    char b[128];
    snprintf(b, sizeof(b), "Oggi è %s %d %s %d, ore %02d:%02d", days[now.weekday % 7], now.day,
                  months[(now.month - 1) % 12], now.year, now.hour, now.minute);
    return b;
}

}  // namespace assist
