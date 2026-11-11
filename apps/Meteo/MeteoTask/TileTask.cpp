#include <collection.h>
#include <ppltasks.h>
#include <vector>
#include "TileTask.h"
#include "../Shared/Forecast.h"
#include "../Shared/Tile.h"

using namespace concurrency;
using namespace MeteoTask;
using namespace Windows::ApplicationModel::Background;

namespace {

// one place after the other: its forecast, then its tiles
task<void> RefreshPlace(MeteoCore::Place place) {
    task<MeteoCore::Place> where = place.here ? MeteoCore::CurrentPlace() : task_from_result(place);
    return where
        .then([](MeteoCore::Place p) {
            if (!p.error.empty()) {
                MeteoCore::Forecast none;
                none.error = p.error;
                return task_from_result(none);
            }
            return MeteoCore::Download(p);
        })
        .then([](task<MeteoCore::Forecast> t) {
            try {
                MeteoCore::Forecast f = t.get();
                if (!f.error.empty()) MeteoCore::Log(L"task: " + f.error);
                else MeteoCore::UpdateTile(f);
            } catch (Platform::Exception^ ex) {
                MeteoCore::Log(L"task: eccezione " + std::wstring(ex->Message->Data()));
            }
        });
}

}  // namespace

// the home place (app tile) and every place pinned to Start
void TileTask::Run(IBackgroundTaskInstance^ taskInstance) {
    auto deferral = taskInstance->GetDeferral();
    create_task(Windows::UI::StartScreen::SecondaryTile::FindAllAsync())
        .then([](task<Windows::Foundation::Collections::IVectorView<Windows::UI::StartScreen::SecondaryTile^>^> t) {
            std::vector<MeteoCore::Place> places;
            places.push_back(MeteoCore::Home());
            try {
                auto tiles = t.get();
                for (unsigned i = 0; i < tiles->Size; ++i) {
                    MeteoCore::Place p;
                    if (MeteoCore::PlaceFromArguments(tiles->GetAt(i)->Arguments->Data(), p) &&
                        !MeteoCore::SamePlace(p, places[0]))
                        places.push_back(p);
                }
            } catch (Platform::Exception^) {}
            task<void> chain = task_from_result();
            for (const MeteoCore::Place& p : places) {
                MeteoCore::Place place = p;
                chain = chain.then([place]() { return RefreshPlace(place); });
            }
            return chain;
        })
        .then([deferral](task<void> t) {
            try { t.get(); } catch (Platform::Exception^) {}
            deferral->Complete();
        });
}
