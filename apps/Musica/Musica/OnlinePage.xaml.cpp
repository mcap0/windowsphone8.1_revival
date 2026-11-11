#include "pch.h"
#include "OnlinePage.xaml.h"
#include "Dialogs.h"

using namespace Musica;
using namespace concurrency;
using namespace Platform;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;

namespace
{
    String^ P(const std::string& s) { return ref new String(Utf16(s).c_str()); }
}

OnlinePage::OnlinePage() : results_(ref new Platform::Collections::Vector<OnlineRow^>()),
    tracks_(ref new Platform::Collections::Vector<OnlineRow^>()), spotifyAlbum_(false), importing_(false), searchGeneration_(0)
{
    InitializeComponent();
    Results->ItemsSource = results_;
    Tracks->ItemsSource = tracks_;
}

void OnlinePage::OnNavigatedTo(NavigationEventArgs^ e)
{
    String^ arg = dynamic_cast<String^>(e->Parameter);
    if (arg != nullptr && arg == L"spotify") Tabs->SelectedIndex = 1;
    // a song the Assistente did not find in the library
    if (arg != nullptr && arg->Length() > 6 && std::wstring(arg->Data()).compare(0, 6, L"cerca:") == 0)
    {
        Query->Text = ref new String(arg->Data() + 6);
        RunSearch();
    }
}

// the new files join the library (albums, artists, search) with a rescan when the page is left
void OnlinePage::OnNavigatedFrom(NavigationEventArgs^)
{
    if (OnlineService::downloadedSomething && !importing_)
    {
        OnlineService::downloadedSomething = false;
        MusicService::Rescan();
    }
}

void OnlinePage::Query_KeyUp(Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key == Windows::System::VirtualKey::Enter) RunSearch();
}

void OnlinePage::RunSearch()
{
    std::string q = Utf8(Query->Text->Data());
    if (q.empty()) return;
    Results->Focus(Windows::UI::Xaml::FocusState::Programmatic);   // closes the keyboard
    SearchStatus->Text = L"cerco...";
    unsigned int gen = ++searchGeneration_;
    OnlineService::Search(q).then([this, gen](task<std::vector<online::Song>> t)
    {
        if (gen != searchGeneration_) return;
        std::vector<online::Song> songs;
        try { songs = t.get(); }
        catch (Exception^ e) { SearchStatus->Text = L"ricerca non riuscita: " + e->Message; return; }
        results_->Clear();
        for (size_t i = 0; i < songs.size(); ++i)
        {
            std::string detail = songs[i].artist;
            if (!songs[i].album.empty()) detail += " \xC2\xB7 " + songs[i].album;
            if (songs[i].seconds) detail += " \xC2\xB7 " + online::formatSeconds(songs[i].seconds);
            auto row = ref new OnlineRow(P(songs[i].title), P(detail));
            row->song = songs[i];
            results_->Append(row);
        }
        SearchStatus->Text = songs.empty() ? L"nessun brano trovato" : L"tocca per ascoltare, tieni premuto per le playlist";
    }, task_continuation_context::use_current());
}

// downloads the row's song (once) and hands it over
void OnlinePage::Fetch(OnlineRow^ row, std::function<void(Song^)> then)
{
    if (row->busy) return;
    row->busy = true;
    row->State = L"...";
    OnlineService::Get(row->song, [row](int pct) { row->State = pct.ToString() + L"%"; })
        .then([row, then](task<Song^> t)
    {
        row->busy = false;
        Song^ s = nullptr;
        try { s = t.get(); }
        catch (Exception^ e) { row->State = L"errore"; return; }
        row->State = L"\u2713";
        then(s);
    }, task_continuation_context::use_current());
}

void OnlinePage::Results_ItemClick(Object^, ItemClickEventArgs^ e)
{
    OnlineRow^ row = dynamic_cast<OnlineRow^>(e->ClickedItem);
    if (row == nullptr) return;
    Fetch(row, [](Song^ s) { MusicService::PlaySong(s); });
}

void OnlinePage::Result_Holding(Object^ sender, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e)
{
    if (e->HoldingState != Windows::UI::Input::HoldingState::Started) return;
    FrameworkElement^ el = dynamic_cast<FrameworkElement^>(sender);
    OnlineRow^ row = el == nullptr ? nullptr : dynamic_cast<OnlineRow^>(el->DataContext);
    if (row == nullptr) return;
    auto menu = ref new MenuFlyout();
    auto add = [menu](String^ text, std::function<void()> action)
    {
        auto item = ref new MenuFlyoutItem();
        item->Text = text;
        item->Click += ref new RoutedEventHandler([action](Object^, RoutedEventArgs^) { action(); });
        menu->Items->Append(item);
    };
    add(L"riproduci", [this, row]() { Fetch(row, [](Song^ s) { MusicService::PlaySong(s); }); });
    add(L"riproduci dopo", [this, row]() { Fetch(row, [](Song^ s) { MusicService::PlayAfter(s); }); });
    add(L"metti in coda", [this, row]() { Fetch(row, [](Song^ s) { MusicService::AddToQueue(s); }); });
    auto playlists = MusicService::GetPlaylists();
    for (unsigned int i = 0; i < playlists->Size; ++i)
    {
        String^ name = playlists->GetAt(i)->Name;
        add(L"aggiungi a \u201c" + name + L"\u201d", [this, row, name]()
        {
            Fetch(row, [name](Song^ s) { MusicService::AddToPlaylist(name, s); });
        });
    }
    add(L"aggiungi a una nuova playlist...", [this, row]()
    {
        AskName(L"nuova playlist", L"", [this, row](String^ name)
        {
            MusicService::CreatePlaylist(name);
            Fetch(row, [name](Song^ s) { MusicService::AddToPlaylist(name, s); });
        });
    });
    add(L"salva soltanto", [this, row]() { Fetch(row, [](Song^) {}); });
    menu->ShowAt(el);
}

