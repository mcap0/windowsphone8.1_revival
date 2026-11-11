#pragma once
#include "Services.h"
#include "Selection.h"
#include "PlaylistPage.g.h"

namespace Musica
{
    public ref class PlaylistPage sealed
    {
    public:
        PlaylistPage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
        virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        Musica::PlaylistInfo^ _playlist;
        unsigned int _stateHandlerToken;
        SongSelection^ _selection;

        void RefreshMini();
        void OnPlayerStateChanged(Platform::Object^ sender, Platform::Object^ args);
        void OnRename(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnSelect(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnDelete(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnPlay(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnShuffle(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnTrackClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnTrackHolding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void OnMiniTapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnPlayClick(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
    };
}
