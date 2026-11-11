#include "StreamServer.h"
#include "WinRtText.h"
#include "core/Srt.h"
#include "core/Util.h"
#include "../Perle81.Native/Media/TsDemux.h"
#include "core/TsMuxer.h"
#include "CastLog.h"
#include "Timeout.h"

#include <collection.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <ppltasks.h>

#include <windows.foundation.h>
#include <windows.networking.h>
#include <windows.networking.connectivity.h>
#include <windows.networking.sockets.h>
#include <windows.storage.streams.h>
#include <windows.web.http.h>

using namespace Windows::Foundation;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage::Streams;
using namespace Windows::Web::Http;

namespace PerleCast {

namespace {

std::string TrimString(const std::string& value)
{
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string GetAttribute(const std::string& line, const std::string& key)
{
    std::string token = key + "=";
    size_t p = line.find(token);
    if (p == std::string::npos) return std::string();
    p += token.size();
    if (p < line.size() && line[p] == '"')
    {
        size_t end = line.find('"', p + 1);
        if (end == std::string::npos) return std::string();
        return line.substr(p + 1, end - p - 1);
    }
    size_t end = line.find(',', p);
    return line.substr(p, end == std::string::npos ? end : end - p);
}

std::string Resolve(const std::string& base, const std::string& path)
{
    return Util::ResolveUrl(base, path);
}

std::vector<std::string> Lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line))
    {
        line = TrimString(line);
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

// One client for every request: its connections to googlevideo are reused (a new client per segment paid a new
// TCP + TLS handshake each time, about a second on this phone).
HttpClient^ g_client;
std::once_flag g_clientOnce;

HttpClient^ Client()
{
    std::call_once(g_clientOnce, []()
    {
        g_client = ref new HttpClient();
        g_client->DefaultRequestHeaders->Append(FromUtf8("User-Agent"),
            FromUtf8("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
    });
    return g_client;
}

std::vector<unsigned char> Download(const std::string& url, int seconds = 30)
{
    HttpResponseMessage^ response = WaitAtMost(Client()->GetAsync(ref new Uri(FromUtf8(url))), seconds);
    if (static_cast<int>(response->StatusCode) < 200 ||
        static_cast<int>(response->StatusCode) >= 300)
        throw ref new Platform::FailureException(FromUtf8("HTTP " +
            std::to_string(static_cast<int>(response->StatusCode))));

    IBuffer^ buffer = WaitAtMost(response->Content->ReadAsBufferAsync(), seconds);
    // read straight into the vector: a 1080p segment is several MB
    std::vector<unsigned char> result(buffer->Length);
    if (!result.empty())
        DataReader::FromBuffer(buffer)->ReadBytes(
            Platform::ArrayReference<unsigned char>(result.data(), static_cast<unsigned int>(result.size())));
    return result;
}

std::string DownloadText(const std::string& url)
{
    std::vector<unsigned char> bytes = Download(url);
    return std::string(bytes.begin(), bytes.end());
}

struct Variant
{
    std::string url;
    std::string group;
    std::string videoCodec;
    std::string audioCodec;
    int height;
    long bandwidth;
    Variant() : height(0), bandwidth(0) {}
};

std::string CodecFamily(const std::string& value)
{
    size_t dot = value.find('.');
    return value.substr(0, dot);
}

void ParseMaster(const std::string& master, const std::string& base,
                 std::vector<Variant>& variants,
                 std::vector<std::string>& audioLines)
{
    std::vector<std::string> lines = Lines(master);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].find("#EXT-X-MEDIA:") == 0)
            audioLines.push_back(lines[i]);

        if (lines[i].find("#EXT-X-STREAM-INF:") != 0)
            continue;

        std::string attributes = lines[i].substr(18);
        std::string codecs = GetAttribute(attributes, "CODECS");
        Variant variant;
        variant.group = GetAttribute(attributes, "AUDIO");
        variant.height = 0;
        variant.bandwidth = 0;

        size_t codecAt = codecs.find("avc1");
        if (codecAt == std::string::npos) continue;
        size_t codecEnd = codecs.find(',', codecAt);
        variant.videoCodec = TrimString(codecs.substr(codecAt,
            codecEnd == std::string::npos ? codecEnd : codecEnd - codecAt));
        codecAt = codecs.find("mp4a");
        if (codecAt != std::string::npos)
        {
            codecEnd = codecs.find(',', codecAt);
            variant.audioCodec = TrimString(codecs.substr(codecAt,
                codecEnd == std::string::npos ? codecEnd : codecEnd - codecAt));
        }

        std::string resolution = GetAttribute(attributes, "RESOLUTION");
        size_t x = resolution.find('x');
        if (x != std::string::npos)
            variant.height = std::atoi(resolution.substr(x + 1).c_str());
        variant.bandwidth = std::atol(GetAttribute(attributes, "BANDWIDTH").c_str());

        for (size_t j = i + 1; j < lines.size(); ++j)
        {
            if (lines[j].empty() || lines[j][0] == '#') continue;
            variant.url = Resolve(base, lines[j]);
            break;
        }
        if (variant.height > 0 && !variant.url.empty())
            variants.push_back(variant);
    }
}

std::string PrimaryLanguage(const std::string& language)
{
    std::string primary = language.substr(0, language.find('-'));
    std::transform(primary.begin(), primary.end(), primary.begin(), ::tolower);
    return primary;
}

// The audio renditions that go with the chosen variant, like HlsSource.AudioOptions: YouTube can publish one
// otherwise identical video variant for each dubbed audio group.
std::vector<StreamAudioOption> AudioOptionsFor(const std::vector<std::string>& audioLines,
                                               const std::vector<Variant>& variants,
                                               const Variant& chosen,
                                               const std::string& masterUrl)
{
    std::vector<std::string> groups;
    if (!chosen.group.empty()) groups.push_back(chosen.group);
    for (size_t i = 0; i < variants.size(); ++i)
    {
        const Variant& variant = variants[i];
        if (variant.group.empty() || variant.audioCodec.empty()) continue;
        if (!chosen.audioCodec.empty() && CodecFamily(variant.audioCodec) != CodecFamily(chosen.audioCodec)) continue;
        if (CodecFamily(variant.videoCodec) != CodecFamily(chosen.videoCodec)) continue;
        if (std::find(groups.begin(), groups.end(), variant.group) == groups.end())
            groups.push_back(variant.group);
    }

    std::vector<StreamAudioOption> result;
    for (size_t i = 0; i < audioLines.size(); ++i)
    {
        std::string line = audioLines[i].substr(13);
        if (GetAttribute(line, "TYPE") != "AUDIO") continue;
        std::string group = GetAttribute(line, "GROUP-ID");
        std::string uri = GetAttribute(line, "URI");
        if (uri.empty() || std::find(groups.begin(), groups.end(), group) == groups.end()) continue;

        StreamAudioOption option;
        option.url = Resolve(masterUrl, uri);
        bool seen = false;
        for (size_t j = 0; j < result.size(); ++j) seen = seen || result[j].url == option.url;
        if (seen) continue;
        option.name = GetAttribute(line, "NAME");
        option.language = GetAttribute(line, "LANGUAGE");
        option.isDefault = GetAttribute(line, "DEFAULT") == "YES";
        result.push_back(option);
    }
    return result;
}

size_t PickAudio(const std::vector<StreamAudioOption>& options, const std::string& preferred)
{
    if (!preferred.empty())
    {
        for (size_t i = 0; i < options.size(); ++i)
            if (PrimaryLanguage(options[i].language) == PrimaryLanguage(preferred) &&
                options[i].language.size() == preferred.size() &&
                std::equal(preferred.begin(), preferred.end(), options[i].language.begin(),
                    [](char x, char y) { return ::tolower(x) == ::tolower(y); }))
                return i;
        for (size_t i = 0; i < options.size(); ++i)
            if (!options[i].language.empty() && PrimaryLanguage(options[i].language) == PrimaryLanguage(preferred))
                return i;
    }
    for (size_t i = 0; i < options.size(); ++i)
        if (options[i].isDefault) return i;
    return 0;
}

int AudioRateIndex(int rate)
{
    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000, 7350};
    for (int i = 0; i < 13; ++i) if (rates[i] == rate) return i;
    return 4;
}

