#include "pch.h"
#include "MainPage.xaml.h"
#include "ArtistPage.xaml.h"
#include "AlbumPage.xaml.h"
#include "PlaylistPage.xaml.h"
#include "NowPlayingPage.xaml.h"
#include "OnlinePage.xaml.h"
#include "Services.h"
#include "Dialogs.h"

using namespace Musica;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
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
    void ConfirmDelete(String^ title, IVector<String^>^ paths)
    {
        MessageDialog^ dialog = ref new MessageDialog("Eliminare \"" + title + "\" dal telefono?");
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
                        if (result.get() > 0)
                        {
                            MessageDialog^ errorDialog =
                                ref new MessageDialog("Impossibile eliminare alcuni brani.");
                            create_task(errorDialog->ShowAsync());
                        }
                    }
                    catch (Exception^)
                    {
                        MessageDialog^ errorDialog =
                            ref new MessageDialog("Errore durante l'eliminazione dei brani.");
                        create_task(errorDialog->ShowAsync());
                    }
                }, task_continuation_context::use_current());
        }, task_continuation_context::use_current());
    }

    void AddAllToPlaylist(String^ name, IVector<Song^>^ songs)
    {
        MusicService::AddSongsToPlaylist(name, songs);
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
        list->MaxHeight = 300;
        list->ItemTemplate = safe_cast<DataTemplate^>(
            Windows::UI::Xaml::Markup::XamlReader::Load(
                "<DataTemplate xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\"><StackPanel Margin=\"8\"><TextBlock Text=\"{Binding Name}\" FontSize=\"20\"/><TextBlock Text=\"{Binding Summary}\" FontSize=\"14\"/></StackPanel></DataTemplate>"));
        panel->Children->Append(list);

        auto nameBox = ref new TextBox();
        nameBox->PlaceholderText = "Nome nuova playlist";
        panel->Children->Append(nameBox);

        auto createButton = ref new Button();
        createButton->Content = "nuova playlist...";
        panel->Children->Append(createButton);
        createButton->Click += ref new RoutedEventHandler(
            [dialog, nameBox, songs](Object^, RoutedEventArgs^)
            {
                String^ name = nameBox->Text;
                if (name == nullptr || name->Length() == 0)
                    return;
                MusicService::CreatePlaylist(name);
                AddAllToPlaylist(name, songs);
                dialog->Hide();
            });

        dialog->Content = panel;
        create_task(dialog->ShowAsync()).then(
            [dialog, list, songs](ContentDialogResult result)
            {
                if (result != ContentDialogResult::Primary)
                    return;
                PlaylistInfo^ playlist = dynamic_cast<PlaylistInfo^>(list->SelectedItem);
                if (playlist != nullptr)
                    AddAllToPlaylist(playlist->Name, songs);
            }, task_continuation_context::use_current());
    }
}

MainPage::MainPage()
    : _stateHandlerToken(0)
    , _coverLibraryVersion(0)
{
    InitializeComponent();
    _selection = ref new SongSelection(this, SongsList, nullptr, nullptr);
    SongsList->ItemsSource = MusicService::GetSongs();
    ArtistsList->ItemsSource = MusicService::GetArtists();
    AlbumsList->ItemsSource = MusicService::GetAlbums();
    PlaylistsList->ItemsSource = MusicService::GetPlaylists();
    _songsRail = ref new LetterRail(SongsList, SongsPane, [](unsigned int i)
    {
        return MusicService::GetSongs()->GetAt(i)->Title;
    });
    _artistsRail = ref new LetterRail(ArtistsList, ArtistsPane, [](unsigned int i)
    {
        return MusicService::GetArtists()->GetAt(i);
    });

    // where the user was: the tab here, the scroll position when each list has its items and is on screen
    auto values = ApplicationData::Current->LocalSettings->Values;
    for (int i = 0; i < 4; ++i)
    {
        String^ key = "mainPos" + i.ToString();
        _pendingPosition[i] = values->HasKey(key) ? safe_cast<int>(values->Lookup(key)) : -1;
        ListAt(i)->Loaded += ref new RoutedEventHandler([this](Object^, RoutedEventArgs^) { RestorePositions(); });
    }
    if (values->HasKey("mainPivot"))
    {
        int tab = safe_cast<int>(values->Lookup("mainPivot"));
        if (tab > 0 && tab < 4)
            LibraryPivot->SelectedIndex = tab;
    }
    UpdateAppBar();
}

