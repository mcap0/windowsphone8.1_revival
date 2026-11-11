#pragma once

#include "pch.h"

namespace Musica
{
    [Windows::UI::Xaml::Data::Bindable]
    public ref class Song sealed
    {
    public:
        Song();

        property Platform::String^ Path;
        property Platform::String^ Title;
        property Platform::String^ Artist;
        property Platform::String^ AlbumArtist;
        property Platform::String^ Album;
        property Platform::String^ TagAlbum;
        property Platform::String^ TagArtist;
        property Platform::String^ TagAlbumArtist;
        property unsigned int TrackNumber;
        property unsigned int Year;
        property double DurationSeconds;
        property Platform::String^ AlbumKey;
    };

    [Windows::UI::Xaml::Data::Bindable]
    public ref class AlbumItem sealed : Windows::UI::Xaml::Data::INotifyPropertyChanged
    {
    public:
        AlbumItem();

        virtual event Windows::UI::Xaml::Data::PropertyChangedEventHandler^ PropertyChanged;

        property Platform::String^ Key;
        property Platform::String^ Name;
        property Platform::String^ Artist;
        property unsigned int Year;
        property Platform::String^ CoverPath;
        property Windows::UI::Xaml::Media::ImageSource^ Cover
        {
            Windows::UI::Xaml::Media::ImageSource^ get() { return _cover; }
            void set(Windows::UI::Xaml::Media::ImageSource^ value)
            {
                _cover = value;
                PropertyChanged(this, ref new Windows::UI::Xaml::Data::PropertyChangedEventArgs(L"Cover"));
            }
        }
        property Windows::Foundation::Collections::IVector<Song^>^ Tracks;
        property Platform::String^ Summary;

    internal:
        // a cover load is in flight: LoadCovers must not start it again on the next state change
        bool CoverRequested;

    private:
        Windows::UI::Xaml::Media::ImageSource^ _cover;
    };

    [Windows::UI::Xaml::Data::Bindable]
    public ref class PlaylistInfo sealed
    {
    public:
        PlaylistInfo();

        property Platform::String^ Name;
        property Windows::Foundation::Collections::IVector<Song^>^ Tracks;
        property Platform::String^ Summary;

    internal:
        // every path of the playlist, in order, also those not (yet) in the library: Tracks is rebuilt from these
        // after each library load / scan, so a song missing for a moment is never dropped from the saved list
        std::vector<std::wstring> StoredPaths;
    };

    class MusicService
    {
    public:
        static void Initialize();
        static void Synchronize();
        static void Rescan();
        static bool IsScanning();
        static void SavePlaybackState();
        static void RequestTaskHandshake();

        static unsigned int AddStateChangedHandler(
            Windows::Foundation::EventHandler<Platform::Object^>^ handler);
        static void RemoveStateChangedHandler(unsigned int token);
        static void StartPlayerMonitoring();
        static void StopPlayerMonitoring();

        static Windows::Foundation::Collections::IVector<Song^>^ GetSongs();
        static Windows::Foundation::Collections::IVector<Song^>^ GetQueue();
        static Windows::Foundation::Collections::IVector<AlbumItem^>^ GetAlbums();
        static Windows::Foundation::Collections::IVector<Platform::String^>^ GetArtists();
        static Windows::Foundation::Collections::IVector<PlaylistInfo^>^ GetPlaylists();
        static Windows::Foundation::Collections::IVector<Song^>^ SongsForArtist(Platform::String^ artist);
        static AlbumItem^ FindAlbum(Platform::String^ key);
        static Song^ FindSong(Platform::String^ path);

        static Platform::String^ GetNowTitle();
        static Platform::String^ GetNowArtist();
        static Platform::String^ GetNowAlbum();
        static Platform::String^ GetNowPath();
        static bool IsPlaying();
        static double GetPositionSeconds();
        static double GetDurationSeconds();

        static void PlaySongs(
            Windows::Foundation::Collections::IVector<Song^>^ songs,
            unsigned int index,
            bool shuffle);
        static void PlayAlbum(AlbumItem^ album, bool shuffle);
        static void PlayPlaylist(PlaylistInfo^ playlist);
        static void PlayOrPause();
        static void Next();
        static void Previous();
        static void Seek(double seconds);
        static void ToggleShuffle();
        static void ToggleRepeat();
        static int GetRepeatMode();
        static bool GetShuffle();
        static bool IsTaskReady();

        static Windows::Foundation::Collections::IVector<Platform::String^>^ GetDiagnosticLines();
        static unsigned int GetSongCount();
        static unsigned int GetLibraryVersion();
        static Platform::String^ GetScanStatus();

        static void PlaySong(Song^ song);
        static void PlayAfter(Song^ song);
        static void AddToQueue(Song^ song);

        static Windows::Foundation::IAsyncOperation<unsigned int>^ DeleteSongsAsync(
            Windows::Foundation::Collections::IVector<Platform::String^>^ paths);

        static void RenameAlbum(AlbumItem^ album, Platform::String^ name);
        static void RenameSong(Song^ song, Platform::String^ title);
        static void RenameArtist(Platform::String^ artist, Platform::String^ name);

        static void CreatePlaylist(Platform::String^ name);
        static void RenamePlaylist(PlaylistInfo^ playlist, Platform::String^ name);
        static void DeletePlaylist(PlaylistInfo^ playlist);
        static void AddToPlaylist(Platform::String^ playlistName, Song^ song);
        static void AddSongsToPlaylist(
            Platform::String^ playlistName,
            Windows::Foundation::Collections::IVector<Song^>^ songs);
        static void RemoveFromPlaylist(PlaylistInfo^ playlist, Song^ song);
        static void SavePlaylists();
        static Platform::String^ FormatTime(double seconds);
    };
}
