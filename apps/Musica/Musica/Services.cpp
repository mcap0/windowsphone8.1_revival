#include "pch.h"
#include "Services.h"
#include "..\Core\Library.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <collection.h>
#include <ppltasks.h>
#include <cwctype>
#include <sstream>
#include <string>
#include <vector>
#include <memory>
#include <utility>
#include <limits>
#include <cmath>
#include <map>

using namespace Musica;
using namespace Platform;
using namespace Platform::Collections;
using namespace Windows::Data::Json;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Media::Playback;
using namespace Windows::Storage;
using namespace Windows::Storage::FileProperties;
using namespace Windows::Storage::Search;
using namespace concurrency;

namespace
{
    Vector<Song^>^ g_songs;
    Vector<AlbumItem^>^ g_albums;
    Vector<String^>^ g_artists;
    Vector<PlaylistInfo^>^ g_playlists;
    Vector<Song^>^ g_queue;
    Vector<String^>^ g_diagnostics;
    Windows::UI::Xaml::DispatcherTimer^ g_handshakeTimer;
    Windows::UI::Xaml::DispatcherTimer^ g_playerTimer;
    Windows::UI::Core::CoreDispatcher^ g_playerDispatcher;

    bool g_initialized = false;
    bool g_scanning = false;
    bool g_playing = false;
    bool g_shuffle = false;
    bool g_taskReady = false;
    bool g_handshakeActive = false;
    bool g_playerStateEventAttached = false;
    bool g_syncRequested = false;
    bool g_libraryLoaded = false;
    int g_repeat = 0;
    unsigned int g_queueIndex = 0;
    unsigned int g_scanErrors = 0;
    unsigned int g_libraryVersion = 0;
    unsigned int g_playerMonitorCount = 0;
    unsigned int g_nextStateHandlerToken = 1;
    unsigned long long g_handshakeStarted = 0;
    unsigned long long g_lastPing = 0;
    MediaPlayerState g_playerState = MediaPlayerState::Closed;
    double g_lastPublishedPosition = -1.0;
    double g_lastPublishedDuration = -1.0;
    std::vector<std::pair<unsigned int, EventHandler<Object^>^> > g_stateHandlers;

    String^ g_nowTitle = "Nessun brano";
    String^ g_nowArtist = "";
    String^ g_nowAlbum = "";
    String^ g_nowPath = "";
    String^ g_scanStatus = "In attesa";
    double g_position = 0.0;
    double g_duration = 0.0;

    void LoadPlaylists();
    void ResolvePlaylists();
    std::vector<ValueSet^>& PendingCommands();
    void RaiseStateChanged();
    void RefreshPlayerState();
    void SendCommand(String^ command);
    void ScanLibrary();
    void RestoreQueueFromDisk();

    std::wstring ToWide(String^ value)
    {
        return value == nullptr ? std::wstring() : std::wstring(value->Data());
    }

    String^ ToPlatform(const std::wstring& value)
    {
        return ref new String(value.c_str());
    }

    std::wstring LowerWide(const std::wstring& value)
    {
        std::wstring result(value);
        for (std::wstring::iterator it = result.begin(); it != result.end(); ++it)
            *it = static_cast<wchar_t>(std::towlower(*it));
        return result;
    }

    std::wstring TrimWide(const std::wstring& value)
    {
        std::wstring::size_type first = 0;
        while (first < value.size() && std::iswspace(value[first]))
            ++first;
        std::wstring::size_type last = value.size();
        while (last > first && std::iswspace(value[last - 1]))
            --last;
        return value.substr(first, last - first);
    }

    // small app files written with Win32 (synchronous, no size limit): LocalSettings values stop at 8 KB, so a
    // playlist of ~50 songs could no longer be saved (Insert threw, the old list came back at every opening)
    std::wstring LocalFilePath(const wchar_t* name)
    {
        return ToWide(ApplicationData::Current->LocalFolder->Path) + L"\\" + name;
    }

