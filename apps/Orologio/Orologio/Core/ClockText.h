#pragma once
// Pure C++ text and NTP helpers of Orologio (no Windows headers): tested by tests/clocktext_tests.cpp.

#include <cstddef>
#include <stdint.h>
#include <string>
#include <vector>

namespace ClockText
{
    std::wstring FormatClock(int h, int m, int s);                 // "07:05:09"
    std::wstring FormatStopwatch(int64_t ticks100ns);               // "01:02,10" or "1:01:02,10"
    std::wstring FormatTimer(int64_t ticks100ns);                   // rounded up to the second: "05:00", "1:00:00"
    std::wstring ItalianDate(int weekday0Sunday, int day, int month1, int year);  // "lunedì 20 ottobre 2026"
    std::wstring OffsetText(int64_t offsetMs);                      // positive offset = the phone is behind

    void BuildNtpRequest(uint8_t out[48]);
    bool ParseNtpTransmit(const uint8_t* data, size_t length, int64_t& ntpMsSince1900);
    int64_t NtpOffsetMs(int64_t t0, int64_t t1, int64_t t2, int64_t t3);
    int64_t UnixMsFromNtpMs(int64_t ntpMs);

    std::wstring JoinLaps(const std::vector<int64_t>& laps);
    std::vector<int64_t> SplitLaps(const std::wstring& serialized);
}
