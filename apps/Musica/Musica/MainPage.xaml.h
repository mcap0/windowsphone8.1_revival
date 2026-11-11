#pragma once
#include "Services.h"
#include "Selection.h"
#include "LetterRail.h"
#include "MainPage.g.h"

namespace Musica
{
    public ref class MainPage sealed
    {
    public:
        MainPage();
        void SaveView();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
        virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        unsigned int _stateHandlerToken;
        unsigned int _coverLibraryVersion;
        SongSelection^ _selection;
        LetterRail^ _songsRail;
        LetterRail^ _artistsRail;
        int _pendingPosition[4];  // first visible item to go back to, -1 = done (the lists fill and load late)

        Windows::UI::Xaml::Controls::ListView^ ListAt(int index);
        void RestorePositions();
        void RefreshMini();
        void UpdateAppBar();
        void OnPlayerStateChanged(Platform::Object^ sender, Platform::Object^ args);
        void OnSongsSelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
        void OnPivotSelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
        void OnSongClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnArtistClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnAlbumClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnPlaylistClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void OnSongHolding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void OnAlbumHolding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void ShowSongMenu(Musica::Song^ song, Windows::UI::Xaml::FrameworkElement^ element);
        void ShowAlbumMenu(Musica::AlbumItem^ album, Windows::UI::Xaml::FrameworkElement^ element);
        void OnToggleSelection(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnArtistHolding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void OnPlaylistHolding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void ShowPlaylistMenu(PlaylistInfo^ playlist, Windows::UI::Xaml::FrameworkElement^ element);
        void LoadCovers();
        void OnRescan(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnOnline(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnSpotify(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnNewPlaylist(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnDiagnostics(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void OnMiniPlayerTapped(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
        void OnMiniPlayClick(Platform::Object^ sender, Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e);
    };
}
