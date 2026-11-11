#include "pch.h"
#include "OnlineService.h"
#include "Mp4Tags.h"
#include "Core/Https.h"

#include <fstream>
#include <iterator>

using namespace Musica;
using namespace concurrency;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::Web::Http;

bool OnlineService::downloadedSomething = false;

std::wstring Musica::Utf16(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Musica::Utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

namespace
{
    HttpClient^ Client()
    {
        static HttpClient^ client;
        if (client == nullptr)
        {
            auto filter = ref new Windows::Web::Http::Filters::HttpBaseProtocolFilter();
            filter->CacheControl->ReadBehavior = Windows::Web::Http::Filters::HttpCacheReadBehavior::MostRecent;
            client = ref new HttpClient(filter);
        }
        return client;
    }

    String^ P(const std::string& s) { return ref new String(Utf16(s).c_str()); }

    std::string visitor;   // VISIONOS visitor id, asked once per run
}

task<std::string> OnlineService::Fetch(const online::Request& r)
{
    auto msg = ref new HttpRequestMessage(r.body.empty() ? HttpMethod::Get : HttpMethod::Post, ref new Uri(P(r.url)));
    for (size_t i = 0; i < r.headers.size(); ++i)
    {
        if (r.headers[i].first == "Content-Type") continue;
        msg->Headers->TryAppendWithoutValidation(P(r.headers[i].first), P(r.headers[i].second));
    }
    if (!r.body.empty())
        msg->Content = ref new HttpStringContent(P(r.body), UnicodeEncoding::Utf8, L"application/json");
    return create_task(Client()->SendRequestAsync(msg)).then([](HttpResponseMessage^ resp)
    {
        if (!resp->IsSuccessStatusCode)
            throw ref new FailureException(L"HTTP " + static_cast<int>(resp->StatusCode).ToString());
        return create_task(resp->Content->ReadAsBufferAsync());
    }).then([](IBuffer^ b)
    {
        std::string s(b->Length, '\0');
        if (b->Length)
        {
            auto reader = DataReader::FromBuffer(b);
            reader->ReadBytes(ArrayReference<unsigned char>(reinterpret_cast<unsigned char*>(&s[0]), b->Length));
        }
        return s;
    });
}

task<std::vector<online::Song>> OnlineService::Search(const std::string& query)
{
    return Fetch(online::searchRequest(query)).then([](std::string json)
    {
        std::vector<online::Song> out;
        if (!online::parseSearch(json, out)) throw ref new FailureException(L"risposta di YouTube Music non valida");
        return out;
    });
}

task<online::SpotifyList> OnlineService::Spotify(const std::string& link)
{
    online::Request r;
    if (!online::spotifyEmbedRequest(link, r))
    {
        online::SpotifyList l;
        l.error = "non \xc3\xa8 un link di Spotify (playlist, album o brano)";
        return task_from_result(l);
    }
    // WP8.1's TLS cannot talk to open.spotify.com (ECDHE-RSA + AES-GCM only): mbedTLS on a worker thread
    return create_task([r]()
    {
        std::string err;
        std::string html = online::httpsGet(r.url, r.headers, err);
        online::SpotifyList l = online::parseSpotifyEmbed(html);
        if (html.empty()) l.error = "Spotify non raggiungibile: " + err;
        return l;
    });
}

namespace
{
    // The audio in 1 MB ranges, appended to path; progress in percent.
    task<void> DownloadRanges(online::Audio a, std::wstring path, long long from, std::function<void(int)> progress)
    {
        long long to = from + 1048575;
        if (a.length > 0 && to >= a.length) to = a.length - 1;
        return OnlineService::Fetch(online::audioRangeRequest(a, from, to)).then([a, path, from, to, progress](std::string bytes)
        {
            if (bytes.empty()) throw ref new FailureException(L"download interrotto");
            std::ofstream f(path, from == 0 ? std::ios::binary | std::ios::trunc : std::ios::binary | std::ios::app);
            f.write(bytes.data(), bytes.size());
            if (!f) throw ref new FailureException(L"spazio esaurito?");
            f.close();
            long long next = from + static_cast<long long>(bytes.size());
            if (a.length > 0 && progress) progress(static_cast<int>(next * 100 / a.length));
            bool more = a.length > 0 ? next < a.length : bytes.size() == static_cast<size_t>(to - from + 1);
            if (!more) return task_from_result();
            return DownloadRanges(a, path, next, progress);
        });
    }

    // cover of YouTube Music: a 120 px link by default, the same image at 300 px (the phone's covers are small)
    std::string BigCover(std::string url)
    {
        size_t p = url.find("=w120-h120");
        if (p != std::string::npos) url.replace(p, 10, "=w300-h300");
        return url;
    }

    task<StorageFolder^> TargetFolder(const AlbumTarget& album)
    {
        auto music = KnownFolders::MusicLibrary;
        if (album.name.empty())
            return create_task(music->CreateFolderAsync(L"Online", CreationCollisionOption::OpenIfExists));
        String^ name = P(online::safeFileName(album.artist + " - " + album.name));
        return create_task(music->CreateFolderAsync(L"Album", CreationCollisionOption::OpenIfExists))
            .then([name](StorageFolder^ all) { return all->CreateFolderAsync(name, CreationCollisionOption::OpenIfExists); });
    }

    bool IsJpeg(const std::string& bytes) { return bytes.size() > 3 && static_cast<unsigned char>(bytes[0]) == 0xFF; }
}

task<Song^> OnlineService::Get(const online::Song& s, std::function<void(int)> progress, const AlbumTarget& target)
{
    std::string fileName = s.artist.empty() ? s.title : s.artist + " - " + s.title;
    if (!target.name.empty())
    {
        char number[16];
        sprintf_s(number, "%02d - ", target.number);
        fileName = number + s.title;
    }
    std::wstring name = Utf16(online::safeFileName(fileName)) + L".m4a";
    AlbumTarget album = target;
    auto song = ref new Song();
    song->Title = P(s.title);
    song->Artist = P(s.artist);
    song->AlbumArtist = P(album.name.empty() ? s.artist : album.artist);
    song->Album = P(album.name.empty() ? s.album : album.name);
    song->DurationSeconds = s.seconds;
    auto ui = Windows::ApplicationModel::Core::CoreApplication::MainView->CoreWindow->Dispatcher;
    auto report = [ui, progress](int pct)
    {
        if (!progress) return;
        ui->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, ref new Windows::UI::Core::DispatchedHandler([progress, pct]() { progress(pct); }));
    };
    online::Song item = s;
    std::shared_ptr<StorageFolder^> folder = std::make_shared<StorageFolder^>(nullptr);
    return TargetFolder(album).then([folder, name](StorageFolder^ f)
    {
        *folder = f;
        // already downloaded: just the file
        return create_task(f->GetFileAsync(ref new String(name.c_str()))).then([](task<StorageFile^> t) -> StorageFile^
        {
            try { return t.get(); } catch (Exception^) { return nullptr; }
        });
    }).then([folder, name, item, report, song, album](StorageFile^ existing) -> task<Song^>
    {
        if (existing != nullptr)
        {
            song->Path = existing->Path;
            return task_from_result(song);
        }
        std::wstring local = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\online-" + Utf16(item.videoId);
        std::wstring raw = local + L".raw.m4a", tagged = local + L".m4a";
        std::shared_ptr<std::string> cover = std::make_shared<std::string>();
        task<std::string> visitorTask = visitor.empty()
            ? OnlineService::Fetch(online::visitorRequest()).then([](std::string json) { visitor = online::parseVisitor(json); return visitor; })
            : task_from_result(visitor);
        return visitorTask.then([item](std::string v)
        {
            return OnlineService::Fetch(online::playerRequest(item.videoId, v));
        }).then([raw, report](std::string json)
        {
            online::Audio a = online::parsePlayer(json);
            if (a.url.empty()) throw ref new FailureException(P(a.error));
            report(0);
            return DownloadRanges(a, raw, 0, report);
        }).then([album]()
        {
            // the album's own cover, the same on all its songs (mbedTLS: the same servers as open.spotify.com)
            std::string url = album.cover;
            return create_task([url]()
            {
                std::string err;
                return url.empty() ? std::string() : online::httpsGet(url, std::vector<std::pair<std::string, std::string> >(), err);
            });
        }).then([item, cover](std::string albumCover)
        {
            if (IsJpeg(albumCover)) { *cover = albumCover; return task_from_result(); }
            if (item.thumb.empty()) return task_from_result();
            online::Request r;
            r.url = BigCover(item.thumb);
            return OnlineService::Fetch(r).then([cover](task<std::string> t)
            {
                try { *cover = t.get(); } catch (Exception^) {}
            });
        }).then([raw, tagged, item, cover, album]()
        {
            {
                // a plain M4A instead of YouTube's fragmented one (see online::defragment)
                std::ifstream in(raw, std::ios::binary);
                std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                in.close();
                std::string err;
                if (online::defragment(data, err) || online::removeEditLists(data) > 0)
                {
                    std::ofstream out(raw, std::ios::binary | std::ios::trunc);
                    out.write(data.data(), data.size());
                }
            }
            perle::Mp4Tags tags;
            tags.title = item.title;
            tags.artist = item.artist;
            tags.album = !album.name.empty() ? album.name : item.album.empty() ? item.title : item.album;
            tags.albumArtist = album.artist;
            tags.track = static_cast<unsigned int>(album.number);
            tags.trackTotal = static_cast<unsigned int>(album.total);
            if (IsJpeg(*cover)) tags.cover.assign(cover->begin(), cover->end());
            std::string err = perle::WriteMp4Tags(raw, tagged, tags);
            _wremove(raw.c_str());
            if (!err.empty()) throw ref new FailureException(P("tag: " + err));
            return create_task(StorageFile::GetFileFromPathAsync(ref new String(tagged.c_str())));
        }).then([folder, name](StorageFile^ file)
        {
            return create_task(file->CopyAsync(*folder, ref new String(name.c_str()), NameCollisionOption::ReplaceExisting))
                .then([file](StorageFile^ copy) { file->DeleteAsync(); return copy; });
        }).then([song](StorageFile^ copy)
        {
            OnlineService::downloadedSomething = true;
            song->Path = copy->Path;
            return song;
        });
    });
}