    bool ReadLocalText(const wchar_t* name, std::wstring& text)
    {
        text.clear();
        HANDLE file = CreateFile2(LocalFilePath(name).c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        std::string bytes;
        char buffer[16384];
        DWORD read = 0;
        bool ok = true;
        for (;;)
        {
            if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) { ok = false; break; }
            if (read == 0) break;
            bytes.append(buffer, read);
        }
        CloseHandle(file);
        if (!ok)
            return false;
        if (!bytes.empty())
        {
            const int count = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            text.resize(count);
            if (count > 0)
                MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &text[0], count);
        }
        return true;
    }

    // written to <name>.tmp and moved over the old file: a crash in the middle never leaves half a list
    bool WriteLocalText(const wchar_t* name, const std::wstring& text)
    {
        std::string bytes;
        if (!text.empty())
        {
            const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            bytes.resize(count);
            if (count > 0)
                WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &bytes[0], count, nullptr, nullptr);
        }
        const std::wstring target = LocalFilePath(name);
        const std::wstring temp = target + L".tmp";
        HANDLE file = CreateFile2(temp.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        const bool ok = bytes.empty() ||
            (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size());
        FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok)
        {
            DeleteFileW(temp.c_str());
            return false;
        }
        return MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    }

    // songs downloaded by the online page (Music\Online): single tracks, each with its own YouTube album, so
    // they stay out of the album list (they are in brani, artisti and their playlists)
    bool IsOnlineDownload(const std::wstring& path)
    {
        const std::wstring::size_type slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return false;
        const std::wstring::size_type before = path.find_last_of(L"\\/", slash == 0 ? 0 : slash - 1);
        const std::wstring folder = path.substr(before == std::wstring::npos ? 0 : before + 1,
            slash - (before == std::wstring::npos ? 0 : before + 1));
        return LowerWide(folder) == L"online";
    }

    String^ Clean(String^ value, String^ fallback)
    {
        return value == nullptr || value->IsEmpty() ? fallback : value;
    }

    void LogDiagnostic(String^ direction, String^ text)
    {
        if (g_diagnostics == nullptr)
            g_diagnostics = ref new Vector<String^>();

        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t timestamp[32];
        swprintf_s(timestamp, L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);

        String^ line = ref new String(timestamp);
        line += " ";
        line += direction;
        line += ": ";
        line += text == nullptr ? "" : text;
        g_diagnostics->Append(line);
        while (g_diagnostics->Size > 20)
            g_diagnostics->RemoveAt(0);
    }

    void BeginHandshake(bool resetReady)
    {
        if (resetReady)
            g_taskReady = false;

        try
        {
            BackgroundMediaPlayer::Current;
        }
        catch (Exception^)
        {
            LogDiagnostic("errore", "BackgroundMediaPlayer non disponibile");
        }

        if (g_handshakeTimer == nullptr)
        {
            g_handshakeTimer = ref new Windows::UI::Xaml::DispatcherTimer();
            TimeSpan interval;
            interval.Duration = 5000000;
            g_handshakeTimer->Interval = interval;
            g_handshakeTimer->Tick += ref new EventHandler<Object^>(
                [](Object^, Object^)
                {
                    if (!g_handshakeActive)
                    {
                        g_handshakeTimer->Stop();
                        return;
                    }

                    const unsigned long long now = GetTickCount64();
                    if (now - g_handshakeStarted > 10000)
                    {
                        g_handshakeActive = false;
                        g_handshakeTimer->Stop();
                        LogDiagnostic("stato", "timeout handshake task");
                        return;
                    }

                    if (now - g_lastPing >= 500)
                    {
                        g_lastPing = now;
                        try
                        {
                            BackgroundMediaPlayer::Current;
                            ValueSet^ ping = ref new ValueSet();
                            ping->Insert("cmd", "ping");
                            BackgroundMediaPlayer::SendMessageToBackground(ping);
                            LogDiagnostic("inviato", "ping");
                        }
                        catch (Exception^)
                        {
                            g_taskReady = false;
                        }
                    }
                });
        }

        g_handshakeActive = true;
        g_handshakeStarted = GetTickCount64();
        g_lastPing = 0;
        g_handshakeTimer->Start();
    }

    void Dispatch(ValueSet^ message)
    {
        if (message == nullptr)
            return;

        String^ command = message->HasKey("cmd")
            ? safe_cast<String^>(message->Lookup("cmd")) : "comando";

        std::vector<ValueSet^>& pending = PendingCommands();
        if (!g_taskReady)
        {
            pending.push_back(message);
            LogDiagnostic("in coda", command);
            BeginHandshake(false);
            return;
        }

        try
        {
            BackgroundMediaPlayer::SendMessageToBackground(message);
            LogDiagnostic("inviato", command);
        }
        catch (Exception^)
        {
            g_taskReady = false;
            pending.push_back(message);
            BeginHandshake(false);
        }
    }

    void FlushPending()
    {
        std::vector<ValueSet^>& pending = PendingCommands();

        while (g_taskReady && !pending.empty())
        {
            ValueSet^ message = pending.front();
            pending.erase(pending.begin());
            try
            {
                BackgroundMediaPlayer::SendMessageToBackground(message);
                String^ command = message->HasKey("cmd")
                    ? safe_cast<String^>(message->Lookup("cmd")) : "comando";
                LogDiagnostic("inviato", command);
            }
            catch (Exception^)
            {
                pending.insert(pending.begin(), message);
                g_taskReady = false;
                BeginHandshake(false);
                break;
            }
        }
    }

    void OnPlayerCurrentStateChanged(MediaPlayer^, Object^)
    {
        Windows::UI::Core::CoreDispatcher^ dispatcher = g_playerDispatcher;
        if (dispatcher == nullptr)
            return;

        try
        {
            dispatcher->RunAsync(
                Windows::UI::Core::CoreDispatcherPriority::Normal,
                ref new Windows::UI::Core::DispatchedHandler(
                    []()
                    {
                        RefreshPlayerState();
                    }));
        }
        catch (Exception^)
        {
        }
    }

    void RefreshPlayerState()
    {
        try
        {
            MediaPlayer^ player = BackgroundMediaPlayer::Current;
            if (player == nullptr)
                return;

            const MediaPlayerState state = player->CurrentState;
            double position = static_cast<double>(player->Position.Duration) / 10000000.0;
            double duration = static_cast<double>(player->NaturalDuration.Duration) / 10000000.0;

            if (!(position >= 0.0))
                position = 0.0;
            if (!(duration >= 0.0))
                duration = 0.0;

            const bool changed = state != g_playerState ||
                std::fabs(position - g_lastPublishedPosition) >= 0.05 ||
                std::fabs(duration - g_lastPublishedDuration) >= 0.05;

            g_playerState = state;
            g_playing = state == MediaPlayerState::Playing ||
                state == MediaPlayerState::Buffering ||
                state == MediaPlayerState::Opening;
            g_position = position;
            g_duration = duration;

            if (changed)
            {
                g_lastPublishedPosition = position;
                g_lastPublishedDuration = duration;
                RaiseStateChanged();
            }
        }
        catch (Exception^)
        {
        }
    }

    void RaiseStateChanged()
    {
        std::vector<std::pair<unsigned int, EventHandler<Object^>^> > handlers(g_stateHandlers);
        for (std::vector<std::pair<unsigned int, EventHandler<Object^>^> >::const_iterator it =
                 handlers.begin(); it != handlers.end(); ++it)
        {
            if (it->second == nullptr)
                continue;
            try
            {
                it->second(nullptr, nullptr);
            }
            catch (Exception^)
            {
            }
        }
    }

    void EnsurePlayerMonitor()
    {
        if (g_playerTimer == nullptr)
        {
            g_playerTimer = ref new Windows::UI::Xaml::DispatcherTimer();
            TimeSpan interval;
            interval.Duration = 5000000;
            g_playerTimer->Interval = interval;
            g_playerTimer->Tick += ref new EventHandler<Object^>(
                [](Object^, Object^)
                {
                    RefreshPlayerState();
                });
        }
    }

    bool HasMusicExtension(String^ path)
    {
        const std::wstring value = LowerWide(ToWide(path));
        const wchar_t* extensions[] =
        {
            L".mp3", L".m4a", L".aac", L".wma", L".wav", L".flac"
        };
        for (unsigned int i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i)
        {
            const std::wstring extension(extensions[i]);
            if (value.size() >= extension.size() &&
                value.compare(value.size() - extension.size(), extension.size(), extension) == 0)
                return true;
        }
        return false;
    }

    MusicaCore::Track ToCore(Song^ song)
    {
        MusicaCore::Track track;
        if (song == nullptr)
            return track;
        track.path = ToWide(song->Path);
        track.title = ToWide(song->Title);
        track.artist = ToWide(song->Artist);
        track.albumArtist = ToWide(song->AlbumArtist);
        track.album = ToWide(song->Album);
        track.trackNumber = song->TrackNumber;
        track.year = song->Year;
        return track;
    }

    // Names changed in the app, kept in LocalFolder\names.json so a rescan does not undo them:
    // "albums": tag album key -> name, "artists": tag artist -> name, "titles": path -> title.
    JsonObject^ g_names;

    JsonObject^ NamesSection(String^ section)
    {
        if (g_names == nullptr)
            g_names = ref new JsonObject();
        if (!g_names->HasKey(section))
            g_names->SetNamedValue(section, ref new JsonObject());
        return g_names->GetNamedObject(section);
    }

    void SetName(String^ section, String^ key, String^ value, String^ original)
    {
        JsonObject^ names = NamesSection(section);
        if (value == original)
        {
            if (names->HasKey(key))
                names->Remove(key);
        }
        else
            names->SetNamedValue(key, JsonValue::CreateStringValue(value));
        g_names->SetNamedValue(section, names);
    }

    String^ LookupName(String^ section, String^ key, String^ fallback)
    {
        JsonObject^ names = NamesSection(section);
        return names->HasKey(key) ? names->GetNamedString(key, fallback) : fallback;
    }

    task<void> LoadNamesAsync()
    {
        return create_task(ApplicationData::Current->LocalFolder->GetFileAsync("names.json")).then(
            [](StorageFile^ file) { return FileIO::ReadTextAsync(file); }).then([](task<String^> text)
            {
                try
                {
                    g_names = JsonObject::Parse(text.get());
                }
                catch (Exception^)
                {
                    g_names = ref new JsonObject();
                    // Musica 1.5.0 kept renamed albums in LocalSettings
                    try
                    {
                        IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
                        if (values->HasKey("albumNames"))
                            g_names->SetNamedValue("albums",
                                JsonObject::Parse(safe_cast<String^>(values->Lookup("albumNames"))));
                    }
                    catch (Exception^) {}
                }
            }, task_continuation_context::use_current());
    }

    void SaveNames()
    {
        if (g_names == nullptr)
            return;
        String^ text = g_names->Stringify();
        create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(
            "names.json", CreationCollisionOption::ReplaceExisting)).then(
            [text](StorageFile^ file) { return FileIO::WriteTextAsync(file, text); }).then([](task<void> result)
            {
                try { result.get(); } catch (Exception^) {}
            });
    }

    String^ TagAlbumKey(Song^ song)
    {
        MusicaCore::Track track = ToCore(song);
        track.artist = ToWide(song->TagArtist);
        track.albumArtist = ToWide(song->TagAlbumArtist);
        track.album = ToWide(song->TagAlbum);
        return ToPlatform(MusicaCore::albumKey(track));
    }

    // song fields hold the file tags here; keeps them in Tag* and applies the names changed in the app
    void ApplyNames(Song^ song)
    {
        song->TagArtist = song->Artist;
        song->TagAlbumArtist = song->AlbumArtist;
        song->TagAlbum = song->Album;
        song->Title = LookupName("titles", song->Path, song->Title);
        song->Artist = LookupName("artists", song->TagArtist, song->Artist);
        song->AlbumArtist = LookupName("artists", song->TagAlbumArtist, song->AlbumArtist);
        song->Album = LookupName("albums", TagAlbumKey(song), song->Album);
    }

    void UpdatePlaylistSummary(PlaylistInfo^ playlist)
    {
        if (playlist == nullptr)
            return;
        std::wstringstream text;
        text << playlist->Tracks->Size << L" brani";
        playlist->Summary = ToPlatform(text.str());
    }

    void RebuildGroups()
    {
        if (g_songs == nullptr)
            return;

        ++g_libraryVersion;
        std::vector<MusicaCore::Track> coreTracks;
        coreTracks.reserve(g_songs->Size);
        std::map<std::wstring, Song^> byPath;
        for (unsigned int i = 0; i < g_songs->Size; ++i)
        {
            Song^ song = g_songs->GetAt(i);
            coreTracks.push_back(ToCore(song));
            // first path wins, like the linear search this replaces
            byPath.insert(std::make_pair(LowerWide(ToWide(song->Path)), song));
        }

        auto findSong = [&byPath](const std::wstring& path) -> Song^
        {
            std::map<std::wstring, Song^>::const_iterator found = byPath.find(LowerWide(path));
            return found == byPath.end() ? nullptr : found->second;
        };

        const std::vector<MusicaCore::Track> sorted = MusicaCore::sortTracks(coreTracks);
        g_songs->Clear();
        for (std::vector<MusicaCore::Track>::const_iterator track = sorted.begin();
             track != sorted.end(); ++track)
        {
            Song^ song = findSong(track->path);
            if (song != nullptr)
                g_songs->Append(song);
        }

        g_albums->Clear();
        std::vector<MusicaCore::Track> albumTracks;
        albumTracks.reserve(coreTracks.size());
        for (std::vector<MusicaCore::Track>::const_iterator track = coreTracks.begin();
             track != coreTracks.end(); ++track)
            if (!IsOnlineDownload(track->path))
                albumTracks.push_back(*track);
        const std::vector<MusicaCore::Album> albums = MusicaCore::groupAlbums(albumTracks);
        for (std::vector<MusicaCore::Album>::const_iterator it = albums.begin();
             it != albums.end(); ++it)
        {
            AlbumItem^ album = ref new AlbumItem();
            album->Key = ToPlatform(it->key);
            album->Name = ToPlatform(it->name);
            album->Artist = ToPlatform(it->artist);
            album->Year = it->year;
            if (!it->tracks.empty())
                album->CoverPath = ToPlatform(it->tracks[0].path);

            for (std::vector<MusicaCore::Track>::const_iterator track = it->tracks.begin();
                 track != it->tracks.end(); ++track)
            {
                Song^ song = findSong(track->path);
                if (song != nullptr)
                    album->Tracks->Append(song);
            }

            std::wstringstream summary;
            summary << ToWide(album->Artist) << L" \u00b7 "
                    << album->Tracks->Size << L" brani";
            album->Summary = ToPlatform(summary.str());
            g_albums->Append(album);
        }

        const std::vector<MusicaCore::Artist> artists = MusicaCore::groupArtists(coreTracks);
        g_artists->Clear();
        for (std::vector<MusicaCore::Artist>::const_iterator it = artists.begin();
             it != artists.end(); ++it)
            g_artists->Append(ToPlatform(it->name));

        ResolvePlaylists();
    }

    void SaveLibrary()
    {
        if (g_songs == nullptr)
            return;
        JsonArray^ array = ref new JsonArray();
        for (unsigned int i = 0; i < g_songs->Size; ++i)
        {
            Song^ song = g_songs->GetAt(i);
            JsonObject^ item = ref new JsonObject();
            item->SetNamedValue("path", JsonValue::CreateStringValue(song->Path));
            item->SetNamedValue("title", JsonValue::CreateStringValue(song->Title));
            item->SetNamedValue("artist", JsonValue::CreateStringValue(song->Artist));
            item->SetNamedValue("albumArtist", JsonValue::CreateStringValue(song->AlbumArtist));
            item->SetNamedValue("album", JsonValue::CreateStringValue(song->Album));
            item->SetNamedValue("tagAlbum", JsonValue::CreateStringValue(song->TagAlbum));
            item->SetNamedValue("tagArtist", JsonValue::CreateStringValue(song->TagArtist));
            item->SetNamedValue("tagAlbumArtist", JsonValue::CreateStringValue(song->TagAlbumArtist));
            item->SetNamedValue("track", JsonValue::CreateNumberValue(song->TrackNumber));
            item->SetNamedValue("year", JsonValue::CreateNumberValue(song->Year));
            item->SetNamedValue("duration", JsonValue::CreateNumberValue(song->DurationSeconds));
            array->Append(item);
        }

        create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(
            "library.json", CreationCollisionOption::ReplaceExisting)).then(
            [array](StorageFile^ file)
            {
                return FileIO::WriteTextAsync(file, array->Stringify());
            }).then([](task<void> result)
            {
                try { result.get(); } catch (Exception^) {}
            });
    }

    void LoadLibrary()
    {
        create_task(ApplicationData::Current->LocalFolder->GetFileAsync(
            "library.json")).then(
            [](task<StorageFile^> fileTask) -> task<void>
            {
                StorageFile^ file = nullptr;
                try { file = fileTask.get(); }
                catch (Exception^) { return create_task([]() {}); }

                return create_task(FileIO::ReadTextAsync(file)).then(
                    [](String^ text)
                    {
                        try
                        {
                            JsonArray^ array = JsonArray::Parse(text);
                            g_songs->Clear();
                            for (unsigned int i = 0; i < array->Size; ++i)
                            {
                                JsonObject^ data = array->GetObjectAt(i);
                                Song^ song = ref new Song();
                                song->Path = data->GetNamedString("path", "");
                                song->Title = data->GetNamedString("title", "");
                                song->Artist = data->GetNamedString("artist", "Sconosciuto");
                                song->AlbumArtist = data->GetNamedString("albumArtist", "Sconosciuto");
                                song->Album = data->GetNamedString("album", "Sconosciuto");
                                song->TagAlbum = data->GetNamedString("tagAlbum", song->Album);
                                song->TagArtist = data->GetNamedString("tagArtist", song->Artist);
                                song->TagAlbumArtist = data->GetNamedString("tagAlbumArtist", song->AlbumArtist);
                                song->TrackNumber = static_cast<unsigned int>(data->GetNamedNumber("track", 0));
                                song->Year = static_cast<unsigned int>(data->GetNamedNumber("year", 0));
                                song->DurationSeconds = data->GetNamedNumber("duration", 0);
                                song->AlbumKey = ToPlatform(MusicaCore::albumKey(ToCore(song)));
                                g_songs->Append(song);
                            }
                            RebuildGroups();
                        }
                        catch (Exception^) {}
                    }, task_continuation_context::use_current());
            }, task_continuation_context::use_current()).then([](task<void> result)
            {
                try { result.get(); } catch (Exception^) {}

                g_libraryLoaded = true;
                // a synchronize may have run before the cache was parsed (queue paths could not be resolved);
                // retry now that the songs exist
                if (g_syncRequested)
                    RestoreQueueFromDisk();
                // first run (or interrupted cache write): there is nothing to show, scan now
                if (g_songs->Size == 0 && !g_scanning)
                    ScanLibrary();
                else
                    RaiseStateChanged();
            }, task_continuation_context::use_current());
    }

    task<void> ScanFolder(StorageFolder^ folder,
                          std::shared_ptr<std::vector<StorageFile^> > files)
    {
        auto ui = task_continuation_context::use_current();
        return create_task(folder->GetFilesAsync()).then(
            [folder, files](task<IVectorView<StorageFile^>^> t)
                -> task<IVectorView<StorageFolder^>^>
            {
                try
                {
                    IVectorView<StorageFile^>^ found = t.get();
                    for (unsigned int i = 0; i < found->Size; ++i)
                        if (HasMusicExtension(found->GetAt(i)->Name))
                            files->push_back(found->GetAt(i));
                }
                catch (Exception^) {}
                return create_task(folder->GetFoldersAsync());
            }, ui).then(
            [files, ui](task<IVectorView<StorageFolder^>^> t) -> task<void>
            {
                task<void> chain = create_task([]() {});
                try
                {
                    IVectorView<StorageFolder^>^ folders = t.get();
                    for (unsigned int i = 0; i < folders->Size; ++i)
                    {
                        StorageFolder^ child = folders->GetAt(i);
                        chain = chain.then([child, files]() -> task<void>
                        {
                            return ScanFolder(child, files);
                        }, ui);
                    }
                }
                catch (Exception^) {}
                return chain;
            }, ui);
    }

    void ScanLibrary()
    {
        if (g_scanning)
            return;
        g_scanning = true;
        g_scanStatus = "Scansione in corso";
        g_scanErrors = 0;
        std::shared_ptr<std::vector<StorageFile^> > files =
            std::make_shared<std::vector<StorageFile^> >();
        std::shared_ptr<std::vector<Song^> > found =
            std::make_shared<std::vector<Song^> >();
        auto ui = task_continuation_context::use_current();

        LoadNamesAsync().then([files]()
        {
            return ScanFolder(KnownFolders::MusicLibrary, files);
        }, ui).then(
            [files, found, ui]() -> task<void>
            {
                task<void> chain = create_task([]() {});
                for (std::vector<StorageFile^>::const_iterator it = files->begin();
                     it != files->end(); ++it)
                {
                    StorageFile^ file = *it;
                    chain = chain.then([file, found, ui]() -> task<void>
                    {
                        return create_task(file->Properties->GetMusicPropertiesAsync()).then(
                            [file, found](task<MusicProperties^> t)
                            {
                                Song^ song = ref new Song();
                                song->Path = file->Path;
                                MusicProperties^ properties = nullptr;
                                try { properties = t.get(); }
                                catch (Exception^) { ++g_scanErrors; }

                                if (properties != nullptr)
                                {
                                    song->Title = Clean(properties->Title, file->DisplayName);
                                    song->Artist = Clean(properties->Artist, "Sconosciuto");
                                    song->AlbumArtist = Clean(properties->AlbumArtist, song->Artist);
                                    song->Album = Clean(properties->Album, "Sconosciuto");
                                    song->TrackNumber = properties->TrackNumber;
                                    song->Year = properties->Year;
                                    song->DurationSeconds =
                                        static_cast<double>(properties->Duration.Duration) / 10000000.0;
                                }
                                else
                                {
                                    song->Title = file->DisplayName;
                                    song->Artist = "Sconosciuto";
                                    song->AlbumArtist = song->Artist;
                                    song->Album = "Sconosciuto";
                                }
                                ApplyNames(song);
                                song->AlbumKey = ToPlatform(MusicaCore::albumKey(ToCore(song)));
                                found->push_back(song);
                            }, ui);
                    }, ui);
                }
                return chain;
            }, ui).then([found](task<void> result)
            {
                g_scanning = false;
                try { result.get(); }
                catch (Exception^) { ++g_scanErrors; }

                if (found->empty() && g_songs->Size > 0)
                {
                    g_scanStatus = "Scansione terminata: libreria salvata mantenuta";
                    if (g_syncRequested)
                        RestoreQueueFromDisk();
                    return;
                }

                g_songs->Clear();
                for (size_t i = 0; i < found->size(); ++i)
                    g_songs->Append((*found)[i]);

                try
                {
                    RebuildGroups();
                    SaveLibrary();
                    if (g_syncRequested && g_queue->Size == 0)
                        RestoreQueueFromDisk();
                    std::wstringstream status;
                    status << L"Completata: " << g_songs->Size << L" brani, "
                           << g_scanErrors << L" errori";
                    g_scanStatus = ToPlatform(status.str());
                    RaiseStateChanged();
                }
                catch (Exception^)
                {
                    ++g_scanErrors;
                    g_scanStatus = "Scansione completata con errori";
                }
            }, ui);
    }

    void OnBackgroundMessage(Object^, MediaPlayerDataReceivedEventArgs^ args)
    {
        if (args == nullptr || args->Data == nullptr)
            return;

        IPropertySet^ data = args->Data;
        String^ command = data->HasKey("cmd")
            ? safe_cast<String^>(data->Lookup("cmd")) : "stato";
        LogDiagnostic("ricevuto", command);

        if (command == "ready")
        {
            // this runs on a background thread: the DispatcherTimer and the pending queue belong to the UI
            // thread (Stop() from here threw and the queued queue/play of a cold start were never sent)
            auto finish = []()
            {
                g_taskReady = true;
                g_handshakeActive = false;
                if (g_handshakeTimer != nullptr)
                    g_handshakeTimer->Stop();
                FlushPending();
                // the background task may have been playing since before this foreground process existed:
                // ask for its state explicitly instead of relying on a queue command (which needs the library)
                SendCommand("state");
            };
            Windows::UI::Core::CoreDispatcher^ dispatcher = g_playerDispatcher;
            if (dispatcher != nullptr)
                dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
                    ref new Windows::UI::Core::DispatchedHandler(finish));
            else
                finish();
        }

        try
        {
            if (data->HasKey("title"))
                g_nowTitle = safe_cast<String^>(data->Lookup("title"));
            if (data->HasKey("artist"))
                g_nowArtist = safe_cast<String^>(data->Lookup("artist"));
            if (data->HasKey("album"))
                g_nowAlbum = safe_cast<String^>(data->Lookup("album"));
            if (data->HasKey("path"))
                g_nowPath = safe_cast<String^>(data->Lookup("path"));
            if (data->HasKey("playing"))
                g_playing = safe_cast<bool>(data->Lookup("playing"));
            if (data->HasKey("positionTicks"))
                g_position = static_cast<double>(safe_cast<long long>(
                    data->Lookup("positionTicks"))) / 10000000.0;
            if (data->HasKey("durationTicks"))
                g_duration = static_cast<double>(safe_cast<long long>(
                    data->Lookup("durationTicks"))) / 10000000.0;
            if (data->HasKey("index"))
                g_queueIndex = safe_cast<unsigned int>(data->Lookup("index"));
        }
        catch (Exception^)
        {
            g_taskReady = false;
            BeginHandshake(false);
        }

        RaiseStateChanged();
    }

    void OnApplicationResuming(Object^, Object^)
    {
        MusicService::RequestTaskHandshake();
        // no automatic rescan here: a full metadata scan on every resume blocked the UI and fought with
        // playback. The cached library is already loaded; "aggiorna" and the online page rescan explicitly.
    }

    std::wstring PathKey(const std::wstring& path)
    {
        return LowerWide(path);
    }

    // Tracks = the stored paths found in the library, in order; the others wait for a later scan
    void ResolvePlaylists()
    {
        if (g_playlists == nullptr)
            return;
        std::map<std::wstring, Song^> byPath;
        for (unsigned int i = 0; g_songs != nullptr && i < g_songs->Size; ++i)
        {
            Song^ song = g_songs->GetAt(i);
            byPath[PathKey(ToWide(song->Path))] = song;
        }
        for (unsigned int p = 0; p < g_playlists->Size; ++p)
        {
            PlaylistInfo^ playlist = g_playlists->GetAt(p);
            playlist->Tracks->Clear();
            for (size_t j = 0; j < playlist->StoredPaths.size(); ++j)
            {
                std::map<std::wstring, Song^>::const_iterator found = byPath.find(PathKey(playlist->StoredPaths[j]));
                if (found != byPath.end())
                    playlist->Tracks->Append(found->second);
            }
            UpdatePlaylistSummary(playlist);
        }
    }

    // LocalFolder\playlists.json; before 1.7.6 the lists were in LocalSettings (moved over once)
    void LoadPlaylists()
    {
        if (g_playlists == nullptr)
            return;
        g_playlists->Clear();
        try
        {
            std::wstring text;
            bool fromSettings = false;
            if (!ReadLocalText(L"playlists.json", text))
            {
                IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
                if (!values->HasKey("playlists"))
                    return;
                text = ToWide(safe_cast<String^>(values->Lookup("playlists")));
                fromSettings = true;
            }
            JsonArray^ list = JsonArray::Parse(ToPlatform(text));
            for (unsigned int i = 0; i < list->Size; ++i)
            {
                JsonObject^ data = list->GetObjectAt(i);
                PlaylistInfo^ playlist = ref new PlaylistInfo();
                playlist->Name = data->GetNamedString("name", "Playlist");
                if (data->HasKey("paths"))
                {
                    JsonArray^ paths = data->GetNamedArray("paths");
                    for (unsigned int j = 0; j < paths->Size; ++j)
                        playlist->StoredPaths.push_back(ToWide(paths->GetStringAt(j)));
                }
                g_playlists->Append(playlist);
            }
            if (fromSettings)
                MusicService::SavePlaylists();
        }
        catch (Exception^) {}
        ResolvePlaylists();
    }

    void ErasePath(std::vector<std::wstring>& paths, const std::wstring& path)
    {
        const std::wstring key = PathKey(path);
        for (std::vector<std::wstring>::iterator it = paths.begin(); it != paths.end(); ++it)
            if (PathKey(*it) == key)
            {
                paths.erase(it);
                return;
            }
    }

    ValueSet^ MakeQueueMessage()
    {
        ValueSet^ message = ref new ValueSet();
        message->Insert("cmd", "queue");
        std::wstring paths;
        for (unsigned int i = 0; g_queue != nullptr && i < g_queue->Size; ++i)
        {
            if (i != 0)
                paths += L"\n";
            paths += ToWide(g_queue->GetAt(i)->Path);
        }
        message->Insert("paths", ToPlatform(paths));
        message->Insert("index", g_queueIndex);
        message->Insert("shuffle", g_shuffle);
        message->Insert("repeat", g_repeat);
        message->Insert("positionTicks", static_cast<long long>(g_position * 10000000.0));
        return message;
    }

    void SendQueue()
    {
        if (g_queue != nullptr)
            Dispatch(MakeQueueMessage());
    }

    void SendCommand(String^ command)
    {
        ValueSet^ message = ref new ValueSet();
        message->Insert("cmd", command);
        Dispatch(message);
    }

    // Rebuilds g_queue from queue.txt. It needs the library: at cold start Synchronize runs before
    // LoadLibrary has parsed the cache, so the call is retried from the LoadLibrary completion.
    void RestoreQueueFromDisk()
    {
        if (!g_libraryLoaded)
            return;

        try
        {
            IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
            std::wstring serialized;
            bool hadSavedQueue = true;
            if (!ReadLocalText(L"queue.txt", serialized))
            {
                if (!values->HasKey("queue"))
                {
                    g_syncRequested = false;
                    return;
                }
                serialized = ToWide(safe_cast<String^>(values->Lookup("queue")));
                hadSavedQueue = serialized.size() > 0;
            }

            g_queue->Clear();
            std::wstring::size_type start = 0;
            while (start < serialized.size())
            {
                std::wstring::size_type end = serialized.find(L'\n', start);
                if (end == std::wstring::npos)
                    end = serialized.size();
                Song^ song = MusicService::FindSong(
                    ToPlatform(serialized.substr(start, end - start)));
                if (song != nullptr)
                    g_queue->Append(song);
                start = end + 1;
            }

            if (g_queue->Size > 0)
            {
                if (g_queueIndex >= g_queue->Size)
                    g_queueIndex = 0;
                SendQueue();
            }
            // if the saved paths exist but none is in the library yet (cache still being updated by a scan),
            // keep the request pending so the scan completion can retry
            if (g_queue->Size > 0 || !hadSavedQueue)
                g_syncRequested = false;
        }
        catch (Exception^)
        {
        }
    }

    void OnSongQueueChanged()
    {
        SendQueue();
    }

    std::wstring ParentPath(const std::wstring& path)
    {
        const std::wstring::size_type slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }

    void CleanupEmptyPerleParent(const std::wstring& folderPath, const std::wstring& rootPath)
    {
        const std::wstring lowered = LowerWide(folderPath);
        const std::wstring loweredRoot = LowerWide(rootPath);
        if (folderPath.empty() || lowered == loweredRoot ||
            folderPath.size() <= rootPath.size() ||
            lowered.compare(0, loweredRoot.size(), loweredRoot) != 0)
            return;

        create_task(StorageFolder::GetFolderFromPathAsync(ToPlatform(folderPath))).then(
            [folderPath, rootPath](task<StorageFolder^> folderTask)
            {
                StorageFolder^ folder = nullptr;
                try { folder = folderTask.get(); } catch (Exception^) { return; }
                create_task(folder->GetFilesAsync()).then(
                    [folder, folderPath, rootPath](task<IVectorView<StorageFile^>^> filesTask)
                    {
                        IVectorView<StorageFile^>^ files = nullptr;
                        try { files = filesTask.get(); } catch (Exception^) { return; }
                        if (files->Size != 0)
                            return;
                        create_task(folder->GetFoldersAsync()).then(
                            [folder, folderPath, rootPath](task<IVectorView<StorageFolder^>^> foldersTask)
                            {
                                IVectorView<StorageFolder^>^ folders = nullptr;
                                try { folders = foldersTask.get(); } catch (Exception^) { return; }
                                if (folders->Size != 0)
                                    return;
                                create_task(folder->DeleteAsync(StorageDeleteOption::PermanentDelete)).then(
                                    [folderPath, rootPath](task<void> deleteTask)
                                    {
                                        try
                                        {
                                            deleteTask.get();
                                            CleanupEmptyPerleParent(ParentPath(folderPath), rootPath);
                                        }
                                        catch (Exception^) {}
                                    });
                            });
                    });
            });
    }
}

