#include "pch.h"
#include "App.xaml.h"
#include "Selection.h"
#include "tscrypto.h"
#include "MainPage.xaml.h"
#include "Services.h"
#include "Commands.h"
#include "NowPlayingPage.xaml.h"
#include "ArtistPage.xaml.h"
#include "AlbumPage.xaml.h"
#include "PlaylistPage.xaml.h"
#include "OnlinePage.xaml.h"

using namespace Musica;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::Phone::UI::Input;

namespace
{
    // TsNet (mbedTLS, WebSocket masks) draws its random bytes from here: without it ts::randomBytes calls abort(), which on
    // the phone ends the app with no log line (the TLS ClientHello needs 32 random bytes before anything is sent)
    void WinRandom(uint8_t* out, size_t n)
    {
        Platform::Array<unsigned char>^ a;
        Windows::Security::Cryptography::CryptographicBuffer::CopyToByteArray(
            Windows::Security::Cryptography::CryptographicBuffer::GenerateRandom(static_cast<unsigned int>(n)), &a);
        memcpy(out, a->Data, n);
    }
}

namespace
{
    // after a command from the Assistente: playback diagnostics to the PC (tools/inbox, share/inbox/musica.txt)
    void ReportToPc(String^ head)
    {
        auto timer = ref new DispatcherTimer();
        TimeSpan wait;
        wait.Duration = 80000000; // 8 s: time for the background task to start the song
        timer->Interval = wait;
        timer->Tick += ref new EventHandler<Object^>([timer, head](Object^, Object^)
        {
            timer->Stop();
            String^ text = head + "\nin riproduzione: " + (MusicService::IsPlaying() ? "si" : "no") +
                           " - " + MusicService::GetNowTitle() + " / " + MusicService::GetNowArtist() + "\n";
            auto lines = MusicService::GetDiagnosticLines();
            for (unsigned int i = 0; i < lines->Size; ++i)
                text += lines->GetAt(i) + "\n";
            try
            {
                auto client = ref new Windows::Web::Http::HttpClient();
                concurrency::create_task(client->PostAsync(ref new Uri("http://192.168.1.148:8765/musica"),
                    ref new Windows::Web::Http::HttpStringContent(text))).then(
                    [client](concurrency::task<Windows::Web::Http::HttpResponseMessage^> t)
                    {
                        try { t.get(); } catch (Exception^) {}
                    });
            }
            catch (Exception^) {}
        });
        timer->Start();
    }
}

App::App()
{
    InitializeComponent();
    ts::setRandomSource(&WinRandom);
    Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
    Resuming += ref new EventHandler<Object^>(this, &App::OnResuming);
    UnhandledException += ref new UnhandledExceptionEventHandler(this, &App::OnUnhandled);
    HardwareButtons::BackPressed += ref new EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
}

Frame^ App::EnsureFrame(String^ arguments)
{
    MusicService::Initialize();

    auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);
    if (rootFrame == nullptr)
    {
        rootFrame = ref new Frame();
        rootFrame->CacheSize = 4;
        rootFrame->Navigated += ref new NavigatedEventHandler(this, &App::OnFrameNavigated);
        Window::Current->Content = rootFrame;
    }

    if (rootFrame->Content == nullptr)
    {
        if (!rootFrame->Navigate(MainPage::typeid, arguments))
            throw ref new FailureException("Impossibile aprire Musica");
    }

    MusicService::Synchronize();
    Window::Current->Activate();
    return rootFrame;
}

// The page the user is on, written at every navigation: a closed app reopens there.
void App::OnFrameNavigated(Object^, NavigationEventArgs^ e)
{
    String^ name = e->SourcePageType.Name;
    String^ page = name == ArtistPage::typeid->FullName ? "artista"
                 : name == AlbumPage::typeid->FullName ? "album"
                 : name == PlaylistPage::typeid->FullName ? "playlist"
                 : name == NowPlayingPage::typeid->FullName ? "brano" : "";
    auto values = ApplicationData::Current->LocalSettings->Values;
    values->Insert("openPage", page);
    values->Insert("openArg", dynamic_cast<String^>(e->Parameter) != nullptr ? safe_cast<String^>(e->Parameter) : "");
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    const bool fresh = dynamic_cast<Frame^>(Window::Current->Content) == nullptr;
    auto values = ApplicationData::Current->LocalSettings->Values;
    String^ page = values->HasKey("openPage") ? safe_cast<String^>(values->Lookup("openPage")) : "";
    String^ arg = values->HasKey("openArg") ? safe_cast<String^>(values->Lookup("openArg")) : "";
    EnsureFrame(e->Arguments);
    if (fresh && (e->Arguments == nullptr || e->Arguments->IsEmpty()) && !page->IsEmpty())
        ReopenWhenLibraryReady(page, arg, 30);
}

