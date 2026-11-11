#ifndef PERLECAST_UTIL_H
#define PERLECAST_UTIL_H

#include <cstdint>
#include <string>
#include <vector>

namespace PerleCast {
namespace Util {

std::string Trim(const std::string& value);
std::string Lower(const std::string& value);
std::string Upper(const std::string& value);
std::vector<std::string> Split(const std::string& value, char delimiter);
std::string XmlEscape(const std::string& value);
std::string XmlDecode(const std::string& value);
std::string FormatTime(int64_t seconds);
int64_t ParseTime(const std::string& value);
std::string ResolveUrl(const std::string& base, const std::string& value);
std::string UrlHost(const std::string& url);

} // namespace Util
} // namespace PerleCast

#endif