namespace
{
    std::vector<ValueSet^>& PendingCommands()
    {
        static std::vector<ValueSet^> value;
        return value;
    }
}

Song::Song()
{
    Path = "";
    Title = "";
    Artist = "Sconosciuto";
    AlbumArtist = "Sconosciuto";
    Album = "Sconosciuto";
    TagAlbum = "Sconosciuto";
    TagArtist = "Sconosciuto";
    TagAlbumArtist = "Sconosciuto";
    TrackNumber = 0;
    Year = 0;
    DurationSeconds = 0.0;
    AlbumKey = "";
}

AlbumItem::AlbumItem()
{
    Key = "";
    Name = "";
    Artist = "";
    CoverPath = "";
    Year = 0;
    Tracks = ref new Vector<Song^>();
    _cover = nullptr;
    CoverRequested = false;
    Summary = "";
}

PlaylistInfo::PlaylistInfo()
{
    Name = "";
    Tracks = ref new Vector<Song^>();
    Summary = "0 brani";
}

void MusicService::Initialize()
{
    if (g_initialized)
        return;

    g_initialized = true;
    g_songs = ref new Vector<Song^>();
    g_albums = ref new Vector<AlbumItem^>();
    g_artists = ref new Vector<String^>();
    g_playlists = ref new Vector<PlaylistInfo^>();
    g_queue = ref new Vector<Song^>();
    g_diagnostics = ref new Vector<String^>();

    try
    {
        g_playerDispatcher =
            Windows::ApplicationModel::Core::CoreApplication::MainView->CoreWindow->Dispatcher;
    }
    catch (Exception^)
    {
        g_playerDispatcher = nullptr;
    }

    EnsurePlayerMonitor();
    try
    {
        MediaPlayer^ player = BackgroundMediaPlayer::Current;
        if (player != nullptr && !g_playerStateEventAttached)
        {
            player->CurrentStateChanged +=
                ref new TypedEventHandler<MediaPlayer^, Object^>(&OnPlayerCurrentStateChanged);
            g_playerStateEventAttached = true;
        }
    }
    catch (Exception^)
    {
    }

    BackgroundMediaPlayer::MessageReceivedFromBackground +=
        ref new EventHandler<MediaPlayerDataReceivedEventArgs^>(&OnBackgroundMessage);

    Windows::ApplicationModel::Core::CoreApplication::Resuming +=
        ref new EventHandler<Object^>(&OnApplicationResuming);

    LoadLibrary();
    LoadPlaylists();
    // the scan is started only when the cache is empty (LoadLibrary completion): a full metadata scan at every
    // cold start made the app slow and competed with playback
    BeginHandshake(false);
}

