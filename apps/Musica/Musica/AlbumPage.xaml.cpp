#include "pch.h"
#include "AlbumPage.xaml.h"
#include "NowPlayingPage.xaml.h"
#include "Services.h"
#include "Dialogs.h"
#include <sstream>

using namespace Musica;
using namespace Platform;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::UI::Xaml::Media::Imaging;
using namespace Windows::UI::Popups;
using namespace Windows::Storage;
using namespace Windows::Storage::FileProperties;
using namespace concurrency;

namespace
{
    void ConfirmDelete(
        String^ title,
        Windows::Foundation::Collections::IVector<String^>^ paths)
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

            create_task(MusicService::DeleteSongsAsync(paths)).then(
                [](task<unsigned int> result)
                {
                    try
                    {
                        const unsigned int failures = result.get();
                        if (failures > 0)
                        {
                            MessageDialog^ status = ref new MessageDialog(
                                "Impossibile eliminare alcuni brani.");
                            create_task(status->ShowAsync());
                        }
                    }
                    catch (Exception^)
                    {
                        MessageDialog^ status = ref new MessageDialog(
                            "Errore durante l'eliminazione dei brani.");
                        create_task(status->ShowAsync());
                    }
                },
                task_continuation_context::use_current());
        }, task_continuation_context::use_current());
    }
}

AlbumPage::AlbumPage()
    : _album(nullptr),
      _stateHandlerToken(0)
{
    InitializeComponent();
    DeleteAlbumButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        if (_album == nullptr)
            return;

        auto paths = ref new Platform::Collections::Vector<String^>();
        for (unsigned int i = 0; i < _album->Tracks->Size; ++i)
            paths->Append(_album->Tracks->GetAt(i)->Path);
        ConfirmDelete(_album->Name, paths);
    });
    RenameAlbumButton->Click += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^)
    {
        if (_album == nullptr)
            return;
        AskName("Rinomina album", _album->Name, [this](String^ name)
        {
            if (_album == nullptr)
                return;
            AlbumItem^ old = _album;
            MusicService::RenameAlbum(old, name);
            // The library regroups after a rename: find the album again through its first track.
            if (old->Tracks->Size > 0)
            {
                AlbumItem^ renamed = MusicService::FindAlbum(old->Tracks->GetAt(0)->AlbumKey);
                if (renamed != nullptr)
                    _album = renamed;
            }
            AlbumTitle->Text = _album->Name;
            AlbumTracks->ItemsSource = _album->Tracks;
        });
    });
}

void AlbumPage::OnNavigatedTo(NavigationEventArgs^ e)
{
    if (_stateHandlerToken == 0)
        _stateHandlerToken = MusicService::AddStateChangedHandler(
            ref new Windows::Foundation::EventHandler<Platform::Object^>(
                this, &AlbumPage::OnPlayerStateChanged));
    MusicService::StartPlayerMonitoring();

    String^ key = dynamic_cast<String^>(e->Parameter);
    _album = MusicService::FindAlbum(key);
    if (_album == nullptr)
        return;

    AlbumTitle->Text = _album->Name;
    AlbumArtist->Text = _album->Artist;

    if (_album->Year == 0)
    {
        AlbumYear->Text = "";
    }
    else
    {
        std::wstringstream year;
        year << _album->Year;
        AlbumYear->Text = ref new String(year.str().c_str());
    }

    AlbumTracks->ItemsSource = _album->Tracks;

    if (_album->CoverPath != nullptr && _album->CoverPath->Length() > 0)
    {
        create_task(StorageFile::GetFileFromPathAsync(_album->CoverPath)).then(
            [this](task<StorageFile^> fileTask)
            {
                try
                {
                    StorageFile^ file = fileTask.get();
                    if (file == nullptr)
                        return;

                    create_task(file->GetThumbnailAsync(ThumbnailMode::MusicView, 400)).then(
                        [this](task<StorageItemThumbnail^> streamTask)
                        {
                            try
                            {
                                StorageItemThumbnail^ stream = streamTask.get();
                                if (stream == nullptr)
                                    return;

                                auto bitmap = ref new BitmapImage();
                                bitmap->DecodePixelWidth = 300;
                                create_task(bitmap->SetSourceAsync(stream)).then(
                                    [this, bitmap](task<void> result)
                                    {
                                        try
                                        {
                                            result.get();
                                            AlbumCover->Source = bitmap;
                                        }
                                        catch (Exception^)
                                        {
                                        }
                                    },
                                    task_continuation_context::use_current());
                            }
                            catch (Exception^)
                            {
                            }
                        },
                        task_continuation_context::use_current());
                }
                catch (Exception^)
                {
                }
            },
            task_continuation_context::use_current());
    }

    RefreshMini();
}

