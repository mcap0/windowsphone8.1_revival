#include "Geo.h"

#include <iomanip>
#include <sstream>
#include <vector>

namespace mappe {

namespace {
const char* const PhotonSearchBase = "https://photon.komoot.io/api/";
const char* const PhotonReverseBase = "https://photon.komoot.io/reverse";

std::wstring Join(const std::vector<std::wstring>& parts, const std::wstring& sep) {
    std::wstring out;
    for (const std::wstring& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += sep;
        out += p;
    }
    return out;
}
}  // namespace

std::string FormatCoord(double v) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(5) << v;
    std::string s = os.str();
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string ToUtf8(const std::wstring& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        unsigned long cp = static_cast<unsigned long>(s[i]);
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
            unsigned long lo = static_cast<unsigned long>(s[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::string UrlEncode(const std::wstring& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string utf8 = ToUtf8(s);
    std::string out;
    for (unsigned char c : utf8) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string PhotonSearchUrl(const std::wstring& query, int limit, bool hasBias, double biasLat,
                            double biasLon) {
    std::string url = std::string(PhotonSearchBase) + "?q=" + UrlEncode(query) +
                      "&limit=" + std::to_string(limit);
    if (hasBias) url += "&lat=" + FormatCoord(biasLat) + "&lon=" + FormatCoord(biasLon);
    return url;
}

std::string PhotonReverseUrl(double lat, double lon) {
    return std::string(PhotonReverseBase) + "?lat=" + FormatCoord(lat) + "&lon=" + FormatCoord(lon);
}

std::string OsmLink(double lat, double lon, int zoom, const std::string& scheme) {
    std::string la = FormatCoord(lat), lo = FormatCoord(lon);
    return scheme + "://www.openstreetmap.org/?mlat=" + la + "&mlon=" + lo + "#map=" +
           std::to_string(zoom) + "/" + la + "/" + lo;
}

std::wstring PlaceTitle(const PlaceInfo& p) {
    if (!p.name.empty()) return p.name;
    std::wstring street = Join({p.street, p.houseNumber}, L" ");
    if (!street.empty()) return street;
    if (!p.city.empty()) return p.city;
    return L"Luogo senza nome";
}

std::wstring PlaceSubtitle(const PlaceInfo& p) {
    std::wstring title = PlaceTitle(p);
    std::wstring street = Join({p.street, p.houseNumber}, L" ");
    std::wstring cityLine = Join({p.postcode, p.city}, L" ");
    std::vector<std::wstring> parts;
    if (street != title) parts.push_back(street);
    parts.push_back(cityLine);
    parts.push_back(p.state);
    parts.push_back(p.country);
    return Join(parts, L", ");
}

}  // namespace mappe