ListView^ MainPage::ListAt(int index)
{
    switch (index)
    {
    case 0: return SongsList;
    case 1: return ArtistsList;
    case 2: return AlbumsList;
    default: return PlaylistsList;
    }
}

void MainPage::RestorePositions()
{
    for (int i = 0; i < 4; ++i)
    {
        if (_pendingPosition[i] < 0)
            continue;
        ListView^ list = ListAt(i);
        if (list->ItemsPanelRoot == nullptr || list->Items->Size == 0)
            continue;
        unsigned int at = static_cast<unsigned int>(_pendingPosition[i]);
        if (at >= list->Items->Size)
            at = list->Items->Size - 1;
        _pendingPosition[i] = -1;
        list->ScrollIntoView(list->Items->GetAt(at), ScrollIntoViewAlignment::Leading);
    }
}

void MainPage::OnNavigatedTo(NavigationEventArgs^)
{
    if (_stateHandlerToken == 0)
        _stateHandlerToken = MusicService::AddStateChangedHandler(
            ref new EventHandler<Object^>(this, &MainPage::OnPlayerStateChanged));
    MusicService::StartPlayerMonitoring();
    RefreshMini();
    UpdateAppBar();
    LoadCovers();
}

void MainPage::LoadCovers()
{
    _coverLibraryVersion = MusicService::GetLibraryVersion();

    for (unsigned int i = 0; i < MusicService::GetAlbums()->Size; ++i)
    {
        AlbumItem^ album = MusicService::GetAlbums()->GetAt(i);
        if (album->Cover != nullptr || album->CoverRequested ||
            album->CoverPath == nullptr || album->CoverPath->Length() == 0)
            continue;

        // a load is already in flight for this album: without this flag every player tick (500 ms) started
        // another GetThumbnailAsync for the same file
        album->CoverRequested = true;

        create_task(StorageFile::GetFileFromPathAsync(album->CoverPath)).then(
            [album](StorageFile^ file) -> task<StorageItemThumbnail^>
            {
                return create_task(file->GetThumbnailAsync(ThumbnailMode::MusicView, 200));
            }).then([album](task<StorageItemThumbnail^> streamTask)
            {
                try
                {
                    StorageItemThumbnail^ stream = streamTask.get();
                    if (stream == nullptr)
                    {
                        album->CoverRequested = false;
                        return;
                    }
                    auto image = ref new BitmapImage();
                    image->DecodePixelWidth = 100;
                    create_task(image->SetSourceAsync(stream)).then(
                        [album, image](task<void> result)
                        {
                            try
                            {
                                result.get();
                                album->Cover = image;
                            }
                            catch (Exception^)
                            {
                                album->CoverRequested = false;
                            }
                        }, task_continuation_context::use_current());
                }
                catch (Exception^)
                {
                    album->CoverRequested = false;
                }
            });
    }
}

void MainPage::OnNavigatedFrom(NavigationEventArgs^)
{
    SaveView();
    _selection->Exit();
    MusicService::StopPlayerMonitoring();
    if (_stateHandlerToken != 0)
    {
        MusicService::RemoveStateChangedHandler(_stateHandlerToken);
        _stateHandlerToken = 0;
    }
}

void MainPage::SaveView()
{
    auto values = ApplicationData::Current->LocalSettings->Values;
    if (LibraryPivot != nullptr)
        values->Insert("mainPivot", LibraryPivot->SelectedIndex);
    for (int i = 0; i < 4; ++i)
    {
        // a list not seen in this run keeps the position saved before
        auto panel = dynamic_cast<ItemsStackPanel^>(ListAt(i)->ItemsPanelRoot);
        if (panel != nullptr && _pendingPosition[i] < 0 && panel->FirstVisibleIndex >= 0)
            values->Insert("mainPos" + i.ToString(), panel->FirstVisibleIndex);
    }
}

void MainPage::RefreshMini()
{
    MiniTitle->Text = MusicService::GetNowTitle();
    MiniArtist->Text = MusicService::GetNowArtist();
    MiniPlayButton->Child->SetValue(TextBlock::TextProperty,
        MusicService::IsPlaying()
            ? ref new String(L"\u275A\u275A")
            : ref new String(L"\u25B6"));
    UpdateAppBar();
}

