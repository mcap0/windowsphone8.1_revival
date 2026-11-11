#include "pch.h"
#include "PlaylistPage.xaml.h"
#include "NowPlayingPage.xaml.h"
#include "Services.h"
#include "Dialogs.h"

using namespace Musica;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::UI::Popups;
using namespace concurrency;

namespace
{
    void ShowErrorMessage(String^ text)
    {
        MessageDialog^ dialog = ref new MessageDialog(text);
        create_task(dialog->ShowAsync());
    }

    void ConfirmDelete(String^ title, IVector<String^>^ paths)
    {
        auto dialog = ref new MessageDialog("Eliminare \"" + title + "\" dal telefono?");
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
                    if (result.get() > 0)
                        ShowErrorMessage("Impossibile eliminare alcuni brani.");
                }
                catch (Exception^)
                {
                    ShowErrorMessage("Errore durante l'eliminazione dei brani.");
                }
            }, task_continuation_context::use_current());
        }, task_continuation_context::use_current());
    }

    void ShowPlaylistPicker(IVector<Song^>^ songs)
    {
        auto dialog = ref new ContentDialog();
        dialog->Title = "Aggiungi a playlist";
        dialog->PrimaryButtonText = "aggiungi";
        dialog->SecondaryButtonText = "annulla";
        auto panel = ref new StackPanel();
        auto list = ref new ListView();
        list->ItemsSource = MusicService::GetPlaylists();
        list->SelectionMode = ListViewSelectionMode::Single;
        list->MaxHeight = 260;
        auto nameBox = ref new TextBox();
        nameBox->PlaceholderText = "Nome nuova playlist";
        auto create = ref new Button();
        create->Content = "nuova playlist...";
        panel->Children->Append(list);
        panel->Children->Append(nameBox);
        panel->Children->Append(create);
        create->Click += ref new RoutedEventHandler([dialog, nameBox, songs](Object^, RoutedEventArgs^)
        {
            if (nameBox->Text == nullptr || nameBox->Text->Length() == 0)
                return;
            MusicService::CreatePlaylist(nameBox->Text);
            for (unsigned int i = 0; i < songs->Size; ++i)
                MusicService::AddToPlaylist(nameBox->Text, songs->GetAt(i));
            dialog->Hide();
        });
        dialog->Content = panel;
        create_task(dialog->ShowAsync()).then([list, songs](ContentDialogResult result)
        {
            if (result != ContentDialogResult::Primary)
                return;
            PlaylistInfo^ playlist = dynamic_cast<PlaylistInfo^>(list->SelectedItem);
            if (playlist != nullptr)
                for (unsigned int i = 0; i < songs->Size; ++i)
                    MusicService::AddToPlaylist(playlist->Name, songs->GetAt(i));
        }, task_continuation_context::use_current());
    }
}

PlaylistPage::PlaylistPage()
    : _playlist(nullptr),
      _stateHandlerToken(0),
      _selection(nullptr)
{
    InitializeComponent();
}

void PlaylistPage::OnNavigatedTo(NavigationEventArgs^ e)
{
    if (_stateHandlerToken == 0)
        _stateHandlerToken = MusicService::AddStateChangedHandler(
            ref new EventHandler<Object^>(this, &PlaylistPage::OnPlayerStateChanged));
    MusicService::StartPlayerMonitoring();

    String^ name = dynamic_cast<String^>(e->Parameter);
    _playlist = nullptr;
    for (unsigned int i = 0; i < MusicService::GetPlaylists()->Size; ++i)
    {
        PlaylistInfo^ playlist = MusicService::GetPlaylists()->GetAt(i);
        if (playlist->Name == name)
        {
            _playlist = playlist;
            break;
        }
    }

    if (_playlist != nullptr)
    {
        PageTitle->Text = _playlist->Name;
        TrackCount->Text = _playlist->Summary;
        PlaylistTracks->ItemsSource = _playlist->Tracks;
        _selection = ref new SongSelection(this, PlaylistTracks, _playlist, [this]()
        {
            if (_playlist != nullptr)
                TrackCount->Text = _playlist->Summary;
        });
    }
    else
    {
        PageTitle->Text = "Playlist non trovata";
        TrackCount->Text = "";
        PlaylistTracks->ItemsSource = nullptr;
    }
    RefreshMini();
}

void PlaylistPage::OnNavigatedFrom(NavigationEventArgs^)
{
    if (_selection != nullptr)
        _selection->Exit();
    MusicService::StopPlayerMonitoring();
    if (_stateHandlerToken != 0)
    {
        MusicService::RemoveStateChangedHandler(_stateHandlerToken);
        _stateHandlerToken = 0;
    }
}

void PlaylistPage::OnPlayerStateChanged(Object^, Object^)
{
    RefreshMini();
    if (_playlist != nullptr)
        TrackCount->Text = _playlist->Summary;
}