unsigned int MusicService::AddStateChangedHandler(EventHandler<Object^>^ handler)
{
    Initialize();
    if (handler == nullptr)
        return 0;

    unsigned int token = g_nextStateHandlerToken++;
    if (token == 0)
        token = g_nextStateHandlerToken++;
    g_stateHandlers.push_back(std::make_pair(token, handler));
    return token;
}

void MusicService::RemoveStateChangedHandler(unsigned int token)
{
    if (token == 0)
        return;

    for (std::vector<std::pair<unsigned int, EventHandler<Object^>^> >::iterator it =
             g_stateHandlers.begin(); it != g_stateHandlers.end(); ++it)
    {
        if (it->first == token)
        {
            g_stateHandlers.erase(it);
            return;
        }
    }
}

void MusicService::StartPlayerMonitoring()
{
    Initialize();
    EnsurePlayerMonitor();
    ++g_playerMonitorCount;
    if (g_playerMonitorCount == 1 && g_playerTimer != nullptr)
        g_playerTimer->Start();
    RefreshPlayerState();
}

void MusicService::StopPlayerMonitoring()
{
    if (g_playerMonitorCount > 0)
        --g_playerMonitorCount;
    if (g_playerMonitorCount == 0 && g_playerTimer != nullptr)
        g_playerTimer->Stop();
}

