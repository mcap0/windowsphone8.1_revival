#ifndef PERLECAST_STREAMSERVER_H
#define PERLECAST_STREAMSERVER_H

#include <collection.h>
#include <cstdint>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <windows.networking.sockets.h>
#include <windows.storage.streams.h>

namespace PerleCast {

struct StreamAudioOption
{
    std::string name;
    std::string language;
    std::string url;
    bool isDefault;
    StreamAudioOption() : isDefault(false) {}
};

ref class StreamServer sealed
{
public:
    StreamServer();
    // Reads the master and media playlists (blocking: call it on a worker thread) and starts listening.
    Platform::String^ Start(Platform::String^ masterUrl,
                            Platform::String^ audioLanguage,
                            Platform::String^ subtitleVttUrl);
    void Close();

internal:
    int64_t DurationSeconds() { return static_cast<int64_t>(totalTicks_ / 10000000); }
    // the audio renditions of the video and the one being streamed
    const std::vector<StreamAudioOption>& AudioOptions() { return audioOptions_; }
    size_t AudioIndex() { return audioIndex_; }
    // highest resolution to stream (set before Start); the resolutions available and the one streamed
    void SetMaxHeight(int height) { maxHeight_ = height; }
    // segments kept in memory (fewer while the app is in the background)
    void SetCacheLimit(size_t count) { cacheLimit_ = count; }
    const std::vector<int>& Heights() { return heights_; }
    int Height() { return height_; }
    long Streams() { return streams_; }  // /v.ts connections being served

private:
    struct Segment { std::string url; int64_t ticks; int64_t start; };
    // one video segment and the audio segments that cover the same time
    struct SegmentData
    {
        std::vector<unsigned char> video;
        std::vector<std::vector<unsigned char> > audio;
    };
    typedef std::tuple<size_t, size_t, size_t> SegmentKey;  // video index, audio range

    // downloads shared by the TV's connections (it opens /v.ts with HEAD, GET, drops it and asks again):
    // each segment is fetched once, the few around the playing point stay in memory
    std::mutex cacheLock_;
    std::map<SegmentKey, std::shared_future<SegmentData> > cache_;
    std::shared_future<SegmentData> FetchSegment(size_t index, size_t audioFirst, size_t audioEnd);

    // the TV's open connections: Close() shuts them. A /v.ts left open and silent (download stuck, or a closed
    // server whose stream had not noticed yet) makes the TV refuse new addresses with 701 for good (tvlab F1/F2)
    std::mutex socketsLock_;
    std::vector<Windows::Networking::Sockets::StreamSocket^> sockets_;

    Windows::Networking::Sockets::StreamSocketListener^ listener_;
    std::string masterUrl_;
    std::string audioLanguage_;
    std::string subtitleVttUrl_;
    std::string localUrl_;
    std::vector<Segment> video_;
    std::vector<Segment> audio_;
    int64_t totalTicks_;
    std::vector<StreamAudioOption> audioOptions_;
    size_t audioIndex_;
    int maxHeight_;
    std::vector<int> heights_;
    int height_;
    volatile size_t cacheLimit_;
    volatile long generation_;
    volatile long streams_;
    bool closed_;

    void Prepare();
    void OnConnectionReceived(
        Windows::Networking::Sockets::StreamSocketListener^ sender,
        Windows::Networking::Sockets::StreamSocketListenerConnectionReceivedEventArgs^ args);
    void Serve(Windows::Networking::Sockets::StreamSocket^ socket);
    void StreamTransport(Windows::Storage::Streams::DataWriter^ writer, double startSeconds, long generation);
    std::string BuildSubtitle(double startSeconds);
};

} // namespace PerleCast

#endif