void App::ReopenWhenLibraryReady(String^ page, String^ arg, int triesLeft)
{
    if (MusicService::GetSongCount() == 0 && triesLeft > 0)
    {
        auto timer = ref new DispatcherTimer();
        TimeSpan wait;
        wait.Duration = 2000000; // 0.2 s
        timer->Interval = wait;
        timer->Tick += ref new EventHandler<Object^>([this, timer, page, arg, triesLeft](Object^, Object^)
        {
            timer->Stop();
            ReopenWhenLibraryReady(page, arg, triesLeft - 1);
        });
        timer->Start();
        return;
    }
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr || dynamic_cast<MainPage^>(frame->Content) == nullptr)
        return;  // a command got there first
    if (page == "artista")
    {
        auto songs = MusicService::SongsForArtist(arg);
        if (songs != nullptr && songs->Size > 0) frame->Navigate(ArtistPage::typeid, arg);
    }
    else if (page == "album")
    {
        if (MusicService::FindAlbum(arg) != nullptr) frame->Navigate(AlbumPage::typeid, arg);
    }
    else if (page == "playlist")
    {
        auto lists = MusicService::GetPlaylists();
        for (unsigned int i = 0; i < lists->Size; ++i)
            if (lists->GetAt(i)->Name == arg)
            {
                frame->Navigate(PlaylistPage::typeid, arg);
                break;
            }
    }
    else if (page == "brano")
    {
        if (MusicService::GetNowPath() != nullptr && !MusicService::GetNowPath()->IsEmpty())
            frame->Navigate(NowPlayingPage::typeid);
    }
}

void App::OnActivated(IActivatedEventArgs^ e)
{
    EnsureFrame(nullptr);
    auto protocol = dynamic_cast<ProtocolActivatedEventArgs^>(e);
    if (protocol == nullptr)
        return;
    // musica:file?name=<file name>&path=<full path in the music library>: Esplora opens audio files here directly
    // (the phone would pick Xbox Music). "musica:file?..." has no host: the path part is "file".
    std::wstring raw = protocol->Uri->RawUri->Data();
    if (raw.compare(0, 12, L"musica:file?") == 0)
    {
        String^ name = nullptr;
        String^ path = nullptr;
        try { name = protocol->Uri->QueryParsed->GetFirstValueByName("name"); }
        catch (Exception^) { }
        try { path = protocol->Uri->QueryParsed->GetFirstValueByName("path"); }
        catch (Exception^) { }
        if (name != nullptr && !name->IsEmpty()) PlayFileWhenLibraryReady(name, 30, path);
        return;
    }
    RunWhenLibraryReady(protocol->Uri, 30);
}

// An audio file opened from another app (Esplora): Esplora hands over a copy, the music itself is in the library,
// so the song with the same file name is played (the background task plays library paths).
void App::OnFileActivated(FileActivatedEventArgs^ e)
{
    EnsureFrame(nullptr);
    if (e->Files->Size > 0) PlayFileWhenLibraryReady(e->Files->GetAt(0)->Name, 30);
}

