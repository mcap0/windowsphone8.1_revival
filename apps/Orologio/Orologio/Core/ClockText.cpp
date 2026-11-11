#include "ClockText.h"

#include <algorithm>
#include <cwchar>
#include <iomanip>
#include <sstream>

namespace ClockText
{
    namespace
    {
        const int64_t NtpUnixEpochDifferenceMs = 2208988800000LL;
        const int64_t TicksPerSecond = 10000000LL;

        std::wstring TwoDigits(int value)
        {
            std::wostringstream stream;
            stream << std::setw(2) << std::setfill(L'0') << value;
            return stream.str();
        }

        int64_t ClampTicks(int64_t ticks)
        {
            return ticks > 0 ? ticks : 0;
        }
    }

    std::wstring FormatClock(int h, int m, int s)
    {
        std::wostringstream stream;
        stream << std::setw(2) << std::setfill(L'0') << h << L":"
               << std::setw(2) << std::setfill(L'0') << m << L":"
               << std::setw(2) << std::setfill(L'0') << s;
        return stream.str();
    }

    std::wstring FormatStopwatch(int64_t ticks100ns)
    {
        int64_t ticks = ClampTicks(ticks100ns);
        int64_t totalCentiseconds = ticks / 100000LL;
        int64_t totalSeconds = ticks / TicksPerSecond;
        int hours = static_cast<int>(totalSeconds / 3600);
        int minutes = static_cast<int>((totalSeconds / 60) % 60);
        int seconds = static_cast<int>(totalSeconds % 60);
        int centiseconds = static_cast<int>(totalCentiseconds % 100);

        std::wostringstream stream;
        if (hours > 0)
        {
            stream << hours << L":"
                   << std::setw(2) << std::setfill(L'0') << minutes << L":"
                   << std::setw(2) << std::setfill(L'0') << seconds;
        }
        else
        {
            stream << std::setw(2) << std::setfill(L'0') << (totalSeconds / 60) << L":"
                   << std::setw(2) << std::setfill(L'0') << seconds;
        }
        stream << L"," << std::setw(2) << std::setfill(L'0') << centiseconds;
        return stream.str();
    }

    std::wstring FormatTimer(int64_t ticks100ns)
    {
        int64_t ticks = ClampTicks(ticks100ns);
        int64_t seconds = ticks == 0 ? 0 : (ticks - 1) / TicksPerSecond + 1;
        int64_t hours = seconds / 3600;
        int minutes = static_cast<int>((seconds / 60) % 60);
        int remainingSeconds = static_cast<int>(seconds % 60);

        std::wostringstream stream;
        if (hours > 0)
        {
            stream << hours << L":"
                   << std::setw(2) << std::setfill(L'0') << minutes << L":"
                   << std::setw(2) << std::setfill(L'0') << remainingSeconds;
        }
        else
        {
            stream << std::setw(2) << std::setfill(L'0') << (seconds / 60) << L":"
                   << std::setw(2) << std::setfill(L'0') << remainingSeconds;
        }
        return stream.str();
    }

    std::wstring ItalianDate(int weekday0Sunday, int day, int month1, int year)
    {
        static const wchar_t* weekdays[] =
        {
            L"domenica", L"luned\u00ec", L"marted\u00ec", L"mercoled\u00ec",
            L"gioved\u00ec", L"venerd\u00ec", L"sabato"
        };
        static const wchar_t* months[] =
        {
            L"gennaio", L"febbraio", L"marzo", L"aprile", L"maggio", L"giugno",
            L"luglio", L"agosto", L"settembre", L"ottobre", L"novembre", L"dicembre"
        };

        if (weekday0Sunday < 0 || weekday0Sunday > 6) weekday0Sunday = 0;
        if (month1 < 1 || month1 > 12) month1 = 1;

        std::wostringstream stream;
        stream << weekdays[weekday0Sunday] << L" " << day << L" "
               << months[month1 - 1] << L" " << year;
        return stream.str();
    }

    std::wstring OffsetText(int64_t offsetMs)
    {
        if (offsetMs > -50 && offsetMs < 50)
            return L"telefono preciso (\u00b10,05 s)";

        uint64_t magnitude = offsetMs < 0
            ? static_cast<uint64_t>(-(offsetMs + 1)) + 1ULL
            : static_cast<uint64_t>(offsetMs);
        uint64_t hundredths = (magnitude + 5ULL) / 10ULL;

        std::wostringstream stream;
        stream << L"telefono " << (offsetMs > 0 ? L"indietro di " : L"avanti di ")
               << (hundredths / 100ULL) << L","
               << std::setw(2) << std::setfill(L'0') << (hundredths % 100ULL)
               << L" s";
        return stream.str();
    }

    void BuildNtpRequest(uint8_t out[48])
    {
        if (out == nullptr) return;
        for (size_t i = 0; i < 48; ++i) out[i] = 0;
        out[0] = 0x23;
    }

    bool ParseNtpTransmit(const uint8_t* data, size_t length, int64_t& ntpMsSince1900)
    {
        if (data == nullptr || length < 48) return false;
        if ((data[0] & 0x07) != 4 || data[1] == 0) return false;

        uint64_t seconds = (static_cast<uint64_t>(data[40]) << 24) |
                           (static_cast<uint64_t>(data[41]) << 16) |
                           (static_cast<uint64_t>(data[42]) << 8) |
                           static_cast<uint64_t>(data[43]);
        uint64_t fraction = (static_cast<uint64_t>(data[44]) << 24) |
                            (static_cast<uint64_t>(data[45]) << 16) |
                            (static_cast<uint64_t>(data[46]) << 8) |
                            static_cast<uint64_t>(data[47]);

        ntpMsSince1900 = static_cast<int64_t>(seconds * 1000ULL +
                                               ((fraction * 1000ULL) >> 32));
        return true;
    }

    int64_t NtpOffsetMs(int64_t t0, int64_t t1, int64_t t2, int64_t t3)
    {
        return ((t1 - t0) + (t2 - t3)) / 2;
    }

    int64_t UnixMsFromNtpMs(int64_t ntpMs)
    {
        return ntpMs - NtpUnixEpochDifferenceMs;
    }

    std::wstring JoinLaps(const std::vector<int64_t>& laps)
    {
        std::wostringstream stream;
        for (size_t i = 0; i < laps.size(); ++i)
        {
            if (i != 0) stream << L",";
            stream << laps[i];
        }
        return stream.str();
    }

    std::vector<int64_t> SplitLaps(const std::wstring& serialized)
    {
        std::vector<int64_t> result;
        size_t begin = 0;
        while (begin <= serialized.size())
        {
            size_t end = serialized.find(L',', begin);
            if (end == std::wstring::npos) end = serialized.size();

            std::wstring token = serialized.substr(begin, end - begin);
            if (!token.empty())
            {
                const wchar_t* first = token.c_str();
                wchar_t* parsedEnd = nullptr;
                long long value = std::wcstoll(first, &parsedEnd, 10);
                while (parsedEnd != nullptr && *parsedEnd != L'\0' &&
                       (*parsedEnd == L' ' || *parsedEnd == L'\t'))
                    ++parsedEnd;
                if (parsedEnd != first && parsedEnd != nullptr && *parsedEnd == L'\0')
                    result.push_back(static_cast<int64_t>(value));
            }

            if (end == serialized.size()) break;
            begin = end + 1;
        }
        return result;
    }
}
