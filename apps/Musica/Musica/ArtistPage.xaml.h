#pragma once
#include "ArtistPage.g.h"

namespace Musica
{
    public ref class ArtistPage sealed
    {
    public:
        ArtistPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
        virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        unsigned int _stateHandlerToken;

        void RefreshMini();
        void OnPlayerStateChanged(Platform::Object^ sender, Platform::Object^ args);
        void OnAlbumClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnSongClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnMiniTapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnPlayClick(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
    };
}
