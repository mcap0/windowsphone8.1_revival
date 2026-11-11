#include <collection.h>
#include <ppltasks.h>
#include <windows.h>
#include <string>
#include <cmath>
#include "Forecast.h"

using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Devices::Geolocation;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Web::Http;

namespace MeteoCore {

namespace {

std::wstring Hex(int hr) {
    wchar_t b[16];
    swprintf_s(b, L"0x%08X", static_cast<unsigned>(hr));
    return b;
}

std::wstring FromUtf8(const std::string& bytes) {
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &text[0], n);
    return text;
}

// names saved before the fix above: PU\u00c3\u00b2 -> PU\u00f2. Only when every char fits a byte and the bytes are valid UTF-8
// with at least one multibyte sequence, so a correct name is left alone.
std::wstring RepairLatin1(const std::wstring& s) {
    std::string bytes;
    bool multi = false;
    for (wchar_t c : s) {
        if (c > 0xFF) return s;
        if (c >= 0x80) multi = true;
        bytes.push_back(static_cast<char>(c));
    }
    if (!multi) return s;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    return n > 0 ? FromUtf8(bytes) : s;
}

struct Body {
    String^ text;
    std::wstring error;
};

// GET returning the body; HTTP errors and exceptions become the error text (also sent to the PC log)
task<Body> Get(const std::wstring& url) {
    // the roots of api.met.no (HARICA) and photon (ISRG Root X1) are newer than WP8.1's certificate store.
    // The data is public: accept an unknown root here.
    auto filter = ref new Windows::Web::Http::Filters::HttpBaseProtocolFilter();
    using Windows::Security::Cryptography::Certificates::ChainValidationResult;
    filter->IgnorableServerCertificateErrors->Append(ChainValidationResult::Untrusted);
    filter->IgnorableServerCertificateErrors->Append(ChainValidationResult::IncompleteChain);
    filter->IgnorableServerCertificateErrors->Append(ChainValidationResult::RevocationFailure);
    filter->IgnorableServerCertificateErrors->Append(ChainValidationResult::RevocationInformationMissing);
    auto client = ref new HttpClient(filter);
    client->DefaultRequestHeaders->UserAgent->ParseAdd(L"DumbPhoneMeteo/1.0 (uso personale)");  // MET and Nominatim refuse requests without an app name
    return create_task(client->GetAsync(ref new Uri(ref new String(url.c_str()))))
        .then([client, url](task<HttpResponseMessage^> t) -> task<Body> {
            HttpResponseMessage^ response = nullptr;
            Body b = {nullptr, L""};
            try {
                response = t.get();
            } catch (Exception^ ex) {
                b.error = L"rete " + Hex(ex->HResult) + L" per " + url;
                Log(b.error);
                return task_from_result(b);
            }
            if (!response->IsSuccessStatusCode) {
                b.error = L"HTTP " + std::to_wstring(static_cast<int>(response->StatusCode)) + L" per " + url;
                Log(b.error);
                return task_from_result(b);
            }
            // photon answers "application/json" without a charset and ReadAsStringAsync then reads Latin-1
            // (mojibake like PU\u00c3\u00b2): the bytes are always UTF-8, decode them here.
            return create_task(response->Content->ReadAsBufferAsync()).then([url](task<Windows::Storage::Streams::IBuffer^> body) {
                Body r = {nullptr, L""};
                try {
                    auto reader = Windows::Storage::Streams::DataReader::FromBuffer(body.get());
                    std::string bytes(reader->UnconsumedBufferLength, '\0');
                    if (!bytes.empty())
                        reader->ReadBytes(ArrayReference<unsigned char>(reinterpret_cast<unsigned char*>(&bytes[0]),
                                                                       static_cast<unsigned>(bytes.size())));
                    r.text = ref new String(FromUtf8(bytes).c_str());
                } catch (Exception^ ex) {
                    r.error = L"lettura " + Hex(ex->HResult) + L" per " + url;
                    Log(r.error);
                }
                return r;
            });
        });
}

std::wstring Wide(String^ s) { return s == nullptr ? std::wstring() : std::wstring(s->Data()); }

}  // namespace