std::vector<unsigned char> MakeAdts(const std::vector<unsigned char>& data,
                                    int sampleRate, int channels)
{
    int length = static_cast<int>(data.size()) + 7;
    int frequency = AudioRateIndex(sampleRate);
    std::vector<unsigned char> result(7 + data.size());
    result[0] = 0xFF;
    result[1] = 0xF1;
    result[2] = static_cast<unsigned char>((1 << 6) | (frequency << 2) |
                                           ((channels >> 2) & 1));
    result[3] = static_cast<unsigned char>(((channels & 3) << 6) |
                                           ((length >> 11) & 3));
    result[4] = static_cast<unsigned char>((length >> 3) & 0xFF);
    result[5] = static_cast<unsigned char>(((length & 7) << 5) | 0x1F);
    result[6] = 0xFC;
    std::copy(data.begin(), data.end(), result.begin() + 7);
    return result;
}

std::string FindHostAddress()
{
    auto names = NetworkInformation::GetHostNames();
    for (unsigned int i = 0; i < names->Size; ++i)
    {
        HostName^ host = names->GetAt(i);
        if (host->Type != HostNameType::Ipv4) continue;
        std::string address = ToUtf8(host->CanonicalName);
        if (address != "127.0.0.1" && address.find("169.254.") != 0)
            return address;
    }
    return std::string();
}

} // namespace

