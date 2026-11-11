// MainPage: the Navit map (SwapChainPanel) with touch, GPS, voice and the offline map download.
#pragma once

#include <deque>
#include <string>

#include "MainPage.g.h"
#include "DxSurface.h"

namespace Mappe81 {

public ref class MainPage sealed {
public:
    MainPage();

internal:
    // Called from Navit's thread through the C hooks; they marshal to the UI thread.
    void SpeakLater(const std::wstring& text);
    void KeepScreenOnLater();
    void NavitReadyLater();
    void ShotReady(Platform::Array<unsigned char>^ bgra, int width, int height);

protected:
    virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

private:
    void Locate_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void ZoomIn_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void ZoomOut_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Shot_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Download_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void Panel_SizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ e);
    void Panel_PointerPressed(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
    void Panel_PointerMoved(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
    void Panel_PointerReleased(Platform::Object^ sender, Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);
    void Voice_MediaEnded(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    void OnPositionChanged(Windows::Devices::Geolocation::Geolocator^ sender,
                           Windows::Devices::Geolocation::PositionChangedEventArgs^ e);
    void OnStatusChanged(Windows::Devices::Geolocation::Geolocator^ sender,
                         Windows::Devices::Geolocation::StatusChangedEventArgs^ e);

    void CheckMapThenStart();
    void DownloadMap();
    void StartNavitIfReady();
    void StartGps();
    void SpeakNext();
    void LogVoices();
    void SetStatus(const std::wstring& text);
    Windows::Foundation::Point ToPixels(Windows::UI::Xaml::Input::PointerRoutedEventArgs^ e);

    DxSurface _dx;
    float _scale;
    bool _mapReady;
    bool _navitStarted;
    bool _downloading;
    bool _hasFix;
    double _lastLat, _lastLon;
    Windows::Devices::Geolocation::Geolocator^ _geo;
    Windows::System::Display::DisplayRequest^ _display;
    bool _displayActive;
    Windows::Media::SpeechSynthesis::SpeechSynthesizer^ _synth;
    std::deque<std::wstring> _speech;
    bool _speaking;
    // touch: first finger drives Navit's drag, a second finger turns it into a pinch
    unsigned int _finger1, _finger2;
    bool _down1, _down2;
    Windows::Foundation::Point _p1, _p2;
    double _pinchStart;
};

}