namespace {
bool Has(const std::wstring& s, const wchar_t* part) { return s.find(part) != std::wstring::npos; }
bool Starts(const std::wstring& s, const wchar_t* part) { return s.compare(0, wcslen(part), part) == 0; }
}  // namespace

std::wstring Describe(const std::wstring& s) {
    if (Has(s, L"thunder")) return L"temporale";
    if (s == L"clearsky") return L"sereno";
    if (s == L"fair") return L"quasi sereno";
    if (s == L"partlycloudy") return L"poco nuvoloso";
    if (s == L"cloudy") return L"nuvoloso";
    if (s == L"fog") return L"nebbia";
    std::wstring what = Has(s, L"sleet") ? L"nevischio" : Has(s, L"snow") ? L"neve" : L"pioggia";
    if (Has(s, L"showers")) what = Has(s, L"snow") || Has(s, L"sleet") ? L"rovesci di " + what : L"rovesci";
    if (Starts(s, L"light")) return what + L" debole";
    if (Starts(s, L"heavy")) return what + L" forte";
    return what;
}

const wchar_t* Icon(const std::wstring& s, bool day) {
    if (s.empty()) return L"not-available";
    if (Has(s, L"thunder")) return L"thunderstorms";
    if (s == L"clearsky" || s == L"fair") return day ? L"clear-day" : L"clear-night";
    if (s == L"partlycloudy" || s == L"cloudy") return L"cloudy";
    if (s == L"fog") return L"mist";
    if (Has(s, L"sleet")) return L"sleet";
    if (Has(s, L"snow")) return L"snow";
    if (Has(s, L"rain")) return L"rain";
    return L"not-available";
}

std::wstring Direction(double degrees) {
    static const wchar_t* names[] = {L"nord", L"nord-est", L"est", L"sud-est", L"sud", L"sud-ovest", L"ovest", L"nord-ovest"};
    int i = static_cast<int>((degrees + 22.5) / 45.0) % 8;
    return names[i < 0 ? 0 : i];
}

std::wstring Speed(double kmh) {
    return std::to_wstring(static_cast<int>(kmh + 0.5)) + L" km/h";
}

std::wstring Millimetres(double mm) {
    wchar_t b[32];
    if (mm < 10) swprintf_s(b, L"%.1f mm", mm);
    else swprintf_s(b, L"%.0f mm", mm);
    for (wchar_t* c = b; *c; ++c)
        if (*c == L'.') *c = L',';
    return b;
}