void MainPage::OnPlayerStateChanged(Object^, Object^)
{
    RefreshMini();
    RescanButton->IsEnabled = !MusicService::IsScanning();
    RestorePositions();
    _songsRail->Refresh();
    _artistsRail->Refresh();
    // not every tick: covers are (re)loaded only when the library was rebuilt (start, scan, rename, delete)
    if (MusicService::GetLibraryVersion() != _coverLibraryVersion)
        LoadCovers();
}

void MainPage::OnOnline(Object^, RoutedEventArgs^)
{
    Frame->Navigate(OnlinePage::typeid);
}

void MainPage::OnSpotify(Object^, RoutedEventArgs^)
{
    Frame->Navigate(OnlinePage::typeid, L"spotify");
}

void MainPage::OnRescan(Object^, RoutedEventArgs^)
{
    MusicService::Rescan();
    RescanButton->IsEnabled = !MusicService::IsScanning();
}

void MainPage::UpdateAppBar()
{
    const bool songsTab = LibraryPivot != nullptr && LibraryPivot->SelectedIndex == 0;
    const bool playlistsTab = LibraryPivot != nullptr && LibraryPivot->SelectedIndex == 3;
    SelectSongsButton->Visibility = songsTab
        ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    NewPlaylistButton->Visibility = playlistsTab
        ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::OnSongsSelectionChanged(Object^, SelectionChangedEventArgs^)
{
    UpdateAppBar();
}

void MainPage::OnPivotSelectionChanged(Object^, SelectionChangedEventArgs^)
{
    if (_selection != nullptr)
        _selection->Exit();
    if (_songsRail != nullptr)
    {
        RestorePositions();
        _songsRail->Refresh();
        _artistsRail->Refresh();
    }
    UpdateAppBar();
}

void MainPage::OnSongClick(Object^, ItemClickEventArgs^ e)
{
    if (SongsList->SelectionMode == ListViewSelectionMode::Multiple)
        return;
    Song^ song = dynamic_cast<Song^>(e->ClickedItem);
    if (song != nullptr)
        MusicService::PlaySong(song);
}

void MainPage::OnArtistClick(Object^, ItemClickEventArgs^ e)
{
    String^ artist = dynamic_cast<String^>(e->ClickedItem);
    if (artist != nullptr)
        Frame->Navigate(ArtistPage::typeid, artist);
}

void MainPage::OnAlbumClick(Object^, ItemClickEventArgs^ e)
{
    AlbumItem^ album = dynamic_cast<AlbumItem^>(e->ClickedItem);
    if (album != nullptr)
        Frame->Navigate(AlbumPage::typeid, album->Key);
}

void MainPage::OnPlaylistClick(Object^, ItemClickEventArgs^ e)
{
    PlaylistInfo^ playlist = dynamic_cast<PlaylistInfo^>(e->ClickedItem);
    if (playlist != nullptr)
        Frame->Navigate(PlaylistPage::typeid, playlist->Name);
}

void MainPage::OnSongHolding(Object^, HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started)
        return;
    FrameworkElement^ element = dynamic_cast<FrameworkElement^>(e->OriginalSource);
    if (element == nullptr)
        return;
    Song^ song = dynamic_cast<Song^>(element->DataContext);
    if (song != nullptr && !_selection->Active())
        ShowSongMenu(song, element);
}

void MainPage::OnAlbumHolding(Object^, HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started)
        return;
    FrameworkElement^ element = dynamic_cast<FrameworkElement^>(e->OriginalSource);
    if (element == nullptr)
        return;
    AlbumItem^ album = dynamic_cast<AlbumItem^>(element->DataContext);
    if (album != nullptr)
        ShowAlbumMenu(album, element);
}

void MainPage::ShowSongMenu(Song^ song, FrameworkElement^ element)
{
    auto menu = ref new MenuFlyout();

    auto select = ref new MenuFlyoutItem();
    select->Text = "seleziona";
    select->Click += ref new RoutedEventHandler([this, song](Object^, RoutedEventArgs^)
    {
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

    auto after = ref new MenuFlyoutItem();
    after->Text = "riproduci dopo";
    after->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        MusicService::PlayAfter(song);
    });
    menu->Items->Append(after);

    auto queue = ref new MenuFlyoutItem();
    queue->Text = "aggiungi alla coda";
    queue->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        MusicService::AddToQueue(song);
    });
    menu->Items->Append(queue);

    auto add = ref new MenuFlyoutItem();
    add->Text = "aggiungi a playlist";
    add->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        auto one = ref new Platform::Collections::Vector<Song^>();
        one->Append(song);
        ShowPlaylistPicker(one);
    });
    menu->Items->Append(add);

    auto rename = ref new MenuFlyoutItem();
    rename->Text = "rinomina";
    rename->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        AskName("Rinomina brano", song->Title, [song](String^ name)
        {
            MusicService::RenameSong(song, name);
        });
    });
    menu->Items->Append(rename);

    auto remove = ref new MenuFlyoutItem();
    remove->Text = "elimina dal telefono";
    remove->Click += ref new RoutedEventHandler([song](Object^, RoutedEventArgs^)
    {
        auto paths = ref new Platform::Collections::Vector<String^>();
        paths->Append(song->Path);
        ConfirmDelete(song->Title, paths);
    });
    menu->Items->Append(remove);

    menu->ShowAt(element);
}