StreamServer::StreamServer() : totalTicks_(0), audioIndex_(0), maxHeight_(1080), height_(0), cacheLimit_(8), generation_(0), streams_(0), closed_(true)
{
}

Platform::String^ StreamServer::Start(Platform::String^ masterUrl,
                                      Platform::String^ audioLanguage,
                                      Platform::String^ subtitleVttUrl)
{
    Close();
    masterUrl_ = ToUtf8(masterUrl);
    audioLanguage_ = ToUtf8(audioLanguage);
    subtitleVttUrl_ = ToUtf8(subtitleVttUrl);

    std::string address = FindHostAddress();
    if (address.empty())
        throw ref new Platform::FailureException(
            ref new Platform::String(L"Indirizzo Wi-Fi non disponibile"));

    Prepare();

    listener_ = ref new StreamSocketListener();
    listener_->ConnectionReceived += ref new TypedEventHandler<
        StreamSocketListener^, StreamSocketListenerConnectionReceivedEventArgs^>(
        this, &StreamServer::OnConnectionReceived);

    unsigned int port = 20000 + static_cast<unsigned int>(rand() % 30000);
    concurrency::create_task(listener_->BindServiceNameAsync(
        FromUtf8(std::to_string(port)))).get();
    localUrl_ = "http://" + address + ":" + std::to_string(port);
    closed_ = false;
    return FromUtf8(localUrl_);
}

void StreamServer::Close()
{
    closed_ = true;
    {
        std::lock_guard<std::mutex> lock(socketsLock_);
        if (!sockets_.empty()) CastLogLine("closing " + std::to_string(sockets_.size()) + " tv connection(s)");
        for (size_t i = 0; i < sockets_.size(); ++i)
        {
            try { delete sockets_[i]; } catch (...) { }
        }
        sockets_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(cacheLock_);
        cache_.clear();
    }
    if (listener_ != nullptr)
    {
        try { delete listener_; }
        catch (Platform::Exception^) { }
        listener_ = nullptr;
    }
}

namespace {

std::vector<std::pair<std::string, int64_t> > MediaSegments(const std::string& playlistUrl)
{
    std::vector<std::pair<std::string, int64_t> > result;
    std::vector<std::string> lines = Lines(DownloadText(playlistUrl));
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].find("#EXTINF:") != 0) continue;
        std::string value = lines[i].substr(8);
        size_t comma = value.find(',');
        if (comma != std::string::npos) value = value.substr(0, comma);
        int64_t ticks = static_cast<int64_t>(std::atof(value.c_str()) * 10000000.0);
        for (size_t j = i + 1; j < lines.size(); ++j)
        {
            if (!lines[j].empty() && lines[j][0] != '#')
            {
                result.push_back(std::make_pair(Resolve(playlistUrl, lines[j]), ticks));
                break;
            }
        }
    }
    return result;
}

