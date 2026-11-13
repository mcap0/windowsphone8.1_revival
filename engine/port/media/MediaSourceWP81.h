// Media Source Extensions for the Lumia port (PLAN §73.2 item 6, stage M2). WebCore keeps the track buffers (what was
// appended, what was removed, what overlaps what); the platform's part is here:
//
//  - SourceBufferPrivateWP81 takes the bytes the page appends apart (Demux.cpp's StreamParser: fragmented MP4) into
//    an initialization segment's tracks and samples, and hands the samples WebCore picks for decoding to the
//    MediaQueue;
//  - MediaSourcePrivateWP81 holds the buffers and tells the player when every one has had its initialization segment:
//    the tracks are then known and the pipeline starts.

#pragma once

#if ENABLE(MEDIA_SOURCE)

#include "Demux.h"
#include "MediaQueue.h"
#include <WebCore/MediaSourcePrivate.h>
#include <WebCore/SourceBufferPrivate.h>
#include <wtf/Logger.h>
#include <wtf/WeakPtr.h>

namespace Lumia {

class MediaPlayerPrivateWP81;
class MediaSourcePrivateWP81;

class SourceBufferPrivateWP81 final : public WebCore::SourceBufferPrivate, private Media::StreamParser::Client {
public:
    static Ref<SourceBufferPrivateWP81> create(MediaSourcePrivateWP81&);
    ~SourceBufferPrivateWP81();

    // No value of our own in WebCore's list, and nothing in this build tells the platforms apart by it.
    constexpr WebCore::MediaPlatformType platformType() const final { return WebCore::MediaPlatformType::GStreamer; }

    bool hasInitialization() const { return m_hasInitialization; }
    const std::optional<Media::Track>& videoTrack() const { return m_videoTrack; }
    const std::optional<Media::Track>& audioTrack() const { return m_audioTrack; }

private:
    explicit SourceBufferPrivateWP81(MediaSourcePrivateWP81&);
    RefPtr<MediaSourcePrivateWP81> mediaSource() const;
    RefPtr<MediaQueue> queueFor(WebCore::TrackID, bool& video) const;

    // SourceBufferPrivate
    Ref<WebCore::MediaPromise> appendInternal(Ref<WebCore::SharedBuffer>&&) final;
    void resetParserStateInternal() final;
    bool canSwitchToType(const WebCore::ContentType&) final;
    void flush(WebCore::TrackID) final;
    void enqueueSample(Ref<WebCore::MediaSample>&&, WebCore::TrackID) final;
    void allSamplesInTrackEnqueued(WebCore::TrackID) final;
    bool isReadyForMoreSamples(WebCore::TrackID) final;
    void notifyClientWhenReadyForMoreSamples(WebCore::TrackID) final;
    void processInitializationSegment(std::optional<InitializationSegment>&&) final;
    void removedFromMediaSource() final;

    // StreamParser::Client
    void initialization(const std::vector<Media::Track>&, const std::vector<uint32_t>& ids, long long durationUs) final;
    void sample(size_t track, Media::Sample&&) final;

#if !RELEASE_LOG_DISABLED
    const Logger& logger() const final { return m_logger.get(); }
    ASCIILiteral logClassName() const final { return "SourceBufferPrivateWP81"_s; }
    uint64_t logIdentifier() const final { return m_logIdentifier; }
    WTFLogChannel& logChannel() const final;
    const Logger& sourceBufferLogger() const final { return m_logger.get(); }
    uint64_t sourceBufferLogIdentifier() final { return m_logIdentifier; }
    const Ref<const Logger> m_logger;
    const uint64_t m_logIdentifier;
#endif

    std::unique_ptr<Media::StreamParser> m_parser;
    // The tracks of the last initialization segment, by the parser's index.
    struct ParsedTrack {
        WebCore::TrackID id { 0 };
        bool video { false };
        WebCore::FloatSize size;
        std::vector<uint8_t> config;
    };
    Vector<ParsedTrack> m_parsedTracks;
    std::optional<Media::Track> m_videoTrack, m_audioTrack;
    std::optional<WebCore::TrackID> m_videoId, m_audioId;
    bool m_hasInitialization { false };
};

class MediaSourcePrivateWP81 final : public WebCore::MediaSourcePrivate {
public:
    static Ref<MediaSourcePrivateWP81> create(MediaPlayerPrivateWP81&, WebCore::MediaSourcePrivateClient&);
    ~MediaSourcePrivateWP81();

    constexpr WebCore::MediaPlatformType platformType() const final { return WebCore::MediaPlatformType::GStreamer; }

    RefPtr<WebCore::MediaPlayerPrivateInterface> player() const final;
    void setPlayer(WebCore::MediaPlayerPrivateInterface*) final;

    const Logger& logger() const { return m_logger.get(); }
    uint64_t nextLogIdentifier() { return ++m_nextLogIdentifier; }

    // The buffer whose track of that kind plays (the first one that declared one). Its queue, else null.
    RefPtr<MediaQueue> queueIfOwner(const SourceBufferPrivateWP81&, bool video) const;
    void initializationArrived(SourceBufferPrivateWP81&);
    void bufferRemoved(SourceBufferPrivateWP81&);

private:
    MediaSourcePrivateWP81(MediaPlayerPrivateWP81&, WebCore::MediaSourcePrivateClient&);

    AddStatus addSourceBuffer(const WebCore::ContentType&, const WebCore::MediaSourceConfiguration&, RefPtr<WebCore::SourceBufferPrivate>&) final;
    void durationChanged(const MediaTime&) final;
    void notifyActiveSourceBuffersChanged() final;

    WeakPtr<MediaPlayerPrivateWP81> m_player;
    const Ref<const Logger> m_logger;
    uint64_t m_nextLogIdentifier { 0 };
    const SourceBufferPrivateWP81* m_videoOwner { nullptr };
    const SourceBufferPrivateWP81* m_audioOwner { nullptr };
    bool m_started { false };
};

} // namespace Lumia

#endif // ENABLE(MEDIA_SOURCE)