void MusicService::RequestTaskHandshake()
{
    Initialize();
    LogDiagnostic("stato", "richiesta handshake");
    BeginHandshake(true);
}

void MusicService::Rescan()
{
    Initialize();
    ScanLibrary();
}

bool MusicService::IsScanning() { return g_scanning; }

void MusicService::Synchronize()
{
    Initialize();
    g_syncRequested = true;
    RequestTaskHandshake();
    // the task may have been playing since before this process existed: ask it for the authoritative
    // title/position/index instead of waiting for a queue command (which needs the library to be loaded)
    SendCommand("state");

    try
    {
        IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;

        if (values->HasKey("shuffle"))
            g_shuffle = safe_cast<bool>(values->Lookup("shuffle"));
        if (values->HasKey("repeat"))
            g_repeat = safe_cast<int>(values->Lookup("repeat"));
        if (values->HasKey("queueIndex"))
            g_queueIndex = safe_cast<unsigned int>(values->Lookup("queueIndex"));
        if (values->HasKey("positionTicks"))
            g_position = static_cast<double>(safe_cast<long long>(
                values->Lookup("positionTicks"))) / 10000000.0;

        // same process resumed: the queue is already in memory and the task still has it, only the state
        // reply is needed. A cold start has an empty queue and must rebuild it from disk.
        if (g_queue->Size == 0)
            RestoreQueueFromDisk();
        else
            g_syncRequested = false;
    }
    catch (Exception^) {}
}

