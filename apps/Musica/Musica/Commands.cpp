#include "pch.h"
#include "Commands.h"
#include "Services.h"
#include "../Core/Match.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace Musica;
using namespace Platform;
using namespace Platform::Collections;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;

namespace
{
    // lower case, accents dropped, everything that is not a letter/digit becomes a space
    std::wstring Fold(String^ s)
    {
        std::wstring out;
        if (s == nullptr) return out;
        for (const wchar_t* p = s->Data(); *p; ++p)
        {
            wchar_t c = towlower(*p);
            const wchar_t* from = L"\xE0\xE1\xE2\xE4\xE8\xE9\xEA\xEB\xEC\xED\xEE\xEF\xF2\xF3\xF4\xF6\xF9\xFA\xFB\xFC\xE7";
            const wchar_t* to   = L"aaaaeeeeiiiioooouuuuc";
            const wchar_t* hit = wcschr(from, c);
            if (hit && c) c = to[hit - from];
            out += iswalnum(c) ? c : L' ';
        }
        return out;
    }

    std::vector<std::wstring> Words(const std::wstring& folded)
    {
        static const wchar_t* skip[] = { L"di", L"del", L"della", L"dei", L"degli", L"il", L"lo", L"la", L"i", L"gli",
                                         L"le", L"un", L"una", L"the", L"by", L"canzone", L"brano", L"album", L"musica" };
        std::vector<std::wstring> words;
        size_t i = 0;
        while (i < folded.size())
        {
            while (i < folded.size() && folded[i] == L' ') ++i;
            size_t j = i;
            while (j < folded.size() && folded[j] != L' ') ++j;
            if (j > i)
            {
                std::wstring w = folded.substr(i, j - i);
                bool common = false;
                for (size_t k = 0; k < sizeof(skip) / sizeof(skip[0]); ++k) if (w == skip[k]) common = true;
                if (!common) words.push_back(w);
            }
            i = j;
        }
        return words;
    }

    // share of the query words found in the text (0..1); a whole-text match gets 1.5
    double Score(const std::vector<std::wstring>& query, const std::wstring& queryFolded, String^ text)
    {
        if (query.empty()) return 0;
        std::wstring t = L" " + Fold(text) + L" ";
        std::wstring q = queryFolded;
        q.erase(0, q.find_first_not_of(L' '));
        q.erase(q.find_last_not_of(L' ') + 1);
        if (!q.empty() && t == L" " + q + L" ") return 1.5;
        int found = 0;
        for (size_t i = 0; i < query.size(); ++i)
            if (t.find(L" " + query[i] + L" ") != std::wstring::npos) ++found;
        return static_cast<double>(found) / query.size();
    }

    std::wstring Param(WwwFormUrlDecoder^ q, const wchar_t* name)
    {
        try { return q->GetFirstValueByName(ref new String(name))->Data(); }
        catch (Exception^) { return std::wstring(); }
    }

    bool PlayArtist(String^ artist, bool shuffle)
    {
        auto songs = MusicService::SongsForArtist(artist);
        if (songs == nullptr || songs->Size == 0) return false;
        MusicService::PlaySongs(songs, 0, shuffle);
        return true;
    }

    // one song, as a tap on it; asked at random: that song first, then the library shuffled
    bool PlayOne(IVector<Song^>^ all, unsigned int song, bool shuffle)
    {
        if (shuffle) MusicService::PlaySongs(all, song, true);
        else MusicService::PlaySong(all->GetAt(song));
        return true;
    }

    std::wstring Text(String^ s)
    {
        return s == nullptr ? std::wstring() : std::wstring(s->Data());
    }

    std::wstring Joined(const std::vector<std::wstring>& words)
    {
        std::wstring out;
        for (size_t i = 0; i < words.size(); ++i) out += words[i];
        return out;
    }

    // against the start of the name too: "comfortable" for Comfortably Numb, the recognizer gives the words it knows
    double SoundsLike(const std::wstring& request, const std::wstring& key)
    {
        double s = MusicaCore::soundSimilarity(request, key);
        if (request.size() >= 7 && key.size() > request.size())
            s = (std::max)(s, 0.95 * MusicaCore::soundSimilarity(request, key.substr(0, request.size() + 1)));
        // a short key resembles too many names ("a nome" and Money)
        return request.size() < 6 && s < 0.85 ? 0 : s;
    }

    // the request as heard, with and without the small words, against one name
    double Sounds(const std::wstring& asSaid, const std::wstring& asWords, const std::wstring& name)
    {
        const std::wstring key = MusicaCore::soundKey(name);
        return (std::max)(SoundsLike(asSaid, key), SoundsLike(asWords, key));
    }
}

