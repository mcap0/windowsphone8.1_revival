#pragma once
#include "Services.h"
#include "AlbumPage.g.h"

namespace Musica
{
    public ref class AlbumPage sealed
    {
    public:
        AlbumPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
        virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        AlbumItem^ _album;
        unsigned int _stateHandlerToken;

        void RefreshMini();
        void OnPlayerStateChanged(Platform::Object^ sender, Platform::Object^ args);
        void OnPlayAlbum(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnShuffleAlbum(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTrackClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnTrackHolding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void OnMiniTapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnPlayClick(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
    };
}