void MusicService::SavePlaybackState()
{
    try
    {
        IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
        std::wstring paths;
        for (unsigned int i = 0; g_queue != nullptr && i < g_queue->Size; ++i)
        {
            if (i != 0)
                paths += L"\n";
            paths += ToWide(g_queue->GetAt(i)->Path);
        }
        // in a file: a long queue went over the 8 KB of a LocalSettings value and was not saved
        if (WriteLocalText(L"queue.txt", paths) && values->HasKey("queue"))
            values->Remove("queue");
        values->Insert("shuffle", g_shuffle);
        values->Insert("repeat", g_repeat);
        values->Insert("positionTicks", static_cast<long long>(g_position * 10000000.0));
        values->Insert("queueIndex", g_queueIndex);
    }
    catch (Exception^) {}
    SendCommand("state");
}

IVector<Song^>^ MusicService::GetSongs() { Initialize(); return g_songs; }
IVector<Song^>^ MusicService::GetQueue() { Initialize(); return g_queue; }
IVector<AlbumItem^>^ MusicService::GetAlbums() { Initialize(); return g_albums; }
IVector<String^>^ MusicService::GetArtists() { Initialize(); return g_artists; }
IVector<PlaylistInfo^>^ MusicService::GetPlaylists() { Initialize(); return g_playlists; }

IVector<Song^>^ MusicService::SongsForArtist(String^ artist)
{
    Initialize();
    Vector<Song^>^ result = ref new Vector<Song^>();
    const std::wstring key = MusicaCore::normalizedName(ToWide(artist));
    for (unsigned int i = 0; i < g_songs->Size; ++i)
    {
        Song^ song = g_songs->GetAt(i);
        const std::wstring artistName = ToWide(song->AlbumArtist).empty()
            ? ToWide(song->Artist) : ToWide(song->AlbumArtist);
        if (MusicaCore::normalizedName(artistName) == key)
            result->Append(song);
    }
    return result;
}

AlbumItem^ MusicService::FindAlbum(String^ key)
{
    Initialize();
    for (unsigned int i = 0; i < g_albums->Size; ++i)
        if (ToWide(g_albums->GetAt(i)->Key) == ToWide(key))
            return g_albums->GetAt(i);
    return nullptr;
}

Song^ MusicService::FindSong(String^ path)
{
    if (g_songs == nullptr)
        return nullptr;
    for (unsigned int i = 0; i < g_songs->Size; ++i)
        if (ToWide(g_songs->GetAt(i)->Path) == ToWide(path))
            return g_songs->GetAt(i);
    return nullptr;
}

String^ MusicService::GetNowTitle() { return g_nowTitle; }
String^ MusicService::GetNowArtist() { return g_nowArtist; }
String^ MusicService::GetNowAlbum() { return g_nowAlbum; }
String^ MusicService::GetNowPath() { return g_nowPath; }
bool MusicService::IsPlaying()
{
    return g_playerState == MediaPlayerState::Playing ||
        g_playerState == MediaPlayerState::Buffering ||
        g_playerState == MediaPlayerState::Opening;
}
double MusicService::GetPositionSeconds() { return g_position; }
double MusicService::GetDurationSeconds() { return g_duration; }
bool MusicService::GetShuffle() { return g_shuffle; }
int MusicService::GetRepeatMode() { return g_repeat; }
bool MusicService::IsTaskReady() { return g_taskReady; }
unsigned int MusicService::GetSongCount() { return g_songs == nullptr ? 0 : g_songs->Size; }
unsigned int MusicService::GetLibraryVersion() { return g_libraryVersion; }
String^ MusicService::GetScanStatus() { return g_scanStatus; }

IVector<String^>^ MusicService::GetDiagnosticLines()
{
    Initialize();
    Vector<String^>^ result = ref new Vector<String^>();
    result->Append("Brani: " + GetSongCount().ToString());
    result->Append("Scansione: " + g_scanStatus);
    result->Append("Errori scansione: " + g_scanErrors.ToString());
    result->Append(g_taskReady ? "Task pronto: si" : "Task pronto: no");
    for (unsigned int i = 0; i < g_diagnostics->Size; ++i)
        result->Append(g_diagnostics->GetAt(i));
    return result;
}

