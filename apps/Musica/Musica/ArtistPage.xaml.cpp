#include "pch.h"
#include "ArtistPage.xaml.h"
#include "AlbumPage.xaml.h"
#include "NowPlayingPage.xaml.h"
#include "Services.h"
#include <cwctype>
#include <algorithm>
#include <string>

using namespace Musica;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Popups;
using namespace concurrency;

namespace
{
    std::wstring changeCase(const std::wstring& value, bool upper)
    {
        std::wstring result(value);
        std::transform(result.begin(), result.end(), result.begin(),
            [upper](wchar_t character)
            {
                return static_cast<wchar_t>(
                    upper ? std::towupper(character) : std::towlower(character));
            });
        return result;
    }

    std::wstring stringValue(Platform::String^ value)
    {
        return value == nullptr ? std::wstring() : std::wstring(value->Data());
    }

    void ConfirmDelete(String^ title, Windows::Foundation::Collections::IVector<String^>^ paths)
    {
        MessageDialog^ dialog = ref new MessageDialog(
            "Eliminare \"" + title + "\" dal telefono?");
        dialog->Commands->Append(ref new UICommand("elimina"));
        dialog->Commands->Append(ref new UICommand("annulla"));
        dialog->DefaultCommandIndex = 1;
        dialog->CancelCommandIndex = 1;

        create_task(dialog->ShowAsync()).then([paths](IUICommand^ command)
        {
            if (command == nullptr || command->Label != "elimina")
                return;

            create_task(MusicService::DeleteSongsAsync(paths)).then([](task<unsigned int> result)
            {
                try
                {
                    unsigned int failures = result.get();
                    if (failures > 0)
                    {
                        MessageDialog^ status = ref new MessageDialog(
                            "Impossibile eliminare " + failures.ToString() + " brani.");
                        create_task(status->ShowAsync());
                    }
                }
                catch (Exception^)
                {
                    MessageDialog^ status = ref new MessageDialog(
                        "Errore durante l'eliminazione dei brani.");
                    create_task(status->ShowAsync());
                }
            }, task_continuation_context::use_current());
        }, task_continuation_context::use_current());
    }
}

ArtistPage::ArtistPage()
    : _stateHandlerToken(0)
{
    InitializeComponent();
    ArtistSongs->Holding += ref new HoldingEventHandler(
        [this](Object^, HoldingRoutedEventArgs^ e)
        {
            if (e->HoldingState != Windows::UI::Input::HoldingState::Started)
                return;

            Song^ song = dynamic_cast<Song^>(ArtistSongs->SelectedItem);
            if (song == nullptr)
                return;

            auto menu = ref new MenuFlyout();
            auto remove = ref new MenuFlyoutItem();
            remove->Text = "elimina dal telefono";
            remove->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
            {
                auto paths = ref new Platform::Collections::Vector<String^>();
                paths->Append(song->Path);
                ConfirmDelete(song->Title, paths);
            });
            menu->Items->Append(remove);
            menu->ShowAt(ArtistSongs);
        });
}

void ArtistPage::OnNavigatedTo(NavigationEventArgs^ e)
{
    if (_stateHandlerToken == 0)
        _stateHandlerToken = MusicService::AddStateChangedHandler(
            ref new EventHandler<Object^>(this, &ArtistPage::OnPlayerStateChanged));
    MusicService::StartPlayerMonitoring();

    String^ artist = dynamic_cast<String^>(e->Parameter);
    if (artist == nullptr)
        artist = "Sconosciuto";

    ArtistPivot->Title = ref new String(changeCase(stringValue(artist), true).c_str());
    ArtistSongs->ItemsSource = MusicService::SongsForArtist(artist);

    auto albums = ref new Platform::Collections::Vector<AlbumItem^>();
    const std::wstring artistName = changeCase(stringValue(artist), false);
    for (unsigned int i = 0; i < MusicService::GetAlbums()->Size; ++i)
    {
        AlbumItem^ album = MusicService::GetAlbums()->GetAt(i);
        if (changeCase(stringValue(album->Artist), false) == artistName)
            albums->Append(album);
    }
    ArtistAlbums->ItemsSource = albums;
    RefreshMini();
}

void ArtistPage::OnNavigatedFrom(NavigationEventArgs^)
{
    MusicService::StopPlayerMonitoring();
    if (_stateHandlerToken != 0)
    {
        MusicService::RemoveStateChangedHandler(_stateHandlerToken);
        _stateHandlerToken = 0;
    }
}

void ArtistPage::OnPlayerStateChanged(Object^, Object^)
{
    RefreshMini();
}

void ArtistPage::RefreshMini()
{
    MiniTitle->Text = MusicService::GetNowTitle();
    MiniArtist->Text = MusicService::GetNowArtist();
    MiniButton->Text = MusicService::IsPlaying()
        ? ref new Platform::String(L"\u275A\u275A")
        : ref new Platform::String(L"\u25B6");
}

void ArtistPage::OnAlbumClick(Object^, ItemClickEventArgs^ e)
{
    AlbumItem^ album = dynamic_cast<AlbumItem^>(e->ClickedItem);
    if (album != nullptr)
        Frame->Navigate(AlbumPage::typeid, album->Key);
}

void ArtistPage::OnSongClick(Object^, ItemClickEventArgs^ e)
{
    Song^ song = dynamic_cast<Song^>(e->ClickedItem);
    auto songs = dynamic_cast<Platform::Collections::Vector<Song^>^>(ArtistSongs->ItemsSource);
    if (song != nullptr && songs != nullptr)
    {
        for (unsigned int i = 0; i < songs->Size; ++i)
            if (songs->GetAt(i) == song)
            {
                MusicService::PlaySongs(songs, i, false);
                break;
            }
    }
}

void ArtistPage::OnMiniTapped(Object^, TappedRoutedEventArgs^)
{
    Frame->Navigate(NowPlayingPage::typeid);
}

void ArtistPage::OnPlayClick(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::PlayOrPause();
}
