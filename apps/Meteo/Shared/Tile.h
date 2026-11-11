// Tile: Start tiles like Haru's. The front is Haru's weather picture, the back "16\u00b0 / nuvoloso" with the time of the
// forecast; the phone flips between the two (notification queue). The app tile shows the home place; each place
// can also have its own pinned tile, labelled like Haru's ("Catania, IT"), which opens Meteo on that place.
// Images from the package (Assets\Haru): no drawing, so it works from the background task too.
#pragma once

#include "Forecast.h"

namespace MeteoCore {

// updates the app tile (when f is the home place) and the place's own tile (when pinned)
void UpdateTile(const Forecast& f);
// pins a tile for the place (the phone goes to Start to show it)
concurrency::task<bool> PinTile(const Forecast& f);
bool IsPinned(const Place& place);

}  // namespace MeteoCore