std::vector<unsigned char> DownloadRetry(const std::string& url)
{
    for (int attempt = 0; ; ++attempt)
    {
        // a stuck request blocked the TV for the whole 30 s: short first tries, a long last one
        try { return Download(url, attempt == 0 ? 8 : attempt == 1 ? 15 : 30); }
        catch (Platform::Exception^ ex)
        {
            CastLogLine("download attempt " + std::to_string(attempt + 1) + ": " + ToUtf8(ex->Message));
            if (attempt >= 2) throw;
        }
        concurrency::wait(1000 * (attempt + 1));
    }
}

void WriteAll(DataWriter^ writer, std::vector<unsigned char>& bytes)
{
    if (bytes.empty()) return;
    Platform::Array<unsigned char>^ array =
        ref new Platform::Array<unsigned char>(bytes.data(), static_cast<unsigned int>(bytes.size()));
    writer->WriteBytes(array);
    concurrency::create_task(writer->StoreAsync()).get();
    bytes.clear();
}

} // namespace

void StreamServer::Prepare()
{
    std::string master = DownloadText(masterUrl_);
    std::vector<Variant> variants;
    std::vector<std::string> audioLines;
    ParseMaster(master, masterUrl_, variants, audioLines);
    if (variants.empty())
        throw ref new Platform::FailureException(
            ref new Platform::String(L"Nessuna variante H.264 fino a 720p"));

    // the TV decodes, the phone only remuxes: any H.264 resolution up to 1080p can be sent
    heights_.clear();
    for (size_t i = 0; i < variants.size(); ++i)
        if (variants[i].height <= 1080 &&
            std::find(heights_.begin(), heights_.end(), variants[i].height) == heights_.end())
            heights_.push_back(variants[i].height);
    std::sort(heights_.begin(), heights_.end(), [](int a, int b) { return a > b; });

    std::vector<Variant> playable;
    for (size_t i = 0; i < variants.size(); ++i)
        if (variants[i].height <= maxHeight_) playable.push_back(variants[i]);
    if (playable.empty())
        for (size_t i = 0; i < variants.size(); ++i)
            if (!heights_.empty() && variants[i].height == heights_.back()) playable.push_back(variants[i]);
    if (playable.empty())
        throw ref new Platform::FailureException(
            ref new Platform::String(L"Nessuna variante H.264 fino a 1080p"));
    std::sort(playable.begin(), playable.end(),
        [](const Variant& a, const Variant& b)
        {
            if (a.height != b.height) return a.height > b.height;
            return a.bandwidth < b.bandwidth;
        });
    const Variant& variant = playable[0];
    height_ = variant.height;
    audioOptions_ = AudioOptionsFor(audioLines, variants, variant, masterUrl_);
    audioIndex_ = PickAudio(audioOptions_, audioLanguage_);
    std::string audioUrl = audioOptions_.empty() ? std::string() : audioOptions_[audioIndex_].url;

    video_.clear();
    audio_.clear();
    totalTicks_ = 0;
    std::vector<std::pair<std::string, int64_t> > list = MediaSegments(variant.url);
    for (size_t i = 0; i < list.size(); ++i)
    {
        Segment segment = { list[i].first, list[i].second, totalTicks_ };
        video_.push_back(segment);
        totalTicks_ += list[i].second;
    }
    if (video_.empty())
        throw ref new Platform::FailureException(
            ref new Platform::String(L"Playlist video vuota"));

    if (!audioUrl.empty())
    {
        int64_t start = 0;
        list = MediaSegments(audioUrl);
        for (size_t i = 0; i < list.size(); ++i)
        {
            Segment segment = { list[i].first, list[i].second, start };
            audio_.push_back(segment);
            start += list[i].second;
        }
    }
}