void PlaylistPage::RefreshMini()
{
    MiniTitle->Text = MusicService::GetNowTitle();
    MiniArtist->Text = MusicService::GetNowArtist();
    MiniButton->Text = MusicService::IsPlaying()
        ? ref new String(L"\u275A\u275A")
        : ref new String(L"\u25B6");
}

void PlaylistPage::OnRename(Object^, RoutedEventArgs^)
{
    if (_playlist == nullptr)
        return;
    AskName("Rinomina playlist", _playlist->Name, [this](String^ name)
    {
        if (_playlist == nullptr)
            return;
        MusicService::RenamePlaylist(_playlist, name);
        PageTitle->Text = _playlist->Name;
    });
}

void PlaylistPage::OnSelect(Object^, RoutedEventArgs^)
{
    if (_selection != nullptr)
        _selection->Enter(nullptr);
}

void PlaylistPage::OnDelete(Object^, RoutedEventArgs^)
{
    if (_playlist == nullptr)
        return;
    auto dialog = ref new MessageDialog(
        "Eliminare la playlist \"" + _playlist->Name + "\"? I brani restano sul telefono.");
    dialog->Commands->Append(ref new UICommand("elimina"));
    dialog->Commands->Append(ref new UICommand("annulla"));
    dialog->DefaultCommandIndex = 1;
    dialog->CancelCommandIndex = 1;
    create_task(dialog->ShowAsync()).then([this](IUICommand^ command)
    {
        if (command == nullptr || command->Label != "elimina" || _playlist == nullptr)
            return;
        MusicService::DeletePlaylist(_playlist);
        _playlist = nullptr;
        if (this->Frame->CanGoBack)
            this->Frame->GoBack();
    }, task_continuation_context::use_current());
}

void PlaylistPage::OnPlay(Object^, RoutedEventArgs^)
{
    MusicService::PlayPlaylist(_playlist);
    RefreshMini();
}

void PlaylistPage::OnShuffle(Object^, RoutedEventArgs^)
{
    if (_playlist != nullptr)
        MusicService::PlaySongs(_playlist->Tracks, 0, true);
    RefreshMini();
}

void PlaylistPage::OnTrackClick(Object^, ItemClickEventArgs^ e)
{
    Song^ song = dynamic_cast<Song^>(e->ClickedItem);
    if (song == nullptr || _playlist == nullptr)
        return;
    for (unsigned int i = 0; i < _playlist->Tracks->Size; ++i)
        if (_playlist->Tracks->GetAt(i) == song)
        {
            MusicService::PlaySongs(_playlist->Tracks, i, false);
            break;
        }
    RefreshMini();
}

void PlaylistPage::OnTrackHolding(
    Object^,
    Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e)
{
    if (_playlist == nullptr ||
        e->HoldingState != Windows::UI::Input::HoldingState::Started)
        return;
    FrameworkElement^ element = dynamic_cast<FrameworkElement^>(e->OriginalSource);
    if (element == nullptr)
        return;
    Song^ song = dynamic_cast<Song^>(element->DataContext);
    if (song == nullptr || (_selection != nullptr && _selection->Active()))
        return;

    auto menu = ref new MenuFlyout();
    auto select = ref new MenuFlyoutItem();
    select->Text = "seleziona";
    select->Click += ref new RoutedEventHandler([this, song](Object^, RoutedEventArgs^)
    {
        if (_selection != nullptr)
            _selection->Enter(song);
    });
    menu->Items->Append(select);
    auto play = ref new MenuFlyoutItem();
    play->Text = "riproduci";
    play->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        MusicService::PlaySong(song);
    });
    menu->Items->Append(play);

    auto remove = ref new MenuFlyoutItem();
    remove->Text = "rimuovi dalla playlist";
    remove->Click += ref new RoutedEventHandler([this, song](Object^, RoutedEventArgs^)
    {
        MusicService::RemoveFromPlaylist(_playlist, song);
        TrackCount->Text = _playlist->Summary;
    });
    menu->Items->Append(remove);

    auto add = ref new MenuFlyoutItem();
    add->Text = "aggiungi a playlist";
    add->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        auto one = ref new Platform::Collections::Vector<Song^>();
        one->Append(song);
        ShowPlaylistPicker(one);
    });
    menu->Items->Append(add);

    auto deleteSong = ref new MenuFlyoutItem();
    deleteSong->Text = "elimina dal telefono";
    deleteSong->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        auto paths = ref new Platform::Collections::Vector<String^>();
        paths->Append(song->Path);
        ConfirmDelete(song->Title, paths);
    });
    menu->Items->Append(deleteSong);
    menu->ShowAt(element);
}

void PlaylistPage::OnMiniTapped(
    Object^,
    Windows::UI::Xaml::Input::TappedRoutedEventArgs^)
{
    Frame->Navigate(NowPlayingPage::typeid);
}

void PlaylistPage::OnPlayClick(
    Object^,
    Windows::UI::Xaml::Input::TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::PlayOrPause();
}
