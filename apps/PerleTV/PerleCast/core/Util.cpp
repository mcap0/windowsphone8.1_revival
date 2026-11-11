#include "Util.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace PerleCast {
namespace Util {

std::string Trim(const std::string& value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

std::string Lower(const std::string& value)
{
    std::string result(value);
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[i])));
    return result;
}

std::string Upper(const std::string& value)
{
    std::string result(value);
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[i])));
    return result;
}

std::vector<std::string> Split(const std::string& value, char delimiter)
{
    std::vector<std::string> result;
    size_t start = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
        if (i == value.size() || value[i] == delimiter) {
            result.push_back(value.substr(start, i - start));
            start = i + 1;
        }
    }
    return result;
}

std::string XmlEscape(const std::string& value)
{
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        switch (value[i]) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&apos;"; break;
        default: result += value[i]; break;
        }
    }
    return result;
}

std::string XmlDecode(const std::string& value)
{
    std::string result;
    for (size_t i = 0; i < value.size();) {
        if (value[i] != '&') {
            result += value[i++];
            continue;
        }
        size_t end = value.find(';', i + 1);
        if (end == std::string::npos) {
            result += value[i++];
            continue;
        }
        std::string entity = value.substr(i + 1, end - i - 1);
        if (entity == "amp") result += '&';
        else if (entity == "lt") result += '<';
        else if (entity == "gt") result += '>';
        else if (entity == "quot") result += '"';
        else if (entity == "apos" || entity == "#39") result += '\'';
        else if (entity == "nbsp") result += ' ';
        else if (entity.size() > 1 && entity[0] == '#') {
            unsigned long code = 0;
            bool hex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
            std::istringstream input(entity.substr(hex ? 2 : 1));
            if (hex) input >> std::hex >> code; else input >> code;
            if (input && code <= 0x7F) result += static_cast<char>(code);
            else if (input && code <= 0x7FF) {
                result += static_cast<char>(0xC0 | (code >> 6));
                result += static_cast<char>(0x80 | (code & 0x3F));
            } else if (input && code <= 0xFFFF) {
                result += static_cast<char>(0xE0 | (code >> 12));
                result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (code & 0x3F));
            } else if (input && code <= 0x10FFFF) {
                result += static_cast<char>(0xF0 | (code >> 18));
                result += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (code & 0x3F));
            } else result.append(value, i, end - i + 1);
        } else result.append(value, i, end - i + 1);
        i = end + 1;
    }
    return result;
}

std::string FormatTime(int64_t seconds)
{
    if (seconds < 0) seconds = 0;
    std::ostringstream out;
    out << seconds / 3600 << ':';
    int64_t minutes = (seconds % 3600) / 60;
    int64_t remainder = seconds % 60;
    if (minutes < 10) out << '0';
    out << minutes << ':';
    if (remainder < 10) out << '0';
    out << remainder;
    return out.str();
}

int64_t ParseTime(const std::string& value)
{
    std::string text = Trim(value);
    if (text.empty()) return -1;
    size_t dot = text.find('.');
    if (dot != std::string::npos) text.resize(dot);
    std::vector<std::string> parts = Split(text, ':');
    if (parts.size() != 3) return -1;
    char* end = 0;
    long long h = std::strtoll(parts[0].c_str(), &end, 10);
    if (!end || *end) return -1;
    long long m = std::strtoll(parts[1].c_str(), &end, 10);
    if (!end || *end) return -1;
    long long s = std::strtoll(parts[2].c_str(), &end, 10);
    if (!end || *end) return -1;
    return h * 3600 + m * 60 + s;
}

std::string UrlHost(const std::string& url)
{
    size_t scheme = url.find("://");
    size_t start = scheme == std::string::npos ? 0 : scheme + 3;
    size_t end = url.find_first_of("/:?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string ResolveUrl(const std::string& baseValue, const std::string& valueValue)
{
    std::string value = Trim(valueValue);
    if (value.find("://") != std::string::npos) return value;
    std::string base = Trim(baseValue);
    size_t scheme = base.find("://");
    if (scheme == std::string::npos) return value;
    size_t authorityEnd = base.find('/', scheme + 3);
    std::string origin = authorityEnd == std::string::npos ? base : base.substr(0, authorityEnd);
    if (!value.empty() && value[0] == '/') return origin + value;

    size_t query = base.find_first_of("?#");
    if (query != std::string::npos) base.resize(query);
    size_t slash = base.rfind('/');
    std::string joined = (slash == std::string::npos || slash < scheme + 3)
        ? base + "/" + value : base.substr(0, slash + 1) + value;
    std::string prefix = joined.substr(0, joined.find("://") + 3);
    size_t pathStart = joined.find('/', joined.find("://") + 3);
    if (pathStart == std::string::npos) return joined;
    std::string path = joined.substr(pathStart);
    std::vector<std::string> segments = Split(path, '/');
    std::vector<std::string> normalized;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].empty() || segments[i] == ".") continue;
        if (segments[i] == "..") {
            if (!normalized.empty()) normalized.pop_back();
        } else normalized.push_back(segments[i]);
    }
    std::string result = prefix + origin.substr(origin.find("://") + 3);
    for (size_t i = 0; i < normalized.size(); ++i) result += "/" + normalized[i];
    if (!path.empty() && path[path.size() - 1] == '/') result += "/";
    return result;
}

} // namespace Util
} // namespace PerleCast