void StreamServer::OnConnectionReceived(StreamSocketListener^,
    StreamSocketListenerConnectionReceivedEventArgs^ args)
{
    StreamSocket^ socket = args->Socket;
    {
        std::lock_guard<std::mutex> lock(socketsLock_);
        if (closed_) { try { delete socket; } catch (...) { } return; }
        sockets_.push_back(socket);
    }
    StreamServer^ self = this;
    concurrency::create_task([self, socket]()
    {
        try { self->Serve(socket); }
        catch (Platform::Exception^) { }
        catch (...) { }
        {
            std::lock_guard<std::mutex> lock(self->socketsLock_);
            auto at = std::find(self->sockets_.begin(), self->sockets_.end(), socket);
            if (at != self->sockets_.end()) self->sockets_.erase(at);
        }
        try { delete socket; } catch (...) { }
    });
}

void StreamServer::Serve(StreamSocket^ socket)
{
    DataReader^ reader = ref new DataReader(socket->InputStream);
    reader->InputStreamOptions = InputStreamOptions::Partial;
    std::string request;
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192)
    {
        unsigned int loaded =
            concurrency::create_task(reader->LoadAsync(2048)).get();
        if (loaded == 0) break;
        Platform::Array<unsigned char>^ chunk = ref new Platform::Array<unsigned char>(loaded);
        reader->ReadBytes(chunk);
        request.append(reinterpret_cast<const char*>(chunk->Data), loaded);
    }
    reader->DetachStream();

    size_t lineEnd = request.find("\r\n");
    std::string firstLine = request.substr(0, lineEnd);
    size_t pathStart = firstLine.find(' ');
    size_t pathEnd = pathStart == std::string::npos ? pathStart :
        firstLine.find(' ', pathStart + 1);
    std::string method = pathStart == std::string::npos ? "GET" : firstLine.substr(0, pathStart);
    std::string path = pathStart == std::string::npos ? "/" :
        firstLine.substr(pathStart + 1, pathEnd - pathStart - 1);
    bool headOnly = method == "HEAD";
    {
        std::string range;
        size_t at = request.find("\nRange:");
        if (at == std::string::npos) at = request.find("\nrange:");
        if (at != std::string::npos) range = " " + TrimString(request.substr(at + 1, request.find('\r', at) - at - 1));
        CastLogLine("tv asks " + method + " " + path + range);
    }

    DataWriter^ writer = ref new DataWriter(socket->OutputStream);
    std::ostringstream header;

    if (path.find("/v.ts") == 0)
    {
        // Live stream: no length, no byte ranges; the TV seeks by asking for a new start position.
        header << "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t"
               << "\r\ntransferMode.dlna.org: Streaming"
               << "\r\ncontentFeatures.dlna.org: DLNA.ORG_OP=00;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000"
               << "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
        writer->WriteString(FromUtf8(header.str()));
        concurrency::create_task(writer->StoreAsync()).get();
        if (!headOnly)
        {
            double start = 0;
            size_t query = path.find("start=");
            if (query != std::string::npos)
                start = std::atof(path.substr(query + 6).c_str());
            // every request runs until the TV closes it: the TV opens two at once and keeps either one (ending
            // the older left it with nothing, "Transition not available"); the segment cache makes the copy free
            long generation = InterlockedIncrement(&generation_);
            InterlockedIncrement(&streams_);
            try { StreamTransport(writer, start, generation); }
            catch (Platform::Exception^ ex)
            {
                CastLogLine("stream from " + std::to_string(static_cast<int>(start)) + "s broken: " + ToUtf8(ex->Message));
                InterlockedDecrement(&streams_);
                throw;
            }
            catch (...) { CastLogLine("stream broken"); InterlockedDecrement(&streams_); throw; }
            InterlockedDecrement(&streams_);
        }
    }
    else
    {
        std::string contentType = "application/x-srt";
        std::vector<unsigned char> body;
        int code = 200;
        try
        {
            if (path.find("/s.srt") == 0)
            {
                double start = 0;
                size_t query = path.find("start=");
                if (query != std::string::npos)
                    start = std::atof(path.substr(query + 6).c_str());
                std::string subtitle = BuildSubtitle(start);
                body.assign(subtitle.begin(), subtitle.end());
            }
            else
            {
                code = 404;
                contentType = "text/plain; charset=utf-8";
                const char* text = "Not found";
                body.assign(text, text + 9);
            }
        }
        catch (Platform::Exception^)
        {
            code = 502;
            contentType = "text/plain; charset=utf-8";
            const char* text = "Subtitle unavailable";
            body.assign(text, text + 20);
        }
        header << "HTTP/1.1 " << code << (code == 200 ? " OK" : (code == 404 ? " Not Found" : " Bad Gateway"))
               << "\r\nContent-Type: " << contentType
               << "\r\nContent-Length: " << body.size()
               << "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n";
        writer->WriteString(FromUtf8(header.str()));
        if (headOnly) body.clear();
        WriteAll(writer, body);
        concurrency::create_task(writer->StoreAsync()).get();
    }

    concurrency::create_task(writer->FlushAsync()).get();
    writer->DetachStream();
}

