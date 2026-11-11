#include "Srt.h"
#include "Util.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace PerleCast {

namespace {
struct Cue { int64_t start, end; std::string text; };

bool ParseClock(std::string value, int64_t& millis)
{
    for (size_t i = 0; i < value.size(); ++i) if (value[i] == ',') value[i] = '.';
    std::vector<std::string> parts = Util::Split(value, ':');
    if (parts.size() < 2 || parts.size() > 3) return false;
    char* end = 0;
    long long hours = 0;
    if (parts.size() == 3) {
        hours = std::strtoll(parts[0].c_str(), &end, 10);
        if (!end || *end) return false;
    }
    const std::string& minutesText = parts[parts.size() - 2];
    long long minutes = std::strtoll(minutesText.c_str(), &end, 10);
    if (!end || *end) return false;
    double seconds = std::strtod(parts[parts.size() - 1].c_str(), &end);
    if (!end || *end) return false;
    millis = hours * 3600000 + minutes * 60000 + static_cast<int64_t>(std::floor(seconds * 1000.0 + 0.5));
    return true;
}

std::string StripTags(const std::string& value)
{
    std::string result;
    bool tag = false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '<') tag = true;
        else if (value[i] == '>') tag = false;
        else if (!tag) result += value[i];
    }
    return Util::XmlDecode(result);
}

std::vector<Cue> ParseCues(const std::string& input)
{
    std::string text = input;
    for (size_t i = 0; i < text.size(); ++i) if (text[i] == '\r') text[i] = '\n';
    std::vector<Cue> cues;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find("\n\n", start);
        if (end == std::string::npos) end = text.size();
        std::vector<std::string> lines;
        std::string block = text.substr(start, end - start);
        size_t lineStart = 0;
        while (lineStart <= block.size()) {
            size_t lineEnd = block.find('\n', lineStart);
            if (lineEnd == std::string::npos) lineEnd = block.size();
            std::string line = Util::Trim(block.substr(lineStart, lineEnd - lineStart));
            if (!line.empty()) lines.push_back(line);
            if (lineEnd == block.size()) break;
            lineStart = lineEnd + 1;
        }
        size_t timing = lines.size();
        for (size_t i = 0; i < lines.size(); ++i)
            if (lines[i].find("-->") != std::string::npos) { timing = i; break; }
        if (timing < lines.size()) {
            size_t arrow = lines[timing].find("-->");
            std::string left = Util::Trim(lines[timing].substr(0, arrow));
            std::string right = Util::Trim(lines[timing].substr(arrow + 3));
            size_t setting = right.find(' ');
            if (setting != std::string::npos) right.resize(setting);
            int64_t a, b;
            if (ParseClock(left, a) && ParseClock(right, b) && b > a) {
                std::string cueText;
                for (size_t i = timing + 1; i < lines.size(); ++i) {
                    if (!cueText.empty()) cueText += '\n';
                    cueText += Util::Trim(StripTags(lines[i]));
                }
                if (!cueText.empty()) cues.push_back(Cue{a, b, cueText});
            }
        }
        start = end + 2;
    }
    std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    return cues;
}

std::string Write(const std::vector<Cue>& cues)
{
    std::string output;
    for (size_t i = 0; i < cues.size(); ++i) {
        output += std::to_string(i + 1) + "\n" + Srt::FormatTime(cues[i].start) + " --> "
            + Srt::FormatTime(cues[i].end) + "\n" + cues[i].text + "\n\n";
    }
    return output;
}
}

std::string Srt::FromVtt(const std::string& vtt)
{
    return Write(ParseCues(vtt));
}

std::string Srt::Shift(const std::string& srt, int64_t offsetMillis)
{
    std::vector<Cue> source = ParseCues(srt), result;
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i].end <= offsetMillis) continue;
        result.push_back(Cue{std::max<int64_t>(0, source[i].start - offsetMillis),
                             source[i].end - offsetMillis, source[i].text});
    }
    return Write(result);
}

std::string Srt::FormatTime(int64_t millis)
{
    if (millis < 0) millis = 0;
    std::ostringstream out;
    out.fill('0');
    out.width(2); out << millis / 3600000 << ':';
    out.width(2); out << (millis / 60000) % 60 << ':';
    out.width(2); out << (millis / 1000) % 60 << ',';
    out.width(3); out << millis % 1000;
    return out.str();
}

} // namespace PerleCast