void OnlinePage::SpotifyLink_KeyUp(Object^, Windows::UI::Xaml::Input::KeyRoutedEventArgs^ e)
{
    if (e->Key != Windows::System::VirtualKey::Enter || importing_) return;
    std::string link = Utf8(SpotifyLink->Text->Data());
    if (link.empty()) return;
    Tracks->Focus(Windows::UI::Xaml::FocusState::Programmatic);
    SpotifyStatus->Text = L"leggo il link...";
    ImportButton->Visibility = Windows::UI::Xaml::Visibility::Collapsed;
    OnlineService::Spotify(link).then([this](task<online::SpotifyList> t)
    {
        online::SpotifyList l;
        try { l = t.get(); }
        catch (Exception^ e) { SpotifyStatus->Text = L"Spotify non raggiungibile: " + e->Message; return; }
        tracks_->Clear();
        if (!l.error.empty()) { SpotifyStatus->Text = P(l.error); return; }
        spotifyName_ = l.name.empty() ? "Spotify" : l.name;
        spotifyArtist_ = l.artist;
        spotifyCover_ = l.cover;
        spotifyAlbum_ = l.kind == "album";
        for (size_t i = 0; i < l.tracks.size(); ++i)
        {
            auto row = ref new OnlineRow(P(l.tracks[i].title), P(l.tracks[i].artist + " \xC2\xB7 " + online::formatSeconds(l.tracks[i].seconds)));
            row->track = l.tracks[i];
            tracks_->Append(row);
        }
        SpotifyStatus->Text = P("\xe2\x80\x9c" + spotifyName_ + "\xe2\x80\x9d" + (spotifyAlbum_ ? " di " + spotifyArtist_ : std::string()) +
            ": " + std::to_string(static_cast<long long>(l.tracks.size())) + " brani");
        ImportButton->Content = spotifyAlbum_ ? L"scarica l'album" : L"crea la playlist e scarica";
        ImportButton->Visibility = Windows::UI::Xaml::Visibility::Visible;
    }, task_continuation_context::use_current());
}

void OnlinePage::Import_Click(Object^, RoutedEventArgs^)
{
    if (importing_ || tracks_->Size == 0) return;
    String^ name = P(spotifyName_);
    // an album goes among the albums; anything else in the playlist with the Spotify name (created if missing)
    if (!spotifyAlbum_)
    {
        bool exists = false;
        auto playlists = MusicService::GetPlaylists();
        for (unsigned int i = 0; i < playlists->Size && !exists; ++i) exists = playlists->GetAt(i)->Name == name;
        if (!exists) MusicService::CreatePlaylist(name);
    }
    importing_ = true;
    ImportButton->IsEnabled = false;
    ImportNext(0, name, 0, 0);
}

// one track at a time: search on YouTube Music, take the best match, download, add to the playlist (or to the album)
void OnlinePage::ImportNext(unsigned int index, String^ playlist, int done, int missing)
{
    if (index >= tracks_->Size)
    {
        importing_ = false;
        ImportButton->IsEnabled = true;
        SpotifyStatus->Text = L"fatto: " + done.ToString() + (spotifyAlbum_ ? L" brani nell'album \u201c" : L" brani in \u201c") + playlist + L"\u201d" +
            (missing ? L", " + missing.ToString() + L" non trovati" : L"");
        if (OnlineService::downloadedSomething) { OnlineService::downloadedSomething = false; MusicService::Rescan(); }
        return;
    }
    OnlineRow^ row = tracks_->GetAt(index);
    SpotifyStatus->Text = L"brano " + (index + 1).ToString() + L" di " + tracks_->Size.ToString() + L"...";
    Tracks->ScrollIntoView(row);
    row->State = L"cerco";
    OnlineService::Search(online::queryFor(row->track)).then([this, row, index, playlist, done, missing](task<std::vector<online::Song>> t)
    {
        std::vector<online::Song> res;
        try { res = t.get(); } catch (Exception^) {}
        int best = online::bestMatch(row->track, res);
        if (best < 0)
        {
            row->State = L"non trovato";
            ImportNext(index + 1, playlist, done, missing + 1);
            return;
        }
        row->song = res[best];
        row->busy = true;
        online::Song wanted = row->song;
        AlbumTarget album;
        if (spotifyAlbum_)
        {
            // Spotify's names, not YouTube's: every song must land in the same album
            wanted.title = row->track.title;
            wanted.artist = row->track.artist;
            album.name = spotifyName_;
            album.artist = spotifyArtist_;
            album.cover = spotifyCover_;
            album.number = row->track.number;
            album.total = static_cast<int>(tracks_->Size);
        }
        OnlineService::Get(wanted, [row](int pct) { row->State = pct.ToString() + L"%"; }, album)
            .then([this, row, index, playlist, done, missing](task<Song^> t2)
        {
            row->busy = false;
            Song^ s = nullptr;
            try { s = t2.get(); } catch (Exception^) {}
            if (s == nullptr) { row->State = L"errore"; ImportNext(index + 1, playlist, done, missing + 1); return; }
            if (!spotifyAlbum_) MusicService::AddToPlaylist(playlist, s);
            row->State = L"\u2713";
            ImportNext(index + 1, playlist, done + 1, missing);
        }, task_continuation_context::use_current());
    }, task_continuation_context::use_current());
}
