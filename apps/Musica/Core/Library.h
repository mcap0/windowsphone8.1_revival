#ifndef MUSICA_CORE_LIBRARY_H
#define MUSICA_CORE_LIBRARY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MusicaCore
{
    struct Track
    {
        std::wstring path;
        std::wstring title;
        std::wstring artist;
        std::wstring albumArtist;
        std::wstring album;
        std::uint32_t trackNumber;
        std::uint32_t year;

        Track();
    };

    struct Album
    {
        std::wstring key;
        std::wstring name;
        std::wstring artist;
        std::uint32_t year;
        std::vector<Track> tracks;

        Album();
    };

    struct Artist
    {
        std::wstring name;
        std::vector<Album> albums;
    };

    std::wstring normalizedName(const std::wstring& value);
    std::wstring albumKey(const Track& track);
    std::vector<Track> sortTracks(const std::vector<Track>& tracks);
    std::vector<Album> groupAlbums(const std::vector<Track>& tracks);
    std::vector<Artist> groupArtists(const std::vector<Track>& tracks);
    std::vector<std::size_t> shuffleOrder(std::size_t count, std::uint32_t seed);

    // repeatMode: 0 = off, 1 = all, 2 = one.
    std::size_t nextIndex(std::size_t index, std::size_t count, int repeatMode);
    std::size_t previousIndex(std::size_t index, std::size_t count, int repeatMode);
    bool canAdvance(std::size_t index, std::size_t count, int repeatMode);
}

#endif
