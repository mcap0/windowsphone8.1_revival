#pragma once

#include "OnlinePage.g.h"
#include "OnlineService.h"

namespace Musica
{
    // A row of the online lists: a search result, or a Spotify track and what became of it.
    [Windows::UI::Xaml::Data::Bindable]
    public ref class OnlineRow sealed : Windows::UI::Xaml::Data::INotifyPropertyChanged
    {
    public:
        virtual event Windows::UI::Xaml::Data::PropertyChangedEventHandler^ PropertyChanged;
        property Platform::String^ Title { Platform::String^ get() { return title_; } }
        property Platform::String^ Detail { Platform::String^ get() { return detail_; } }
        property Platform::String^ State
        {
            Platform::String^ get() { return state_; }
            void set(Platform::String^ v)
            {
                state_ = v;
                PropertyChanged(this, ref new Windows::UI::Xaml::Data::PropertyChangedEventArgs(L"State"));
            }
        }
    internal:
        OnlineRow(Platform::String^ title, Platform::String^ detail) : title_(title), detail_(detail), state_(L""), busy(false) {}
        online::Song song;
        online::SpotifyTrack track;
        bool busy;
    private:
        Platform::String^ title_;
        Platform::String^ detail_;
        Platform::String^ state_;
    };

    public ref class OnlinePage sealed
    {
    public:
        OnlinePage();

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;
        virtual void OnNavigatedFrom(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        Platform::Collections::Vector<OnlineRow^>^ results_;
        Platform::Collections::Vector<OnlineRow^>^ tracks_;
        std::string spotifyName_, spotifyArtist_, spotifyCover_;
        bool spotifyAlbum_;
        bool importing_;
        unsigned int searchGeneration_;

        void RunSearch();
        void Fetch(OnlineRow^ row, std::function<void(Song^)> then);
        void ImportNext(unsigned int index, Platform::String^ playlist, int done, int missing);
        void Query_KeyUp(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void Results_ItemClick(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void Result_Holding(Platform::Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e);
        void SpotifyLink_KeyUp(Platform::Object^ sender, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e);
        void Import_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
    };
}