std::shared_future<StreamServer::SegmentData> StreamServer::FetchSegment(size_t index, size_t audioFirst, size_t audioEnd)
{
    SegmentKey key(index, audioFirst, audioEnd);
    std::lock_guard<std::mutex> lock(cacheLock_);
    auto found = cache_.find(key);
    if (found != cache_.end()) return found->second;

    // keep the recent ones: drop finished segments well behind this one (a seek back downloads again)
    for (auto it = cache_.begin(); it != cache_.end() && cache_.size() > cacheLimit_;)
    {
        bool ready = it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        if (ready && std::get<0>(it->first) + 2 < index) it = cache_.erase(it);
        else ++it;
    }

    std::string videoUrl = video_[index].url;
    std::vector<std::string> audioUrls;
    for (size_t a = audioFirst; a < audioEnd && a < audio_.size(); ++a) audioUrls.push_back(audio_[a].url);
    auto promise = std::make_shared<std::promise<SegmentData> >();
    std::shared_future<SegmentData> future = promise->get_future().share();
    cache_[key] = future;
    StreamServer^ self = this;
    concurrency::create_task([self, key, promise, videoUrl, audioUrls, index]()
    {
        try
        {
            SegmentData data;
            data.video = DownloadRetry(videoUrl);
            for (size_t a = 0; a < audioUrls.size(); ++a) data.audio.push_back(DownloadRetry(audioUrls[a]));
            promise->set_value(std::move(data));
        }
        catch (Platform::Exception^ ex)
        {
            CastLogLine("segment " + std::to_string(index + 1) + " download failed: " + ToUtf8(ex->Message));
            {
                // not kept: the next connection tries again
                std::lock_guard<std::mutex> lock(self->cacheLock_);
                self->cache_.erase(key);
            }
            promise->set_exception(std::make_exception_ptr(ex));
        }
    });
    return future;
}

// The TV's clock starts at 0 with the first segment of /v.ts?start=: the cues move back by that segment's start.
std::string StreamServer::BuildSubtitle(double startSeconds)
{
    if (subtitleVttUrl_.empty()) return std::string();
    std::string vtt = DownloadText(subtitleVttUrl_);
    std::string srt = Srt::FromVtt(vtt);
    int64_t startTicks = static_cast<int64_t>((std::max)(0.0, startSeconds) * 10000000.0);
    size_t startIndex = 0;
    while (startIndex + 1 < video_.size() && video_[startIndex + 1].start <= startTicks)
        ++startIndex;
    int64_t offsetTicks = video_.empty() ? 0 : video_[startIndex].start;
    return offsetTicks > 0 ? Srt::Shift(srt, offsetTicks / 10000) : srt;
}