bool Commands::Run(Uri^ uri)
{
    if (uri == nullptr) return false;
    std::wstring verb = uri->Path == nullptr ? L"" : uri->Path->Data();
    while (!verb.empty() && verb[0] == L'/') verb.erase(0, 1);
    std::wstring raw = uri->Query == nullptr ? L"" : uri->Query->Data();
    if (!raw.empty() && raw[0] == L'?') raw.erase(0, 1);
    WwwFormUrlDecoder^ query = nullptr;
    try { query = ref new WwwFormUrlDecoder(ref new String(raw.c_str())); }
    catch (Exception^) { return false; }

    if (verb == L"azione")
    {
        std::wstring a = Param(query, L"a");
        if (a == L"avanti") MusicService::Next();
        else if (a == L"indietro") MusicService::Previous();
        else if (a == L"pausa") { if (MusicService::IsPlaying()) MusicService::PlayOrPause(); }
        else if (a == L"riprendi") { if (!MusicService::IsPlaying()) MusicService::PlayOrPause(); }
        return false;
    }
    if (verb != L"riproduci") return false;

    std::wstring kind = Param(query, L"tipo");
    const bool shuffle = Param(query, L"casuale") == L"1";
    std::wstring folded = Fold(ref new String(Param(query, L"q").c_str()));
    std::vector<std::wstring> words = Words(folded);
    auto all = MusicService::GetSongs();
    if (all == nullptr || all->Size == 0) return false;

    if (kind == L"casuale" || words.empty())
    {
        MusicService::PlaySongs(all, static_cast<unsigned int>(GetTickCount64() % all->Size), true);
        return true;
    }

    // best artist, album, playlist and song; the kind asked for wins, otherwise the highest score
    double bestArtist = 0, bestAlbum = 0, bestList = 0, bestSong = 0;
    String^ artist = nullptr;
    AlbumItem^ album = nullptr;
    PlaylistInfo^ list = nullptr;
    unsigned int song = 0;
    auto artists = MusicService::GetArtists();
    for (unsigned int i = 0; i < artists->Size; ++i)
    {
        double s = Score(words, folded, artists->GetAt(i));
        if (s > bestArtist) { bestArtist = s; artist = artists->GetAt(i); }
    }
    auto albums = MusicService::GetAlbums();
    for (unsigned int i = 0; i < albums->Size; ++i)
    {
        double s = Score(words, folded, albums->GetAt(i)->Name);
        if (s > bestAlbum) { bestAlbum = s; album = albums->GetAt(i); }
    }
    auto lists = MusicService::GetPlaylists();
    for (unsigned int i = 0; i < lists->Size; ++i)
    {
        double s = Score(words, folded, lists->GetAt(i)->Name);
        if (s > bestList) { bestList = s; list = lists->GetAt(i); }
    }
    for (unsigned int i = 0; i < all->Size; ++i)
    {
        Song^ x = all->GetAt(i);
        // "albachiara vasco" matches title + artist together
        double s = (std::max)(Score(words, folded, x->Title), Score(words, folded, x->Title + L" " + x->Artist));
        if (s > bestSong) { bestSong = s; song = i; }
    }

    double top = (std::max)((std::max)(bestArtist, bestAlbum), (std::max)(bestList, bestSong));

    // no name has all the words: compare by sound (Core/Match.h). On the phone's 395 titles the right title gives
    // 0.76-1.0 for a spelling as heard by the Italian recognizer, the best wrong one at most 0.69.
    if (top < 1.0)
    {
        const double close = 0.74;
        const std::wstring asSaid = MusicaCore::soundKey(folded);
        const std::wstring asWords = MusicaCore::soundKey(Joined(words));
        for (unsigned int i = 0; i < artists->Size; ++i)
        {
            double s = Sounds(asSaid, asWords, Text(artists->GetAt(i)));
            if (s >= close && s > bestArtist) { bestArtist = s; artist = artists->GetAt(i); }
        }
        for (unsigned int i = 0; i < albums->Size; ++i)
        {
            double s = Sounds(asSaid, asWords, MusicaCore::withoutBrackets(Text(albums->GetAt(i)->Name)));
            if (s >= close && s > bestAlbum) { bestAlbum = s; album = albums->GetAt(i); }
        }
        for (unsigned int i = 0; i < lists->Size; ++i)
        {
            double s = Sounds(asSaid, asWords, Text(lists->GetAt(i)->Name));
            if (s >= close && s > bestList) { bestList = s; list = lists->GetAt(i); }
        }
        for (unsigned int i = 0; i < all->Size; ++i)
        {
            Song^ x = all->GetAt(i);
            const std::wstring title = MusicaCore::withoutBrackets(Text(x->Title));
            double s = (std::max)(Sounds(asSaid, asWords, title), Sounds(asSaid, asWords, title + L" " + Text(x->Artist)));
            if (s >= close && s > bestSong) { bestSong = s; song = i; }
        }
        top = (std::max)((std::max)(bestArtist, bestAlbum), (std::max)(bestList, bestSong));
    }

    const double enough = 0.5;
    if (kind == L"artista" && artist != nullptr && bestArtist >= enough) return PlayArtist(artist, shuffle);
    if (kind == L"album" && album != nullptr && bestAlbum >= enough) { MusicService::PlayAlbum(album, shuffle); return true; }
    if (kind == L"playlist" && list != nullptr && bestList >= enough) { MusicService::PlaySongs(list->Tracks, 0, shuffle); return true; }
    if (kind == L"brano" && bestSong >= enough) return PlayOne(all, song, shuffle);

    if (top < enough) return false;
    if (bestSong == top && bestSong > bestArtist) return PlayOne(all, song, shuffle);
    if (bestArtist == top && artist != nullptr) return PlayArtist(artist, shuffle);
    if (bestAlbum == top && album != nullptr) { MusicService::PlayAlbum(album, shuffle); return true; }
    if (bestList == top && list != nullptr) { MusicService::PlaySongs(list->Tracks, 0, shuffle); return true; }
    return PlayOne(all, song, shuffle);
}
