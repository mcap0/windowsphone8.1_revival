//
// MainPage.xaml.h
// Declaration of the MainPage class: map, search, location, long press, diagnostics.
//

#pragma once

#include <string>

#include "MainPage.g.h"
#include "Services.h"
#include "TileMapView.h"

namespace MappeCpp {

public ref class MainPage sealed {
public:
    MainPage();
    // Stores the centre and zoom in the app settings.
    void SaveView();

protected:
    virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
    virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

private:
    void Locate_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void SearchFocus_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Diag_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Diag_Refresh_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Diag_Close_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void SearchBox_TextChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::TextChangedEventArgs^ e);
    void SearchBox_KeyDown(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
    void ResultsList_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
    void CopyCoords_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void OpenBrowser_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void ClosePress_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Map_LongPressed(Platform::Object^ sender, LatLonArgs^ e);
    void SearchTimer_Tick(Platform::Object^ sender, Platform::Object^ e);
    void SaveTimer_Tick(Platform::Object^ sender, Platform::Object^ e);
    void OnBackPressed(Platform::Object^ sender, Windows::Phone::UI::Input::BackPressedEventArgs^ e);

    void OnPositionChanged(Windows::Devices::Geolocation::Geolocator^ sender,
                           Windows::Devices::Geolocation::PositionChangedEventArgs^ e);
    void OnStatusChanged(Windows::Devices::Geolocation::Geolocator^ sender,
                         Windows::Devices::Geolocation::StatusChangedEventArgs^ e);

    void RunSearch(const std::wstring& query);
    void ReverseGeocode(double lat, double lon);
    void RequestPosition();
    void ShowPosition(Windows::Devices::Geolocation::Geoposition^ pos);
    void ShowGeoStatus(Windows::Devices::Geolocation::PositionStatus status);
    void ShowPositionError(Platform::Exception^ ex);
    void RestoreView();
    void ShowCrash();
    void SetStatus(const std::wstring& text);
    void SetGeo(const std::wstring& text);
    void RefreshDiag();

    TileMapView^ _map;
    Windows::UI::Xaml::DispatcherTimer^ _searchTimer;
    Windows::UI::Xaml::DispatcherTimer^ _saveTimer;
    Windows::Devices::Geolocation::Geolocator^ _geo;
    Windows::Devices::Geolocation::Geoposition^ _lastPos;
    bool _hadFix = false;
    bool _initialized;
    int _searchSeq;
    int _reverseSeq;
    bool _hasPress;
    double _pressLat;
    double _pressLon;
    std::wstring _geoStatus;
    std::wstring _geoError;
    std::wstring _searchInfo;
    std::wstring _reverseInfo;
};

}