void MainPage::ShowAlbumMenu(AlbumItem^ album, FrameworkElement^ element)
{
    auto menu = ref new MenuFlyout();

    auto play = ref new MenuFlyoutItem();
    play->Text = "riproduci";
    play->Click += ref new RoutedEventHandler([album](Object^, RoutedEventArgs^)
    {
        MusicService::PlayAlbum(album, false);
    });
    menu->Items->Append(play);

    auto after = ref new MenuFlyoutItem();
    after->Text = "riproduci dopo";
    after->Click += ref new RoutedEventHandler([album](Object^, RoutedEventArgs^)
    {
        for (unsigned int i = 0; i < album->Tracks->Size; ++i)
            MusicService::PlayAfter(album->Tracks->GetAt(i));
    });
    menu->Items->Append(after);

    auto queue = ref new MenuFlyoutItem();
    queue->Text = "aggiungi alla coda";
    queue->Click += ref new RoutedEventHandler([album](Object^, RoutedEventArgs^)
    {
        for (unsigned int i = 0; i < album->Tracks->Size; ++i)
            MusicService::AddToQueue(album->Tracks->GetAt(i));
    });
    menu->Items->Append(queue);

    auto add = ref new MenuFlyoutItem();
    add->Text = "aggiungi a playlist";
    add->Click += ref new RoutedEventHandler([album](Object^, RoutedEventArgs^)
    {
        ShowPlaylistPicker(album->Tracks);
    });
    menu->Items->Append(add);

    auto rename = ref new MenuFlyoutItem();
    rename->Text = "rinomina";
    rename->Click += ref new RoutedEventHandler([this, album](Object^, RoutedEventArgs^)
    {
        AskName("Rinomina album", album->Name, [this, album](String^ name)
        {
            MusicService::RenameAlbum(album, name);
            LoadCovers();
        });
    });
    menu->Items->Append(rename);

    auto remove = ref new MenuFlyoutItem();
    remove->Text = "elimina dal telefono";
    remove->Click += ref new RoutedEventHandler([album](Object^, RoutedEventArgs^)
    {
        auto paths = ref new Platform::Collections::Vector<String^>();
        for (unsigned int i = 0; i < album->Tracks->Size; ++i)
            paths->Append(album->Tracks->GetAt(i)->Path);
        ConfirmDelete(album->Name, paths);
    });
    menu->Items->Append(remove);

    menu->ShowAt(element);
}

void MainPage::OnArtistHolding(Object^, HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started)
        return;
    FrameworkElement^ element = dynamic_cast<FrameworkElement^>(e->OriginalSource);
    if (element == nullptr)
        return;
    String^ artist = dynamic_cast<String^>(element->DataContext);
    if (artist == nullptr)
        return;

    auto menu = ref new MenuFlyout();
    auto play = ref new MenuFlyoutItem();
    play->Text = "riproduci";
    play->Click += ref new RoutedEventHandler([artist](Object^, RoutedEventArgs^)
    {
        MusicService::PlaySongs(MusicService::SongsForArtist(artist), 0, false);
    });
    menu->Items->Append(play);

    auto rename = ref new MenuFlyoutItem();
    rename->Text = "rinomina";
    rename->Click += ref new RoutedEventHandler([this, artist](Object^, RoutedEventArgs^)
    {
        AskName("Rinomina artista", artist, [this, artist](String^ name)
        {
            MusicService::RenameArtist(artist, name);
            LoadCovers();
        });
    });
    menu->Items->Append(rename);
    menu->ShowAt(element);
}