void AlbumPage::OnNavigatedFrom(NavigationEventArgs^)
{
    MusicService::StopPlayerMonitoring();
    if (_stateHandlerToken != 0)
    {
        MusicService::RemoveStateChangedHandler(_stateHandlerToken);
        _stateHandlerToken = 0;
    }
}

void AlbumPage::OnPlayerStateChanged(Object^, Object^)
{
    RefreshMini();
}

void AlbumPage::RefreshMini()
{
    MiniTitle->Text = MusicService::GetNowTitle();
    MiniArtist->Text = MusicService::GetNowArtist();
    MiniButton->Text = MusicService::IsPlaying()
        ? ref new Platform::String(L"\u275A\u275A")
        : ref new Platform::String(L"\u25B6");
}

void AlbumPage::OnPlayAlbum(Object^, RoutedEventArgs^)
{
    MusicService::PlayAlbum(_album, false);
    RefreshMini();
}

void AlbumPage::OnShuffleAlbum(Object^, RoutedEventArgs^)
{
    MusicService::PlayAlbum(_album, true);
    RefreshMini();
}

void AlbumPage::OnTrackClick(Object^, ItemClickEventArgs^ e)
{
    Song^ song = dynamic_cast<Song^>(e->ClickedItem);
    if (song == nullptr || _album == nullptr)
        return;

    for (unsigned int i = 0; i < _album->Tracks->Size; ++i)
    {
        if (_album->Tracks->GetAt(i) == song)
        {
            MusicService::PlaySongs(_album->Tracks, i, false);
            break;
        }
    }

    RefreshMini();
}

void AlbumPage::OnTrackHolding(Object^, HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started)
        return;

    Song^ song = dynamic_cast<Song^>(AlbumTracks->SelectedItem);
    if (song == nullptr)
        return;

    auto menu = ref new MenuFlyout();

    auto playAfter = ref new MenuFlyoutItem();
    playAfter->Text = "riproduci dopo";
    playAfter->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        auto one = ref new Platform::Collections::Vector<Song^>();
        one->Append(song);
        MusicService::PlaySongs(one, 0, false);
    });
    menu->Items->Append(playAfter);

    auto queue = ref new MenuFlyoutItem();
    queue->Text = "aggiungi alla coda";
    queue->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        auto one = ref new Platform::Collections::Vector<Song^>();
        one->Append(song);
        MusicService::PlaySongs(one, 0, false);
    });
    menu->Items->Append(queue);

    auto deleteSong = ref new MenuFlyoutItem();
    deleteSong->Text = "elimina dal telefono";
    deleteSong->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        auto paths = ref new Platform::Collections::Vector<String^>();
        paths->Append(song->Path);
        ConfirmDelete(song->Title, paths);
    });
    menu->Items->Append(deleteSong);

    for (unsigned int i = 0; i < MusicService::GetPlaylists()->Size; ++i)
    {
        PlaylistInfo^ playlist = MusicService::GetPlaylists()->GetAt(i);
        String^ playlistName = playlist->Name;

        auto item = ref new MenuFlyoutItem();
        item->Text = "aggiungi a playlist: " + playlistName;
        item->Click += ref new RoutedEventHandler([playlistName, song](Object^, RoutedEventArgs^)
        {
            MusicService::AddToPlaylist(playlistName, song);
        });
        menu->Items->Append(item);
    }

    menu->ShowAt(AlbumTracks);
}

void AlbumPage::OnMiniTapped(Object^, TappedRoutedEventArgs^)
{
    Frame->Navigate(NowPlayingPage::typeid);
}

void AlbumPage::OnPlayClick(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::PlayOrPause();
}