void App::PlayFileWhenLibraryReady(String^ name, int triesLeft, String^ path)
{
    if (MusicService::GetSongCount() == 0 && triesLeft > 0)
    {
        auto timer = ref new DispatcherTimer();
        TimeSpan wait;
        wait.Duration = 5000000; // 0.5 s
        timer->Interval = wait;
        timer->Tick += ref new EventHandler<Object^>([this, timer, name, triesLeft, path](Object^, Object^)
        {
            timer->Stop();
            PlayFileWhenLibraryReady(name, triesLeft - 1, path);
        });
        timer->Start();
        return;
    }
    std::wstring wanted = L"\\" + std::wstring(name->Data());
    Song^ found = nullptr;
    auto all = MusicService::GetSongs();
    for (unsigned int i = 0; i < all->Size && found == nullptr; ++i)
    {
        std::wstring path = all->GetAt(i)->Path->Data();
        if (path.size() >= wanted.size() && _wcsicmp(path.c_str() + path.size() - wanted.size(), wanted.c_str()) == 0)
            found = all->GetAt(i);
    }
    if (found == nullptr && path != nullptr && !path->IsEmpty())
    {
        // not scanned yet (Esplora copied it into the music folder): played by its path, the scan adds it later
        std::wstring title = name->Data();
        size_t dot = title.rfind(L'.');
        if (dot != std::wstring::npos) title = title.substr(0, dot);
        found = ref new Song();
        found->Path = path;
        found->Title = ref new String(title.c_str());
        found->Artist = "";
        found->AlbumArtist = "";
        found->Album = "";
    }
    ReportToPc(L"file " + name + (found != nullptr ? L" -> " + found->Path : ref new String(L" -> non nella libreria")));
    if (found == nullptr) return;
    MusicService::PlaySong(found);
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame != nullptr && dynamic_cast<NowPlayingPage^>(frame->Content) == nullptr)
        frame->Navigate(NowPlayingPage::typeid);
}

// The library comes from the cache at start-up; on a first run the scan may still be filling it.
void App::RunWhenLibraryReady(Uri^ uri, int triesLeft)
{
    if (MusicService::GetSongCount() == 0 && triesLeft > 0)
    {
        auto timer = ref new DispatcherTimer();
        TimeSpan wait;
        wait.Duration = 5000000; // 0.5 s
        timer->Interval = wait;
        timer->Tick += ref new EventHandler<Object^>([this, timer, uri, triesLeft](Object^, Object^)
        {
            timer->Stop();
            RunWhenLibraryReady(uri, triesLeft - 1);
        });
        timer->Start();
        return;
    }
    bool ok = Commands::Run(uri);
    ReportToPc(uri->RawUri + (ok ? " -> eseguito" : " -> niente trovato") +
               " (brani " + MusicService::GetSongCount().ToString() + ", tentativi rimasti " + triesLeft.ToString() + ")");
    if (!ok)
    {
        // said aloud "Metto ..." by the Assistente: the song is not here, say so and offer the online search
        String^ asked = nullptr;
        if (std::wstring(uri->RawUri->Data()).compare(0, 17, L"musica:riproduci?") == 0)
        {
            try { asked = uri->QueryParsed->GetFirstValueByName("q"); }
            catch (Exception^) { }
        }
        if (asked == nullptr || asked->IsEmpty())
            return;
        auto dialog = ref new Windows::UI::Popups::MessageDialog("\"" + asked + "\" non \u00e8 nella libreria.");
        dialog->Commands->Append(ref new Windows::UI::Popups::UICommand("cerca online"));
        dialog->Commands->Append(ref new Windows::UI::Popups::UICommand("chiudi"));
        dialog->DefaultCommandIndex = 0;
        dialog->CancelCommandIndex = 1;
        concurrency::create_task(dialog->ShowAsync()).then([asked](Windows::UI::Popups::IUICommand^ command)
        {
            auto root = dynamic_cast<Frame^>(Window::Current->Content);
            if (root != nullptr && command != nullptr && command->Label == "cerca online")
                root->Navigate(OnlinePage::typeid, "cerca:" + asked);
        }, concurrency::task_continuation_context::use_current());
        return;
    }
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame != nullptr && dynamic_cast<NowPlayingPage^>(frame->Content) == nullptr)
        frame->Navigate(NowPlayingPage::typeid);
}

void App::OnSuspending(Object^, SuspendingEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame != nullptr)
    {
        auto page = dynamic_cast<MainPage^>(frame->Content);
        if (page != nullptr)
            page->SaveView();
    }
    MusicService::SavePlaybackState();
}

void App::OnResuming(Object^, Object^)
{
    MusicService::Synchronize();
}

void App::OnBackPressed(Object^, BackPressedEventArgs^ e)
{
    if (BackHook() && BackHook()())
    {
        e->Handled = true;
        return;
    }
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame != nullptr && frame->CanGoBack)
    {
        frame->GoBack();
        e->Handled = true;
    }
}

void App::OnUnhandled(Object^, UnhandledExceptionEventArgs^ e)
{
    e->Handled = true;
    String^ message = e->Message != nullptr ? e->Message : ref new String(L"errore sconosciuto");
    ApplicationData::Current->LocalSettings->Values->Insert("lastCrash", message);
}
