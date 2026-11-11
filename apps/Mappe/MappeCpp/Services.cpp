#include "pch.h"
#include "Services.h"

#include "Core/Geo.h"

using namespace concurrency;
using namespace Platform;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Platform::Collections;
using namespace Windows::Web::Http;

namespace MappeCpp {

namespace {
const wchar_t* const UserAgentW = L"DumbPhoneMappe/0.2 (personal use)";

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// Text of a JSON string field, or empty when absent or not a string.
std::wstring JsonText(JsonObject^ o, const wchar_t* key) {
    String^ k = ref new String(key);
    if (o == nullptr || !o->HasKey(k)) return std::wstring();
    IJsonValue^ v = o->GetNamedValue(k);
    if (v->ValueType != JsonValueType::String) return std::wstring();
    return std::wstring(v->GetString()->Data());
}
}  // namespace

task<String^> HttpGetTextAsync(const std::string& url) {
    HttpClient^ client = ref new HttpClient();
    client->DefaultRequestHeaders->TryAppendWithoutValidation(L"User-Agent", ref new String(UserAgentW));
    client->DefaultRequestHeaders->TryAppendWithoutValidation(L"Accept", L"application/json");
    Uri^ uri = ref new Uri(ref new String(Wide(url).c_str()));
    return create_task(client->GetAsync(uri)).then([client](HttpResponseMessage^ resp) -> task<String^> {
        if (!resp->IsSuccessStatusCode) {
            std::wstring reason = resp->ReasonPhrase != nullptr ? std::wstring(resp->ReasonPhrase->Data()) : std::wstring();
            std::wstring msg = L"HTTP " + std::to_wstring(static_cast<int>(resp->StatusCode)) + L" " + reason;
            throw Platform::Exception::CreateException(static_cast<int>(0x80004005), ref new String(msg.c_str()));
        }
        return create_task(resp->Content->ReadAsStringAsync());
    });
}

Vector<PlaceItem^>^ ParsePhoton(String^ json) {
    auto items = ref new Vector<PlaceItem^>();
    JsonObject^ root = JsonObject::Parse(json);
    JsonArray^ features = root->GetNamedArray(L"features");
    for each (IJsonValue^ fv in features) {
        JsonObject^ feature = fv->GetObject();
        mappe::PlaceInfo p;
        JsonArray^ coords = feature->GetNamedObject(L"geometry")->GetNamedArray(L"coordinates");
        p.lon = coords->GetNumberAt(0);
        p.lat = coords->GetNumberAt(1);
        JsonObject^ props = feature->GetNamedObject(L"properties");
        p.name = JsonText(props, L"name");
        p.street = JsonText(props, L"street");
        p.houseNumber = JsonText(props, L"housenumber");
        p.postcode = JsonText(props, L"postcode");
        p.city = JsonText(props, L"city");
        p.state = JsonText(props, L"state");
        p.country = JsonText(props, L"country");

        PlaceItem^ item = ref new PlaceItem();
        item->Title = ref new String(mappe::PlaceTitle(p).c_str());
        item->Subtitle = ref new String(mappe::PlaceSubtitle(p).c_str());
        item->Lat = p.lat;
        item->Lon = p.lon;
        items->Append(item);
    }
    return items;
}

}  // namespace MappeCpp
