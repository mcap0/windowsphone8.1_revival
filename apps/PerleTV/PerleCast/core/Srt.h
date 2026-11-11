#ifndef PERLECAST_SRT_H
#define PERLECAST_SRT_H

#include <cstdint>
#include <string>

namespace PerleCast {

class Srt {
public:
    static std::string FromVtt(const std::string& vtt);
    static std::string Shift(const std::string& srt, int64_t offsetMillis);
    static std::string FormatTime(int64_t millis);
};

} // namespace PerleCast

#endif