void MusicService::PlaySongs(IVector<Song^>^ songs, unsigned int index, bool shuffle)
{
    Initialize();
    g_queue->Clear();
    if (songs == nullptr || songs->Size == 0)
        return;
    if (index >= songs->Size)
        index = 0;

    std::vector<std::size_t> order;
    if (shuffle)
    {
        order = MusicaCore::shuffleOrder(songs->Size,
            static_cast<std::uint32_t>(GetTickCount64()));
        std::size_t selected = 0;
        for (std::size_t i = 0; i < order.size(); ++i)
            if (order[i] == index) { selected = i; break; }
        if (selected != 0 && !order.empty())
            std::swap(order[0], order[selected]);
    }
    else
    {
        for (unsigned int i = index; i < songs->Size; ++i)
            order.push_back(i);
        for (unsigned int i = 0; i < index; ++i)
            order.push_back(i);
    }

    for (std::vector<std::size_t>::const_iterator it = order.begin();
         it != order.end(); ++it)
        g_queue->Append(songs->GetAt(static_cast<unsigned int>(*it)));

    if (g_queue->Size == 0)
        return;
    g_queueIndex = 0;
    g_shuffle = shuffle;
    g_position = 0.0;
    g_duration = 0.0;
    Song^ first = g_queue->GetAt(0);
    g_nowTitle = first->Title;
    g_nowArtist = first->Artist;
    g_nowAlbum = first->Album;
    g_nowPath = first->Path;
    SendQueue();
    SendCommand("play");
    RaiseStateChanged();
}

void MusicService::PlayAlbum(AlbumItem^ album, bool shuffle)
{
    if (album != nullptr)
        PlaySongs(album->Tracks, 0, shuffle);
}

void MusicService::PlayPlaylist(PlaylistInfo^ playlist)
{
    if (playlist != nullptr)
        PlaySongs(playlist->Tracks, 0, false);
}

void MusicService::PlaySong(Song^ song)
{
    if (song == nullptr)
        return;
    Vector<Song^>^ one = ref new Vector<Song^>();
    one->Append(song);
    PlaySongs(one, 0, false);
}

void MusicService::PlayAfter(Song^ song)
{
    Initialize();
    if (song == nullptr)
        return;
    if (g_queue->Size == 0)
    {
        PlaySong(song);
        return;
    }
    const unsigned int insertAt = g_queueIndex < g_queue->Size
        ? g_queueIndex + 1 : g_queue->Size;
    g_queue->InsertAt(insertAt, song);
    SendQueue();
}

void MusicService::AddToQueue(Song^ song)
{
    Initialize();
    if (song == nullptr)
        return;
    if (g_queue->Size == 0)
    {
        PlaySong(song);
        return;
    }
    g_queue->Append(song);
    SendQueue();
}

void MusicService::PlayOrPause() { SendCommand("toggle"); }
void MusicService::Next() { SendCommand("next"); }
void MusicService::Previous() { SendCommand("previous"); }

void MusicService::Seek(double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;
    ValueSet^ message = ref new ValueSet();
    message->Insert("cmd", "seek");
    message->Insert("positionTicks", static_cast<long long>(seconds * 10000000.0));
    Dispatch(message);
    g_position = seconds;
    RaiseStateChanged();
}

void MusicService::ToggleShuffle()
{
    g_shuffle = !g_shuffle;
    SendQueue();
    RaiseStateChanged();
}

void MusicService::ToggleRepeat()
{
    g_repeat = (g_repeat + 1) % 3;
    ValueSet^ message = ref new ValueSet();
    message->Insert("cmd", "repeat");
    message->Insert("repeat", g_repeat);
    Dispatch(message);
    RaiseStateChanged();
}

IAsyncOperation<unsigned int>^ MusicService::DeleteSongsAsync(IVector<String^>^ paths)
{
    Initialize();
    return create_async([paths]() -> task<unsigned int>
    {
        std::shared_ptr<std::vector<std::wstring> > deleted =
            std::make_shared<std::vector<std::wstring> >();
        std::shared_ptr<unsigned int> failures = std::make_shared<unsigned int>(0);
        task<void> chain = create_task([]() {});

        if (paths != nullptr)
        {
            for (unsigned int i = 0; i < paths->Size; ++i)
            {
                const std::wstring path = ToWide(paths->GetAt(i));
                if (path.empty())
                    continue;
                chain = chain.then([path, deleted, failures]() -> task<void>
                {
                    return create_task(StorageFile::GetFileFromPathAsync(ToPlatform(path))).then(
                        [path, deleted, failures](task<StorageFile^> fileTask) -> task<void>
                        {
                            StorageFile^ file = nullptr;
                            try { file = fileTask.get(); }
                            catch (Exception^) { ++(*failures); return create_task([]() {}); }
                            return create_task(file->DeleteAsync(StorageDeleteOption::PermanentDelete)).then(
                                [path, deleted, failures](task<void> deleteTask)
                                {
                                    try { deleteTask.get(); deleted->push_back(path); }
                                    catch (Exception^) { ++(*failures); }
                                });
                        });
                });
            }
        }

        return chain.then([deleted, failures]() -> unsigned int
        {
            if (deleted->empty())
                return *failures;

            const std::wstring currentPath = ToWide(g_nowPath);
            bool currentDeleted = false;

            for (unsigned int i = 0; g_songs != nullptr && i < g_songs->Size;)
            {
                const std::wstring path = ToWide(g_songs->GetAt(i)->Path);
                bool remove = false;
                for (std::vector<std::wstring>::const_iterator it = deleted->begin();
                     it != deleted->end(); ++it)
                    if (path == *it) { remove = true; break; }
                if (remove) g_songs->RemoveAt(i); else ++i;
            }

            for (unsigned int p = 0; g_playlists != nullptr && p < g_playlists->Size; ++p)
            {
                for (std::vector<std::wstring>::const_iterator it = deleted->begin();
                     it != deleted->end(); ++it)
                    ErasePath(g_playlists->GetAt(p)->StoredPaths, *it);
                IVector<Song^>^ tracks = g_playlists->GetAt(p)->Tracks;
                for (unsigned int i = 0; i < tracks->Size;)
                {
                    bool remove = false;
                    const std::wstring path = ToWide(tracks->GetAt(i)->Path);
                    for (std::vector<std::wstring>::const_iterator it = deleted->begin();
                         it != deleted->end(); ++it)
                        if (path == *it) { remove = true; break; }
                    if (remove) tracks->RemoveAt(i); else ++i;
                }
                UpdatePlaylistSummary(g_playlists->GetAt(p));
            }

            for (unsigned int i = 0; g_queue != nullptr && i < g_queue->Size;)
            {
                const std::wstring path = ToWide(g_queue->GetAt(i)->Path);
                bool remove = false;
                for (std::vector<std::wstring>::const_iterator it = deleted->begin();
                     it != deleted->end(); ++it)
                    if (path == *it) { remove = true; break; }
                if (!remove) { ++i; continue; }
                if (path == currentPath) currentDeleted = true;
                if (i < g_queueIndex && g_queueIndex > 0) --g_queueIndex;
                g_queue->RemoveAt(i);
            }

            if (g_queue != nullptr && g_queue->Size == 0)
            {
                g_queueIndex = 0;
                g_nowTitle = "Nessun brano";
                g_nowArtist = "";
                g_nowAlbum = "";
                g_nowPath = "";
                g_duration = 0.0;
                g_position = 0.0;
                g_playing = false;
            }
            else if (g_queue != nullptr && g_queueIndex >= g_queue->Size)
                g_queueIndex = 0;

            if (currentDeleted && g_queue != nullptr && g_queue->Size > 0)
            {
                Song^ next = g_queue->GetAt(g_queueIndex);
                g_nowTitle = next->Title;
                g_nowArtist = next->Artist;
                g_nowAlbum = next->Album;
                g_nowPath = next->Path;
                g_duration = next->DurationSeconds;
                g_position = 0.0;
            }

            RebuildGroups();
            SaveLibrary();
            MusicService::SavePlaylists();
            MusicService::SavePlaybackState();

            const std::wstring musicPath = ToWide(KnownFolders::MusicLibrary->Path);
            const std::wstring rootPath = musicPath + L"\\Perle";
            for (std::vector<std::wstring>::const_iterator it = deleted->begin();
                 it != deleted->end(); ++it)
            {
                const std::wstring parent = ParentPath(*it);
                const std::wstring lowParent = LowerWide(parent);
                const std::wstring lowRoot = LowerWide(rootPath);
                if (lowParent.size() > lowRoot.size() &&
                    lowParent.compare(0, lowRoot.size(), lowRoot) == 0)
                    CleanupEmptyPerleParent(parent, rootPath);
            }

            SendQueue();
            if (currentDeleted)
            {
                if (g_queue != nullptr && g_queue->Size > 0)
                    SendCommand("play");
                else
                    SendCommand("stop");
            }
            RaiseStateChanged();
            return *failures;
        }, task_continuation_context::use_current());
    });
}

