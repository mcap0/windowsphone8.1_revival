#pragma once

#include "pch.h"
#include "Services.h"
#include "Core/Online.h"

namespace Musica
{
    // Where a song goes when it is part of an album: Music\Album\<artist - album>, with the album's tags and cover.
    // The default (no name) is a single song in Music\Online.
    struct AlbumTarget
    {
        std::string name, artist, cover;
        int number, total;
        AlbumTarget() : number(0), total(0) {}
    };

    // HTTP (WinRT HttpClient) and files for Core/Online: search, download + tag + copy into the music library.
    class OnlineService
    {
    public:
        static concurrency::task<std::string> Fetch(const online::Request& r);
        static concurrency::task<std::vector<online::Song>> Search(const std::string& query);
        static concurrency::task<online::SpotifyList> Spotify(const std::string& link);
        // The song as a file of the music library (downloaded once, then found there): ready to play or add to a
        // playlist. progress(percent) is called on the UI thread.
        static concurrency::task<Song^> Get(const online::Song& s, std::function<void(int)> progress,
                                            const AlbumTarget& album = AlbumTarget());
        // the library is rescanned when the online page is left after downloads
        static bool downloadedSomething;
    };

    std::wstring Utf16(const std::string& s);
    std::string Utf8(const std::wstring& w);
}
