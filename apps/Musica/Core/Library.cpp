#include "Library.h"

#include <algorithm>
#include <cwctype>
#include <map>
#include <numeric>
#include <random>

namespace MusicaCore
{
    namespace
    {
        std::wstring trim(const std::wstring& value)
        {
            std::wstring::size_type first = 0;
            while (first < value.size() && std::iswspace(value[first]))
                ++first;

            std::wstring::size_type last = value.size();
            while (last > first && std::iswspace(value[last - 1]))
                --last;

            return value.substr(first, last - first);
        }

        bool trackLess(const Track& left, const Track& right)
        {
            const std::wstring leftTitle = normalizedName(left.title);
            const std::wstring rightTitle = normalizedName(right.title);
            if (leftTitle != rightTitle)
                return leftTitle < rightTitle;

            return normalizedName(left.path) < normalizedName(right.path);
        }

        bool albumLess(const Album& left, const Album& right)
        {
            const std::wstring leftName = normalizedName(left.name);
            const std::wstring rightName = normalizedName(right.name);
            if (leftName != rightName)
                return leftName < rightName;

            return normalizedName(left.artist) < normalizedName(right.artist);
        }
    }

    Track::Track() : trackNumber(0), year(0)
    {
    }

    Album::Album() : year(0)
    {
    }

    std::wstring normalizedName(const std::wstring& value)
    {
        std::wstring result = trim(value);
        for (std::wstring::size_type i = 0; i < result.size(); ++i)
            result[i] = static_cast<wchar_t>(std::towlower(result[i]));
        return result;
    }

    std::wstring albumKey(const Track& track)
    {
        std::wstring artist = trim(track.albumArtist);
        if (artist.empty())
            artist = trim(track.artist);
        if (artist.empty())
            artist = L"Sconosciuto";

        std::wstring album = trim(track.album);
        if (album.empty())
            album = L"Sconosciuto";

        return normalizedName(artist) + L"|" + normalizedName(album);
    }

    std::vector<Track> sortTracks(const std::vector<Track>& tracks)
    {
        // normalize once per track instead of twice per comparison inside std::sort
        struct Entry
        {
            std::wstring titleKey;
            std::wstring pathKey;
            std::size_t index;
        };

        std::vector<Entry> entries;
        entries.reserve(tracks.size());
        for (std::size_t i = 0; i < tracks.size(); ++i)
        {
            Entry entry;
            entry.titleKey = normalizedName(tracks[i].title);
            // '"Heroes"' under H, "(Don't Fear) The Reaper" under D: the letter on the list is the letter of the title
            std::size_t skip = 0;
            while (skip < entry.titleKey.size() && !std::iswalnum(entry.titleKey[skip]))
                ++skip;
            if (skip < entry.titleKey.size())
                entry.titleKey.erase(0, skip);
            entry.pathKey = normalizedName(tracks[i].path);
            entry.index = i;
            entries.push_back(entry);
        }

        std::sort(entries.begin(), entries.end(),
            [](const Entry& left, const Entry& right)
            {
                if (left.titleKey != right.titleKey)
                    return left.titleKey < right.titleKey;
                if (left.pathKey != right.pathKey)
                    return left.pathKey < right.pathKey;
                return left.index < right.index;
            });

        std::vector<Track> result;
        result.reserve(entries.size());
        for (std::vector<Entry>::const_iterator entry = entries.begin();
             entry != entries.end(); ++entry)
            result.push_back(tracks[entry->index]);
        return result;
    }

    std::vector<Album> groupAlbums(const std::vector<Track>& tracks)
    {
        std::vector<Album> result;
        // album key -> position in result, instead of a linear scan for every track
        std::map<std::wstring, std::size_t> positions;

        for (std::vector<Track>::const_iterator track = tracks.begin();
             track != tracks.end(); ++track)
        {
            const std::wstring key = albumKey(*track);
            std::map<std::wstring, std::size_t>::const_iterator found = positions.find(key);
            std::size_t position = 0;
            if (found == positions.end())
            {
                Album album;
                album.key = key;
                album.name = trim(track->album);
                if (album.name.empty())
                    album.name = L"Sconosciuto";

                album.artist = trim(track->albumArtist);
                if (album.artist.empty())
                    album.artist = trim(track->artist);
                if (album.artist.empty())
                    album.artist = L"Sconosciuto";

                album.year = track->year;
                result.push_back(album);
                position = result.size() - 1;
                positions.insert(std::make_pair(key, position));
            }
            else
            {
                position = found->second;
                if (result[position].year == 0 && track->year != 0)
                    result[position].year = track->year;
            }

            result[position].tracks.push_back(*track);
        }

        for (std::vector<Album>::iterator album = result.begin();
             album != result.end(); ++album)
        {
            std::stable_sort(album->tracks.begin(), album->tracks.end(),
                [](const Track& left, const Track& right)
                {
                    if (left.trackNumber != right.trackNumber)
                    {
                        if (left.trackNumber == 0)
                            return false;
                        if (right.trackNumber == 0)
                            return true;
                        return left.trackNumber < right.trackNumber;
                    }
                    return trackLess(left, right);
                });
        }

        std::sort(result.begin(), result.end(), albumLess);
        return result;
    }

    std::vector<Artist> groupArtists(const std::vector<Track>& tracks)
    {
        std::vector<Artist> result;
        std::map<std::wstring, std::size_t> positions;
        const std::vector<Album> albums = groupAlbums(tracks);

        for (std::vector<Album>::const_iterator album = albums.begin();
             album != albums.end(); ++album)
        {
            const std::wstring key = normalizedName(album->artist);
            std::map<std::wstring, std::size_t>::const_iterator found = positions.find(key);
            std::size_t position = 0;
            if (found == positions.end())
            {
                Artist newArtist;
                newArtist.name = album->artist;
                result.push_back(newArtist);
                position = result.size() - 1;
                positions.insert(std::make_pair(key, position));
            }
            else
                position = found->second;

            result[position].albums.push_back(*album);
        }

        std::sort(result.begin(), result.end(),
            [](const Artist& left, const Artist& right)
            {
                return normalizedName(left.name) < normalizedName(right.name);
            });

        return result;
    }

    std::vector<std::size_t> shuffleOrder(std::size_t count, std::uint32_t seed)
    {
        std::vector<std::size_t> result(count);
        std::iota(result.begin(), result.end(), static_cast<std::size_t>(0));
        std::mt19937 generator(seed);
        std::shuffle(result.begin(), result.end(), generator);
        return result;
    }

    std::size_t nextIndex(std::size_t index, std::size_t count, int repeatMode)
    {
        if (count == 0)
            return 0;
        if (repeatMode == 2)
            return index < count ? index : 0;
        if (index + 1 < count)
            return index + 1;
        return repeatMode == 1 ? 0 : index;
    }

    std::size_t previousIndex(std::size_t index, std::size_t count, int repeatMode)
    {
        if (count == 0)
            return 0;
        if (index > 0)
            return index - 1;
        return repeatMode == 1 ? count - 1 : 0;
    }

    bool canAdvance(std::size_t index, std::size_t count, int repeatMode)
    {
        return count > 0 && (repeatMode != 0 || index + 1 < count);
    }
}
