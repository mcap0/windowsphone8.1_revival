// PlacesPage: the favourite places (the phone position first), add a city, choose the home place of the tile.
#pragma once

#include "PlacesPage.g.h"

namespace Meteo {

public ref class PlacesPage sealed {
public:
    PlacesPage();

protected:
    virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

private:
    void SearchBox_KeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
    void Add_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void List_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
    void Fill();
    void Add();

    std::vector<MeteoCore::Place> _places;
};

}  // namespace Meteo
