#include "Test.h"
#include "../core/Srt.h"

int RunSrtTests()
{
    using namespace PerleCast;
    const std::string vtt = "WEBVTT\n\n00:00:00.500 --> 00:00:02.000 align:start\n<c>Hi</c> there\n\n01:02.000 --> 01:03.250\nshort &amp; sweet\n";
    std::string expected = "1\n00:00:00,500 --> 00:00:02,000\nHi there\n\n2\n00:01:02,000 --> 00:01:03,250\nshort & sweet\n\n";
    Equal(expected, Srt::FromVtt(vtt), "converts VTT cues to SRT");
    Equal(std::string("01:02:03,004"), Srt::FormatTime(3723004), "formats SRT time");
    return TestFailures();
}
