#include "../Core/ClockText.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

int main()
{
    int tests = 0;
#define CHECK(expression) do { assert(expression); ++tests; } while (0)

    CHECK(ClockText::FormatClock(7, 5, 9) == L"07:05:09");
    CHECK(ClockText::FormatClock(23, 0, 1) == L"23:00:01");

    CHECK(ClockText::FormatStopwatch(624500000LL) == L"01:02,45");
    CHECK(ClockText::FormatStopwatch(36624500000LL) == L"1:01:02,45");
    CHECK(ClockText::FormatStopwatch(-1) == L"00:00,00");

    CHECK(ClockText::FormatTimer(1) == L"00:01");
    CHECK(ClockText::FormatTimer(300LL * 10000000LL) == L"05:00");
    CHECK(ClockText::FormatTimer(36000000000LL) == L"1:00:00");
    CHECK(ClockText::FormatTimer(0) == L"00:00");

    CHECK(ClockText::ItalianDate(1, 20, 10, 2026) == L"luned\u00ec 20 ottobre 2026");
    CHECK(ClockText::ItalianDate(0, 1, 1, 2024) == L"domenica 1 gennaio 2024");

    CHECK(ClockText::OffsetText(1234) == L"telefono indietro di 1,23 s");
    CHECK(ClockText::OffsetText(-456) == L"telefono avanti di 0,46 s");
    CHECK(ClockText::OffsetText(49) == L"telefono preciso (\u00b10,05 s)");

    uint8_t request[48];
    ClockText::BuildNtpRequest(request);
    CHECK(request[0] == 0x23);
    bool requestZeroed = true;
    for (size_t i = 1; i < 48; ++i)
        if (request[i] != 0) requestZeroed = false;
    CHECK(requestZeroed);

    uint8_t response[48] = {};
    response[0] = 0x24;
    response[1] = 2;
    response[40] = 0x83;
    response[41] = 0xAA;
    response[42] = 0x7E;
    response[43] = 0x80;
    response[44] = 0x80;
    int64_t parsed = 0;
    CHECK(ClockText::ParseNtpTransmit(response, sizeof(response), parsed));
    CHECK(parsed > 0);
    CHECK(!ClockText::ParseNtpTransmit(response, 47, parsed));
    response[0] = 0x23;
    CHECK(!ClockText::ParseNtpTransmit(response, sizeof(response), parsed));
    response[0] = 0x24;
    response[1] = 0;
    CHECK(!ClockText::ParseNtpTransmit(response, sizeof(response), parsed));

    CHECK(ClockText::NtpOffsetMs(1000, 1020, 1025, 1005) == 20);
    CHECK(ClockText::NtpOffsetMs(1000, 990, 995, 1005) == -10);
    CHECK(ClockText::UnixMsFromNtpMs(2208988800000LL) == 0);
    CHECK(ClockText::UnixMsFromNtpMs(2208988801234LL) == 1234);

    std::vector<int64_t> laps;
    laps.push_back(10000000LL);
    laps.push_back(625000000LL);
    CHECK(ClockText::JoinLaps(laps) == L"10000000,625000000");

    std::vector<int64_t> split = ClockText::SplitLaps(L"10000000,625000000");
    CHECK(split.size() == 2);
    CHECK(split[0] == 10000000LL);
    CHECK(split[1] == 625000000LL);
    CHECK(ClockText::SplitLaps(L"").empty());
    CHECK(ClockText::SplitLaps(L"12,x,34").size() == 2);

#undef CHECK
    std::cout << "OK " << tests << " tests" << std::endl;
    return 0;
}
