// Forecast: MET Norway (api.met.no locationforecast, free, no key; places from Photon, OpenStreetMap data) for the
// Meteo app and its background task. Open-Meteo could not be used: its ECDSA Let's Encrypt certificate fails the
// phone's TLS handshake (0x80072F8F). The roots of these RSA chains are unknown to WP8.1 too: accepted on purpose.
#pragma once

#include <string>
#include <vector>

namespace MeteoCore {

struct Place {
    double lat, lon;
    std::wstring name;  // "Palermo, Sicilia" or "Posizione attuale"
    bool here;          // follows the phone position
    std::wstring error; // why it could not be found (empty = ok)
    std::wstring country;  // "IT", for the tile label like Haru's: "Catania, IT"
};

struct Hour {
    std::wstring date;  // AAAA-MM-GG, local
    int hour;
    double temp;
    std::wstring symbol;  // MET symbol without _day/_night: "clearsky", "partlycloudy", "rain", ...
    double rain;          // mm in the next hour (or 6 hours, further ahead)
    bool day;
    int humidity;         // %
    double wind;          // km/h
    double windFrom;      // degrees, where the wind comes from
    double pressure;      // hPa at sea level
};

struct Day {
    std::wstring date;  // AAAA-MM-GG, local
    std::wstring symbol;
    double tmin, tmax;
    double rain;  // mm in the day
    int humidity;  // average %
    double wind, windFrom;  // the strongest wind of the day
};

struct Forecast {
    Place place;
    double temp, wind, windFrom, pressure;  // degrees, km/h, degrees, hPa
    int humidity;
    std::wstring symbol;
    bool day;
    std::wstring updated;  // HH:MM local, when it was downloaded
    std::vector<Hour> hours;
    std::vector<Day> days;
    std::wstring error;  // what failed, HRESULT or HTTP status (empty = ok)
};

// MET symbol -> Italian words and the Haru icon name (Assets\Haru\<name>.png, tile-<name>[_wide].png)
std::wstring Describe(const std::wstring& symbol);
const wchar_t* Icon(const std::wstring& symbol, bool day);
std::wstring Millimetres(double mm);  // 1,5 mm
std::wstring Direction(double degrees);  // "sud-ovest"
std::wstring Speed(double kmh);          // "12 km/h"

// city name -> place
concurrency::task<Place> FindPlace(const std::wstring& name);
// phone position (last known one when the GPS does not answer); name = the town there (Photon reverse), so
// the tile can say "Catania" and never "Posizione attuale"
concurrency::task<Place> CurrentPlace();
// 7 days for a place
concurrency::task<Forecast> Download(const Place& place);

bool Parse(Platform::String^ json, const Place& place, Forecast& out);
// one cache file per place (LocalFolder\meteo-<key>.json): the app opens on the last forecast at once
void SaveCache(Platform::String^ json, const Place& place);
bool LoadCache(const Place& place, Forecast& out);

// favourite places; the first one is always "Posizione attuale". home = the place on the Start tile.
std::vector<Place> Favorites();
void SaveFavorites(const std::vector<Place>& places);
Place Home();
void SetHome(const Place& place);
bool SamePlace(const Place& a, const Place& b);
// "Catania, IT" (the town without the region, and the country code)
std::wstring TileLabel(const Place& place);
// the id of a place's own Start tile, and the place back from the tile's launch arguments
std::wstring TileId(const Place& place);
std::wstring TileArguments(const Place& place);
bool PlaceFromArguments(const std::wstring& arguments, Place& place);
// the place the app shows (kept between openings and handed over by the places page)
Place Shown();
void SetShown(const Place& place);

// "gioved\u00ec 22", "oggi", "domani"
std::wstring DayName(const std::wstring& date, const std::wstring& today);
std::wstring Today();
std::wstring Degrees(double value);  // "23\u00b0"

// sends a diagnostic line to the PC (tools/inbox, share/inbox/meteo.txt)
void Log(const std::wstring& text);

}  // namespace MeteoCore