void Log(const std::wstring& text) {
    try {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[16];
        swprintf_s(stamp, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
        auto client = ref new HttpClient();
        create_task(client->PostAsync(ref new Uri(L"http://192.168.1.148:8765/meteo"),
                                      ref new HttpStringContent(ref new String((stamp + text + L"\n").c_str()))))
            .then([client](task<HttpResponseMessage^> t) {
                try { t.get(); } catch (Exception^) {}
            });
    } catch (Exception^) {}
}

// Photon (komoot, OpenStreetMap data): Nominatim refuses the phone's older TLS ciphers (0x80072EFD)
task<Place> FindPlace(const std::wstring& name) {
    std::wstring url = L"https://photon.komoot.io/api/?limit=1&q=" + Wide(Uri::EscapeComponent(ref new String(name.c_str())));
    return Get(url).then([name](Body body) {
        Place p = {0, 0, name, false, body.error};
        if (body.text == nullptr) return p;
        JsonObject^ root = nullptr;
        if (!JsonObject::TryParse(body.text, &root) || !root->HasKey(L"features") || root->GetNamedArray(L"features")->Size == 0) {
            p.error = L"non trovo \"" + name + L"\"";
            return p;
        }
        JsonObject^ feature = root->GetNamedArray(L"features")->GetObjectAt(0);
        JsonArray^ coordinates = feature->GetNamedObject(L"geometry")->GetNamedArray(L"coordinates");  // lon, lat
        p.lon = coordinates->GetNumberAt(0);
        p.lat = coordinates->GetNumberAt(1);
        JsonObject^ props = feature->GetNamedObject(L"properties");
        p.name = Wide(props->GetNamedString(L"name", ref new String(name.c_str())));
        if (props->HasKey(L"state")) p.name += L", " + Wide(props->GetNamedString(L"state"));
        p.country = Wide(props->GetNamedString(L"countrycode", L""));
        return p;
    });
}

task<Place> CurrentPlace() {
    auto values = ApplicationData::Current->LocalSettings->Values;
    auto locator = ref new Geolocator();
    TimeSpan maxAge, timeout;
    maxAge.Duration = 30LL * 60 * 10000000;  // a fix from the last 30 minutes is fine
    timeout.Duration = 15LL * 10000000;
    return create_task(locator->GetGeopositionAsync(maxAge, timeout)).then([values, locator](task<Geoposition^> t) {
        Place p = {0, 0, L"Posizione attuale", true, L""};
        try {
            BasicGeoposition pos = t.get()->Coordinate->Point->Position;
            p.lat = pos.Latitude;
            p.lon = pos.Longitude;
            values->Insert(L"gps.lat", p.lat);
            values->Insert(L"gps.lon", p.lon);
        } catch (Exception^ ex) {
            Log(L"posizione " + Hex(ex->HResult));
            if (values->HasKey(L"gps.lat")) {
                p.lat = safe_cast<double>(values->Lookup(L"gps.lat"));
                p.lon = safe_cast<double>(values->Lookup(L"gps.lon"));
            } else {
                p.error = L"posizione non disponibile (attiva la posizione o cerca una citt\u00e0)";
            }
        }
        return p;
    }).then([values](Place p) -> task<Place> {
        // the town at that position, kept for when the next lookup fails
        if (!p.error.empty()) return task_from_result(p);
        wchar_t url[160];
        swprintf_s(url, L"https://photon.komoot.io/reverse?limit=1&lat=%.4f&lon=%.4f", p.lat, p.lon);
        return Get(url).then([p, values](Body body) {
            Place named = p;
            JsonObject^ root = nullptr;
            if (body.text != nullptr && JsonObject::TryParse(body.text, &root) && root->HasKey(L"features") &&
                root->GetNamedArray(L"features")->Size > 0) {
                JsonObject^ props = root->GetNamedArray(L"features")->GetObjectAt(0)->GetNamedObject(L"properties");
                // the main town of the area ("Catania", not the suburb "San Gregorio di Catania"), like Haru
                String^ town = props->HasKey(L"county") ? props->GetNamedString(L"county")
                             : props->HasKey(L"city") ? props->GetNamedString(L"city") : props->GetNamedString(L"name", L"");
                if (!town->IsEmpty()) {
                    named.name = Wide(town);
                    named.country = Wide(props->GetNamedString(L"countrycode", L""));
                    values->Insert(L"gps.name", town);
                    values->Insert(L"gps.country", ref new String(named.country.c_str()));
                }
            } else if (values->HasKey(L"gps.name")) {
                named.name = Wide(safe_cast<String^>(values->Lookup(L"gps.name")));
                if (values->HasKey(L"gps.country")) named.country = Wide(safe_cast<String^>(values->Lookup(L"gps.country")));
            }
            return named;
        });
    });
}

task<Forecast> Download(const Place& place) {
    wchar_t url[160];
    swprintf_s(url, L"https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=%.4f&lon=%.4f", place.lat, place.lon);
    Place p = place;
    return Get(url).then([p](Body body) {
        Forecast f;
        f.place = p;
        f.error = body.error;
        if (body.text == nullptr) return f;
        if (!Parse(body.text, p, f)) {
            f.error = L"risposta del meteo non valida";
            Log(f.error);
            return f;
        }
        SaveCache(body.text, p);
        return f;
    });
}

namespace {

// "2026-10-22T13:00:00Z" -> local time
bool LocalTime(const std::wstring& utc, SYSTEMTIME& local) {
    SYSTEMTIME st = {};
    int y, mo, d, h, mi;
    if (swscanf_s(utc.c_str(), L"%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5) return false;
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(mo);
    st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    return SystemTimeToTzSpecificLocalTime(nullptr, &st, &local) != 0;
}

std::wstring DateText(const SYSTEMTIME& t) {
    wchar_t b[16];
    swprintf_s(b, L"%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return b;
}

// "partlycloudy_night" -> "partlycloudy", day = false
std::wstring Symbol(JsonObject^ data, const wchar_t* period, bool& day, int hour) {
    day = hour >= 7 && hour < 19;
    if (!data->HasKey(ref new String(period))) return L"";
    JsonObject^ summary = data->GetNamedObject(ref new String(period))->GetNamedObject(L"summary", nullptr);
    if (summary == nullptr || !summary->HasKey(L"symbol_code")) return L"";
    std::wstring s = Wide(summary->GetNamedString(L"symbol_code"));
    size_t u = s.find(L'_');
    if (u == std::wstring::npos) return s;
    day = s.substr(u + 1) == L"day";
    return s.substr(0, u);
}

double Amount(JsonObject^ data, const wchar_t* period) {
    if (!data->HasKey(ref new String(period))) return 0;
    JsonObject^ details = data->GetNamedObject(ref new String(period))->GetNamedObject(L"details", nullptr);
    return details != nullptr && details->HasKey(L"precipitation_amount") ? details->GetNamedNumber(L"precipitation_amount") : 0;
}

}  // namespace

bool Parse(String^ json, const Place& place, Forecast& f) {
    JsonObject^ root = nullptr;
    if (!JsonObject::TryParse(json, &root) || !root->HasKey(L"properties")) return false;
    try {
        f.place = place;
        JsonArray^ series = root->GetNamedObject(L"properties")->GetNamedArray(L"timeseries");
        f.hours.clear();
        f.days.clear();
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t updated[8];
        swprintf_s(updated, L"%02d:%02d", now.wHour, now.wMinute);
        f.updated = updated;
        bool first = true;
        std::vector<int> readings;  // per day
        for (unsigned i = 0; i < series->Size; ++i) {
            JsonObject^ entry = series->GetObjectAt(i);
            SYSTEMTIME t;
            if (!LocalTime(Wide(entry->GetNamedString(L"time")), t)) continue;
            JsonObject^ data = entry->GetNamedObject(L"data");
            JsonObject^ details = data->GetNamedObject(L"instant")->GetNamedObject(L"details");
            Hour h;
            h.date = DateText(t);
            h.hour = t.wHour;
            h.temp = details->GetNamedNumber(L"air_temperature");
            const wchar_t* period = data->HasKey(L"next_1_hours") ? L"next_1_hours" : L"next_6_hours";
            h.symbol = Symbol(data, period, h.day, t.wHour);
            h.rain = Amount(data, period);
            h.humidity = static_cast<int>(details->GetNamedNumber(L"relative_humidity", 0) + 0.5);
            h.wind = details->GetNamedNumber(L"wind_speed", 0) * 3.6;
            h.windFrom = details->GetNamedNumber(L"wind_from_direction", 0);
            h.pressure = details->GetNamedNumber(L"air_pressure_at_sea_level", 0);
            if (first) {  // the current hour
                first = false;
                f.temp = h.temp;
                f.symbol = h.symbol;
                f.day = h.day;
                f.humidity = h.humidity;
                f.wind = h.wind;
                f.windFrom = h.windFrom;
                f.pressure = h.pressure;
            }
            if (!h.symbol.empty()) f.hours.push_back(h);

            // the days: min/max of all readings, rain of each period once, the sky around midday
            if (f.days.empty() || f.days.back().date != h.date) {
                Day d;
                d.date = h.date;
                d.tmin = d.tmax = h.temp;
                d.rain = 0;
                d.symbol = h.symbol;
                d.humidity = 0;
                d.wind = -1;
                d.windFrom = 0;
                f.days.push_back(d);
                readings.push_back(0);
            }
            Day& d = f.days.back();
            readings.back() += 1;
            d.humidity += h.humidity;  // summed here, averaged at the end
            if (h.wind > d.wind) {
                d.wind = h.wind;
                d.windFrom = h.windFrom;
            }
            if (h.temp < d.tmin) d.tmin = h.temp;
            if (h.temp > d.tmax) d.tmax = h.temp;
            d.rain += h.rain;
            if (t.wHour >= 11 && t.wHour <= 14 && !h.symbol.empty()) d.symbol = h.symbol;
        }
        for (size_t i = 0; i < f.days.size(); ++i)
            if (readings[i] > 0) f.days[i].humidity /= readings[i];
        // the last day often has only one reading after midnight: it says little
        if (f.days.size() > 1 && readings.back() < 2) f.days.pop_back();
        return !f.days.empty() && !f.symbol.empty();
    } catch (Exception^) {
        return false;
    }
}

namespace {

std::wstring CacheName(const Place& place) {
    if (place.here) return L"meteo-qui.json";
    wchar_t b[64];
    swprintf_s(b, L"meteo-%.2f_%.2f.json", place.lat, place.lon);
    return b;
}

JsonObject^ PlaceJson(const Place& p) {
    JsonObject^ o = ref new JsonObject();
    o->SetNamedValue(L"lat", JsonValue::CreateNumberValue(p.lat));
    o->SetNamedValue(L"lon", JsonValue::CreateNumberValue(p.lon));
    o->SetNamedValue(L"name", JsonValue::CreateStringValue(ref new String(p.name.c_str())));
    o->SetNamedValue(L"here", JsonValue::CreateBooleanValue(p.here));
    o->SetNamedValue(L"country", JsonValue::CreateStringValue(ref new String(p.country.c_str())));
    return o;
}

Place PlaceFrom(JsonObject^ o) {
    Place p = {o->GetNamedNumber(L"lat", 0), o->GetNamedNumber(L"lon", 0), RepairLatin1(Wide(o->GetNamedString(L"name", L""))),
               o->GetNamedBoolean(L"here", false), L"", Wide(o->GetNamedString(L"country", L""))};
    return p;
}

Place Here() {
    Place p = {0, 0, L"Posizione attuale", true, L""};
    return p;
}

}  // namespace

void SaveCache(String^ json, const Place& place) {
    JsonObject^ o = PlaceJson(place);
    o->SetNamedValue(L"data", JsonValue::CreateStringValue(json));
    String^ text = o->Stringify();
    create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(ref new String(CacheName(place).c_str()),
                                                                      CreationCollisionOption::ReplaceExisting))
        .then([text](StorageFile^ file) { return FileIO::WriteTextAsync(file, text); })
        .then([](task<void> t) {
            try { t.get(); } catch (Exception^) {}
        });
}

bool LoadCache(const Place& place, Forecast& out) {
    // synchronous read: small file, wanted before the first frame
    std::wstring path = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\" + CacheName(place);
    HANDLE file = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::string bytes;
    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) bytes.append(buffer, read);
    CloseHandle(file);
    std::wstring text = FromUtf8(bytes);
    JsonObject^ o = nullptr;
    if (!JsonObject::TryParse(ref new String(text.c_str()), &o) || !o->HasKey(L"data")) return false;
    Place p = PlaceFrom(o);  // for the position: the town found there when it was saved
    return Parse(o->GetNamedString(L"data"), p, out);
}

std::vector<Place> Favorites() {
    std::vector<Place> list;
    list.push_back(Here());
    try {
        auto values = ApplicationData::Current->LocalSettings->Values;
        if (values->HasKey(L"preferiti")) {
            JsonArray^ a = JsonArray::Parse(safe_cast<String^>(values->Lookup(L"preferiti")));
            for (unsigned i = 0; i < a->Size; ++i) {
                Place p = PlaceFrom(a->GetObjectAt(i));
                if (!p.here) list.push_back(p);
            }
        }
    } catch (Exception^) {}
    return list;
}

void SaveFavorites(const std::vector<Place>& places) {
    JsonArray^ a = ref new JsonArray();
    for (const Place& p : places)
        if (!p.here) a->Append(PlaceJson(p));
    ApplicationData::Current->LocalSettings->Values->Insert(L"preferiti", a->Stringify());
}

Place Home() {
    try {
        auto values = ApplicationData::Current->LocalSettings->Values;
        if (values->HasKey(L"casa")) return PlaceFrom(JsonObject::Parse(safe_cast<String^>(values->Lookup(L"casa"))));
    } catch (Exception^) {}
    return Here();
}

void SetHome(const Place& place) {
    ApplicationData::Current->LocalSettings->Values->Insert(L"casa", PlaceJson(place)->Stringify());
}

Place Shown() {
    try {
        auto values = ApplicationData::Current->LocalSettings->Values;
        if (values->HasKey(L"mostra")) return PlaceFrom(JsonObject::Parse(safe_cast<String^>(values->Lookup(L"mostra"))));
    } catch (Exception^) {}
    return Home();
}

void SetShown(const Place& place) {
    ApplicationData::Current->LocalSettings->Values->Insert(L"mostra", PlaceJson(place)->Stringify());
}

std::wstring TileLabel(const Place& place) {
    size_t comma = place.name.find(L',');
    std::wstring town = comma == std::wstring::npos ? place.name : place.name.substr(0, comma);
    return place.country.empty() ? town : town + L", " + place.country;
}

std::wstring TileId(const Place& place) {
    if (place.here) return L"qui";
    wchar_t b[64];
    swprintf_s(b, L"p%.2f_%.2f", place.lat, place.lon);
    std::wstring id = b;
    for (auto& c : id)
        if (c == L'-') c = L'm';
    return id;
}

std::wstring TileArguments(const Place& place) {
    return std::wstring(PlaceJson(place)->Stringify()->Data());
}

bool PlaceFromArguments(const std::wstring& arguments, Place& place) {
    JsonObject^ o = nullptr;
    if (arguments.empty() || !JsonObject::TryParse(ref new String(arguments.c_str()), &o)) return false;
    place = PlaceFrom(o);
    return true;
}

bool SamePlace(const Place& a, const Place& b) {
    if (a.here || b.here) return a.here == b.here;
    return fabs(a.lat - b.lat) < 0.01 && fabs(a.lon - b.lon) < 0.01;
}

std::wstring Today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[16];
    swprintf_s(b, L"%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return b;
}

std::wstring DayName(const std::wstring& date, const std::wstring& today) {
    static const wchar_t* names[] = {L"domenica", L"luned\u00ec", L"marted\u00ec", L"mercoled\u00ec",
                                     L"gioved\u00ec", L"venerd\u00ec", L"sabato"};
    int y = 0, m = 0, d = 0;
    if (swscanf_s(date.c_str(), L"%d-%d-%d", &y, &m, &d) != 3) return date;
    SYSTEMTIME st = {};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    FileTimeToSystemTime(&ft, &st);  // fills wDayOfWeek
    if (date == today) return L"oggi";
    int ty = 0, tm = 0, td = 0;
    swscanf_s(today.c_str(), L"%d-%d-%d", &ty, &tm, &td);
    SYSTEMTIME tt = {};
    tt.wYear = static_cast<WORD>(ty);
    tt.wMonth = static_cast<WORD>(tm);
    tt.wDay = static_cast<WORD>(td);
    FILETIME tf;
    SystemTimeToFileTime(&tt, &tf);
    unsigned long long a = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    unsigned long long b = (static_cast<unsigned long long>(tf.dwHighDateTime) << 32) | tf.dwLowDateTime;
    if (a > b && a - b == 864000000000ULL) return L"domani";
    return std::wstring(names[st.wDayOfWeek]) + L" " + std::to_wstring(d);
}

std::wstring Degrees(double value) {
    int v = static_cast<int>(value < 0 ? value - 0.5 : value + 0.5);
    return std::to_wstring(v) + L"\u00b0";
}

}  // namespace MeteoCore
