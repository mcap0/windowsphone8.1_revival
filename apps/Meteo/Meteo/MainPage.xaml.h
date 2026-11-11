// MainPage: the forecast of one place in three columns (adesso, prossime 12 ore, prossimi 7 giorni), like Haru:
// small grey labels, values in the accent colour, square accent boxes for the times.
#pragma once

#include "MainPage.g.h"

namespace Meteo {

public ref class MainPage sealed {
public:
    MainPage();
    // null = normal start (the last place shown); meteo:?luogo=...&giorno=AAAA-MM-GG from the Assistente;
    // a place's own Start tile passes the place (TileArguments)
    void Open(Platform::String^ uri);

protected:
    virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

private:
    void Places_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Refresh_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Home_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Pin_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);

    void ShowPlace(const MeteoCore::Place& place, const std::wstring& day);
    void Refresh();
    void Render(const MeteoCore::Forecast& f);
    void SelectDay();
    void SetStatus(const std::wstring& text);
    void PutSection(unsigned i, Platform::String^ header, Windows::UI::Xaml::UIElement^ content);
    void UpdateHomeButton();
    void RefreshTile();
    void DrawTile(const MeteoCore::Forecast& f);

    MeteoCore::Place _place;
    MeteoCore::Forecast _forecast;  // the one on screen, for pinning
    std::wstring _day;  // AAAA-MM-GG to open on (empty = today)
    std::wstring _updated;  // HH:MM of the forecast shown
    bool _hasData;
    bool _started;
    Windows::UI::Xaml::Controls::TextBlock^ _status;  // bottom of "adesso": when it was updated, or the error
    Windows::UI::Xaml::Controls::ScrollViewer^ _week;  // scrolled to _day
};

}  // namespace Meteo