void MainPage::OnPlaylistHolding(Object^, HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started)
        return;
    FrameworkElement^ element = dynamic_cast<FrameworkElement^>(e->OriginalSource);
    if (element == nullptr)
        return;
    PlaylistInfo^ playlist = dynamic_cast<PlaylistInfo^>(element->DataContext);
    if (playlist != nullptr)
        ShowPlaylistMenu(playlist, element);
}

void MainPage::ShowPlaylistMenu(PlaylistInfo^ playlist, FrameworkElement^ element)
{
    auto menu = ref new MenuFlyout();

    auto play = ref new MenuFlyoutItem();
    play->Text = "riproduci";
    play->Click += ref new RoutedEventHandler([playlist](Object^, RoutedEventArgs^)
    {
        MusicService::PlayPlaylist(playlist);
    });
    menu->Items->Append(play);

    auto shuffle = ref new MenuFlyoutItem();
    shuffle->Text = "riproduci in ordine casuale";
    shuffle->Click += ref new RoutedEventHandler([playlist](Object^, RoutedEventArgs^)
    {
        MusicService::PlaySongs(playlist->Tracks, 0, true);
    });
    menu->Items->Append(shuffle);

    auto rename = ref new MenuFlyoutItem();
    rename->Text = "rinomina";
    rename->Click += ref new RoutedEventHandler([playlist](Object^, RoutedEventArgs^)
    {
        AskName("Rinomina playlist", playlist->Name, [playlist](String^ name)
        {
            MusicService::RenamePlaylist(playlist, name);
        });
    });
    menu->Items->Append(rename);

    auto remove = ref new MenuFlyoutItem();
    remove->Text = "elimina playlist";
    remove->Click += ref new RoutedEventHandler([playlist](Object^, RoutedEventArgs^)
    {
        MessageDialog^ dialog = ref new MessageDialog(
            "Eliminare la playlist \"" + playlist->Name + "\"? I brani restano sul telefono.");
        dialog->Commands->Append(ref new UICommand("elimina"));
        dialog->Commands->Append(ref new UICommand("annulla"));
        dialog->DefaultCommandIndex = 1;
        dialog->CancelCommandIndex = 1;
        create_task(dialog->ShowAsync()).then([playlist](IUICommand^ command)
        {
            if (command != nullptr && command->Label == "elimina")
                MusicService::DeletePlaylist(playlist);
        }, task_continuation_context::use_current());
    });
    menu->Items->Append(remove);

    menu->ShowAt(element);
}

void MainPage::OnToggleSelection(Object^, RoutedEventArgs^)
{
    _selection->Enter(nullptr);
}

void MainPage::OnNewPlaylist(Object^, RoutedEventArgs^)
{
    auto dialog = ref new ContentDialog();
    dialog->Title = "Nuova playlist";
    dialog->PrimaryButtonText = "crea";
    dialog->SecondaryButtonText = "annulla";
    auto box = ref new TextBox();
    box->PlaceholderText = "Nome playlist";
    dialog->Content = box;
    create_task(dialog->ShowAsync()).then([box](ContentDialogResult result)
    {
        if (result == ContentDialogResult::Primary)
            MusicService::CreatePlaylist(box->Text);
    }, task_continuation_context::use_current());
}

void MainPage::OnDiagnostics(Object^, RoutedEventArgs^)
{
    auto lines = MusicService::GetDiagnosticLines();
    String^ text = "";
    for (unsigned int i = 0; i < lines->Size; ++i)
    {
        if (i != 0)
            text += "\n";
        text += lines->GetAt(i);
    }
    auto dialog = ref new MessageDialog(text, "Diagnostica");
    dialog->Commands->Append(ref new UICommand("richiedi handshake"));
    dialog->Commands->Append(ref new UICommand("chiudi"));
    dialog->DefaultCommandIndex = 1;
    dialog->CancelCommandIndex = 1;
    create_task(dialog->ShowAsync()).then([](IUICommand^ command)
    {
        if (command != nullptr && command->Label == "richiedi handshake")
            MusicService::RequestTaskHandshake();
    }, task_continuation_context::use_current());
}

void MainPage::OnMiniPlayerTapped(Object^, TappedRoutedEventArgs^)
{
    Frame->Navigate(NowPlayingPage::typeid);
}

void MainPage::OnMiniPlayClick(Object^, TappedRoutedEventArgs^ e)
{
    e->Handled = true;
    MusicService::PlayOrPause();
}
