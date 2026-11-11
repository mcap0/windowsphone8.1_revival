// Network and JSON services for the app: HTTP GET (Windows::Web::Http) and Photon results (Windows::Data::Json).
#pragma once

#include <string>

#include "pch.h"

namespace MappeCpp {

// One search or reverse-geocoding result, bound to the results list.
public ref class PlaceItem sealed {
public:
    property Platform::String^ Title;
    property Platform::String^ Subtitle;
    property double Lat;
    property double Lon;
};

// GET with the app User-Agent (OSM and Photon policy). Throws on HTTP errors.
concurrency::task<Platform::String^> HttpGetTextAsync(const std::string& url);

// Parses a Photon GeoJSON response. Throws on malformed JSON.
Platform::Collections::Vector<PlaceItem^>^ ParsePhoton(Platform::String^ json);

}  // namespace MappeCpp