void MusicService::RenameAlbum(AlbumItem^ album, String^ name)
{
    if (album == nullptr)
        return;
    const std::wstring trimmed = TrimWide(ToWide(name));
    if (trimmed.empty())
        return;

    String^ newName = ToPlatform(trimmed);
    for (unsigned int i = 0; i < album->Tracks->Size; ++i)
    {
        Song^ song = album->Tracks->GetAt(i);
        SetName("albums", TagAlbumKey(song), newName, song->TagAlbum);
        song->Album = newName;
        song->AlbumKey = ToPlatform(MusicaCore::albumKey(ToCore(song)));
    }
    SaveNames();
    RebuildGroups();
    SaveLibrary();
    RaiseStateChanged();
}

void MusicService::RenameSong(Song^ song, String^ title)
{
    if (song == nullptr)
        return;
    const std::wstring trimmed = TrimWide(ToWide(title));
    if (trimmed.empty())
        return;
    song->Title = ToPlatform(trimmed);
    SetName("titles", song->Path, song->Title, nullptr);
    SaveNames();
    RebuildGroups();
    SaveLibrary();
    RaiseStateChanged();
}

void MusicService::RenameArtist(String^ artist, String^ name)
{
    const std::wstring trimmed = TrimWide(ToWide(name));
    if (artist == nullptr || trimmed.empty() || g_songs == nullptr)
        return;
    String^ newName = ToPlatform(trimmed);
    for (unsigned int i = 0; i < g_songs->Size; ++i)
    {
        Song^ song = g_songs->GetAt(i);
        if (song->Artist == artist)
        {
            SetName("artists", song->TagArtist, newName, song->TagArtist);
            song->Artist = newName;
        }
        if (song->AlbumArtist == artist)
        {
            SetName("artists", song->TagAlbumArtist, newName, song->TagAlbumArtist);
            song->AlbumArtist = newName;
        }
        song->AlbumKey = ToPlatform(MusicaCore::albumKey(ToCore(song)));
    }
    SaveNames();
    RebuildGroups();
    SaveLibrary();
    RaiseStateChanged();
}

void MusicService::CreatePlaylist(String^ name)
{
    Initialize();
    const std::wstring trimmed = TrimWide(ToWide(name));
    if (trimmed.empty())
        return;

    for (unsigned int i = 0; i < g_playlists->Size; ++i)
        if (LowerWide(ToWide(g_playlists->GetAt(i)->Name)) == LowerWide(trimmed))
            return;

    PlaylistInfo^ playlist = ref new PlaylistInfo();
    playlist->Name = ToPlatform(trimmed);
    UpdatePlaylistSummary(playlist);
    g_playlists->Append(playlist);
    SavePlaylists();
}

void MusicService::RenamePlaylist(PlaylistInfo^ playlist, String^ name)
{
    if (playlist == nullptr)
        return;
    const std::wstring trimmed = TrimWide(ToWide(name));
    if (trimmed.empty())
        return;
    playlist->Name = ToPlatform(trimmed);
    for (unsigned int i = 0; g_playlists != nullptr && i < g_playlists->Size; ++i)
        if (g_playlists->GetAt(i) == playlist)
        {
            g_playlists->SetAt(i, playlist);
            break;
        }
    SavePlaylists();
}

void MusicService::DeletePlaylist(PlaylistInfo^ playlist)
{
    if (playlist == nullptr || g_playlists == nullptr)
        return;
    for (unsigned int i = 0; i < g_playlists->Size; ++i)
        if (g_playlists->GetAt(i) == playlist)
        {
            g_playlists->RemoveAt(i);
            break;
        }
    SavePlaylists();
}

void MusicService::AddToPlaylist(String^ playlistName, Song^ song)
{
    if (song == nullptr || g_playlists == nullptr)
        return;
    for (unsigned int i = 0; i < g_playlists->Size; ++i)
    {
        PlaylistInfo^ playlist = g_playlists->GetAt(i);
        if (ToWide(playlist->Name) == ToWide(playlistName))
        {
            // already there (a Spotify list imported again, a song added twice): nothing to do
            const std::wstring key = PathKey(ToWide(song->Path));
            for (size_t j = 0; j < playlist->StoredPaths.size(); ++j)
                if (PathKey(playlist->StoredPaths[j]) == key)
                    return;
            playlist->StoredPaths.push_back(ToWide(song->Path));
            playlist->Tracks->Append(song);
            UpdatePlaylistSummary(playlist);
            SavePlaylists();
            return;
        }
    }
}

void MusicService::AddSongsToPlaylist(String^ playlistName, IVector<Song^>^ songs)
{
    if (songs == nullptr)
        return;
    for (unsigned int i = 0; i < songs->Size; ++i)
        AddToPlaylist(playlistName, songs->GetAt(i));
}

void MusicService::RemoveFromPlaylist(PlaylistInfo^ playlist, Song^ song)
{
    if (playlist == nullptr || song == nullptr)
        return;
    ErasePath(playlist->StoredPaths, ToWide(song->Path));
    for (unsigned int i = 0; i < playlist->Tracks->Size; ++i)
        if (playlist->Tracks->GetAt(i) == song)
        {
            playlist->Tracks->RemoveAt(i);
            break;
        }
    UpdatePlaylistSummary(playlist);
    SavePlaylists();
}

void MusicService::SavePlaylists()
{
    if (g_playlists == nullptr)
        return;
    JsonArray^ array = ref new JsonArray();
    for (unsigned int i = 0; i < g_playlists->Size; ++i)
    {
        PlaylistInfo^ playlist = g_playlists->GetAt(i);
        JsonObject^ item = ref new JsonObject();
        item->SetNamedValue("name", JsonValue::CreateStringValue(playlist->Name));
        JsonArray^ paths = ref new JsonArray();
        for (size_t j = 0; j < playlist->StoredPaths.size(); ++j)
            paths->Append(JsonValue::CreateStringValue(ToPlatform(playlist->StoredPaths[j])));
        item->SetNamedValue("paths", paths);
        array->Append(item);
    }
    if (WriteLocalText(L"playlists.json", ToWide(array->Stringify())))
    {
        try
        {
            IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
            if (values->HasKey("playlists"))
                values->Remove("playlists");
        }
        catch (Exception^) {}
    }
}

String^ MusicService::FormatTime(double seconds)
{
    if (!(seconds > 0.0))
        seconds = 0.0;
    const double maximum = static_cast<double>((std::numeric_limits<unsigned int>::max)());
    if (seconds > maximum)
        seconds = maximum;
    const unsigned int total = static_cast<unsigned int>(seconds);
    wchar_t buffer[24];
    swprintf_s(buffer, L"%u:%02u", total / 60, total % 60);
    return ref new String(buffer);
}
