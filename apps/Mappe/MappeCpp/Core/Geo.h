// URL builders and place-text formatting (Photon geocoder, OSM links). Standard C++ only.
#pragma once

#include <string>

namespace mappe {

// Fixed-point text for coordinates (5 decimals, trailing zeros removed).
std::string FormatCoord(double v);

// Wide string to UTF-8 (handles UTF-16 surrogate pairs where wchar_t is 16 bits).
std::string ToUtf8(const std::wstring& s);

// Percent-encoding of the UTF-8 bytes of s.
std::string UrlEncode(const std::wstring& s);

std::string PhotonSearchUrl(const std::wstring& query, int limit, bool hasBias, double biasLat,
                            double biasLon);
std::string PhotonReverseUrl(double lat, double lon);

// Opens the same spot in the browser (scheme "metrosurf" first, then "https").
std::string OsmLink(double lat, double lon, int zoom, const std::string& scheme);

// Fields of one Photon feature, filled by the app's JSON reader.
struct PlaceInfo {
    std::wstring name;
    std::wstring street;
    std::wstring houseNumber;
    std::wstring postcode;
    std::wstring city;
    std::wstring state;
    std::wstring country;
    double lat = 0.0;
    double lon = 0.0;
};

std::wstring PlaceTitle(const PlaceInfo& p);
std::wstring PlaceSubtitle(const PlaceInfo& p);

}  // namespace mappe