void StreamServer::StreamTransport(DataWriter^ writer, double startSeconds, long generation)
{
    int64_t startTicks = static_cast<int64_t>((std::max)(0.0, startSeconds) * 10000000.0);
    size_t startIndex = 0;
    while (startIndex + 1 < video_.size() && video_[startIndex + 1].start <= startTicks)
        ++startIndex;

    // audio segment ranges for each video segment, by playlist time
    std::vector<std::pair<size_t, size_t> > audioRanges(video_.size());
    size_t nextAudio = 0;
    while (nextAudio + 1 < audio_.size() && audio_[nextAudio + 1].start <= video_[startIndex].start)
        ++nextAudio;
    for (size_t i = startIndex; i < video_.size(); ++i)
    {
        int64_t end = i + 1 < video_.size() ? video_[i + 1].start : (std::numeric_limits<int64_t>::max)();
        size_t first = nextAudio;
        while (nextAudio < audio_.size() && audio_[nextAudio].start < end) ++nextAudio;
        audioRanges[i] = std::make_pair(first, nextAudio);
    }

    auto fetch = [this, audioRanges](size_t index)
    {
        return FetchSegment(index, audioRanges[index].first, audioRanges[index].second);
    };

    std::vector<unsigned char> output;
    std::unique_ptr<TsMuxer> muxer;
    int64_t origin = 0;
    bool separateAudio = !audio_.empty();
    bool anyAudio = false;

    std::deque<std::shared_future<SegmentData> > pending;
    size_t nextToSchedule = startIndex;
    for (int ahead = 0; ahead < 3 && nextToSchedule < video_.size(); ++ahead, ++nextToSchedule)
        pending.push_back(fetch(nextToSchedule));

    for (size_t i = startIndex; i < video_.size() && !closed_; ++i)
    {
        if (pending.empty()) break;
        ULONGLONG fetchStart = GetTickCount64();
        std::shared_future<SegmentData> current = pending.front();
        try { current.wait(); current.get(); }
        catch (Platform::Exception^ ex)
        {
            CastLogLine("segment " + std::to_string(i + 1) + " failed: " + ToUtf8(ex->Message));
            throw;
        }
        ULONGLONG waited = GetTickCount64() - fetchStart;
        pending.pop_front();
        if (nextToSchedule < video_.size())
        {
            pending.push_back(fetch(nextToSchedule));
            ++nextToSchedule;
        }
        const SegmentData& data = current.get();

        perle::TsSegmentData parsed;
        if (!perle::ParseTsSegment(data.video.data(), data.video.size(), parsed)) continue;
        if (!muxer)
        {
            // the first frame we send starts the TV's clock at 0
            origin = parsed.baseRaw + (parsed.video.empty() ? 0 : parsed.video[0].dts90k);
            muxer.reset(new TsMuxer([&output](const std::array<uint8_t, TsWriter::PacketSize>& packet)
            {
                output.insert(output.end(), packet.begin(), packet.end());
            }, origin));
        }

        for (size_t v = 0; v < parsed.video.size(); ++v)
        {
            const perle::VideoAccessUnit& sample = parsed.video[v];
            muxer->PushVideo(sample.annexB.data(), sample.annexB.size(),
                parsed.baseRaw + sample.pts90k, parsed.baseRaw + sample.dts90k, sample.key);
        }

        std::vector<perle::TsSegmentData> audioParts;
        if (separateAudio)
        {
            for (size_t a = 0; a < data.audio.size(); ++a)
            {
                perle::TsSegmentData part;
                if (perle::ParseAacSegment(data.audio[a].data(), data.audio[a].size(), part))
                    audioParts.push_back(part);
            }
        }
        else if (parsed.hasAudio)
        {
            audioParts.push_back(parsed);
        }

        for (size_t p = 0; p < audioParts.size(); ++p)
        {
            const perle::TsSegmentData& part = audioParts[p];
            for (size_t a = 0; a < part.audio.size(); ++a)
            {
                int64_t pts = part.baseRaw + part.audio[a].pts90k;
                if (pts < origin) continue;
                std::vector<unsigned char> adts = MakeAdts(part.audio[a].aac,
                    part.audioSampleRate, part.audioChannels);
                muxer->PushAudio(adts.data(), adts.size(), pts);
                anyAudio = true;
            }
        }

        // without audio the muxer would hold every video frame waiting to interleave
        if (!anyAudio) muxer->Finish();
        size_t bytes = output.size();
        ULONGLONG writeStart = GetTickCount64();
        WriteAll(writer, output);
        CastLogLine("segment " + std::to_string(i + 1) + "/" + std::to_string(video_.size()) + ": " +
            std::to_string(bytes / 1024) + " KB, waited download " + std::to_string(waited) + " ms, TV took it in " +
            std::to_string(GetTickCount64() - writeStart) + " ms");
    }
    CastLogLine(closed_ ? "stream closed (new video)" : "stream finished");

    if (muxer)
    {
        muxer->Finish();
        WriteAll(writer, output);
    }
}

} // namespace PerleCast
