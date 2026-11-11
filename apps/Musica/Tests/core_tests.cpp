#include "../Core/Library.h"

#include <cassert>
#include <string>
#include <vector>

using namespace MusicaCore;

static Track makeTrack(const wchar_t* title, const wchar_t* album,
                       const wchar_t* artist, unsigned number)
{
    Track track;
    track.title = title;
    track.album = album;
    track.artist = artist;
    track.albumArtist = artist;
    track.trackNumber = number;
    return track;
}

int main()
{
    std::vector<Track> tracks;
    tracks.push_back(makeTrack(L"zeta", L"Primo", L"Artista", 2));
    tracks.push_back(makeTrack(L"alfa", L"Primo", L"Artista", 1));
    tracks.push_back(makeTrack(L"Beta", L"secondo", L"artista", 1));
    tracks.push_back(makeTrack(L"senza artista", L"Sconosciuto", L"", 0));

    const std::vector<Track> sorted = sortTracks(tracks);
    assert(sorted.size() == 4);
    assert(sorted[0].title == L"alfa");

    const std::vector<Album> albums = groupAlbums(tracks);
    assert(albums.size() == 3);

    bool foundPrimo = false;
    for (std::vector<Album>::const_iterator album = albums.begin();
         album != albums.end(); ++album)
    {
        if (normalizedName(album->name) == L"primo")
        {
            foundPrimo = true;
            assert(album->tracks.size() == 2);
            assert(album->tracks[0].title == L"alfa");
            assert(album->tracks[1].title == L"zeta");
        }
    }
    assert(foundPrimo);

    const std::vector<Artist> artists = groupArtists(tracks);
    assert(artists.size() == 2);
    assert(normalizedName(artists[0].name) == L"artista");

    const std::vector<std::size_t> firstShuffle = shuffleOrder(20, 12345);
    const std::vector<std::size_t> secondShuffle = shuffleOrder(20, 12345);
    assert(firstShuffle == secondShuffle);
    assert(firstShuffle.size() == 20);
    for (std::size_t i = 0; i < firstShuffle.size(); ++i)
        assert(firstShuffle[i] < 20);

    assert(nextIndex(0, 3, 0) == 1);
    assert(nextIndex(2, 3, 0) == 2);
    assert(nextIndex(2, 3, 1) == 0);
    assert(nextIndex(1, 3, 2) == 1);
    assert(previousIndex(0, 3, 0) == 0);
    assert(previousIndex(0, 3, 1) == 2);
    assert(canAdvance(1, 3, 0));
    assert(!canAdvance(2, 3, 0));
    assert(canAdvance(2, 3, 1));
    assert(nextIndex(0, 0, 1) == 0);

    return 0;
}
