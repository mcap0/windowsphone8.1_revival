#include "config.h"
#include "MediaSourceWP81.h"

#if ENABLE(MEDIA_SOURCE)

#include "Engine.h"
#include "MediaPlayerPrivateWP81.h"
#include <WebCore/AudioTrackPrivate.h>
#include <WebCore/ContentType.h>
#include <WebCore/Logging.h>
#include <WebCore/MediaDescription.h>
#include <WebCore/MediaSample.h>
#include <WebCore/MediaSourcePrivateClient.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/VideoTrackPrivate.h>
#include <wtf/TZoneMallocInlines.h>

namespace Lumia {

using namespace WebCore;

namespace {

MediaTime timeFromUs(long long us)
{
    return MediaTime(us, 1000000);
}

using SharedBytes = std::shared_ptr<const std::vector<uint8_t>>;

// One coded frame in WebCore's track buffers. The bytes are shared by the copies WebCore makes of a sample.
class SampleWP81 final : public MediaSample {
public:
    static Ref<SampleWP81> create(TrackID id, bool video, const FloatSize& size, Media::Sample&& sample)
    {
        Ref result = adoptRef(*new SampleWP81);
        result->m_id = id;
        result->m_video = video;
        result->m_size = size;
        result->m_presentation = timeFromUs(sample.ptsUs);
        result->m_decode = timeFromUs(sample.dtsUs);
        result->m_duration = timeFromUs(sample.durationUs);
        result->m_key = sample.key;
        result->m_bytes = std::make_shared<const std::vector<uint8_t>>(std::move(sample.data));
        return result;
    }

    bool video() const { return m_video; }
    Media::Sample toSample() const
    {
        Media::Sample sample;
        sample.ptsUs = m_presentation.toMicroseconds();
        sample.dtsUs = m_decode.toMicroseconds();
        sample.durationUs = m_duration.toMicroseconds();
        sample.key = m_key;
        sample.data = *m_bytes;
        return sample;
    }

private:
    SampleWP81() = default;
    Ref<SampleWP81> copy() const
    {
        Ref result = adoptRef(*new SampleWP81);
        result->m_id = m_id;
        result->m_video = m_video;
        result->m_size = m_size;
        result->m_presentation = m_presentation;
        result->m_decode = m_decode;
        result->m_duration = m_duration;
        result->m_key = m_key;
        result->m_nonDisplaying = m_nonDisplaying;
        result->m_bytes = m_bytes;
        return result;
    }

    MediaTime presentationTime() const final { return m_presentation; }
    MediaTime decodeTime() const final { return m_decode; }
    MediaTime duration() const final { return m_duration; }
    TrackID trackID() const final { return m_id; }
    size_t sizeInBytes() const final { return m_bytes->size(); }
    FloatSize presentationSize() const final { return m_size; }
    void offsetTimestampsBy(const MediaTime& offset) final
    {
        m_presentation += offset;
        m_decode += offset;
    }
    void setTimestamps(const MediaTime& presentation, const MediaTime& decode) final
    {
        m_presentation = presentation;
        m_decode = decode;
    }
    Ref<MediaSample> createNonDisplayingCopy() const final
    {
        auto result = copy();
        result->m_nonDisplaying = true;
        return result;
    }
    Ref<MediaSample> createCopyWithAdjustedStartTime(const MediaTime& offset) const final
    {
        MediaTime clamped = std::max(MediaTime::zeroTime(), std::min(offset, m_duration));
        auto result = copy();
        result->m_presentation = m_presentation + clamped;
        result->m_decode = m_decode + clamped;
        result->m_duration = m_duration - clamped;
        return result;
    }
    SampleFlags flags() const final
    {
        unsigned flags = None;
        if (m_key)
            flags |= IsSync;
        if (m_nonDisplaying)
            flags |= IsNonDisplaying;
        return static_cast<SampleFlags>(flags);
    }
    PlatformSample platformSample() const final { return PlatformSample { PlatformSample::VariantType { static_cast<const MockSampleBox*>(nullptr) } }; }
    Type type() const final { return Type::None; }

    TrackID m_id { 0 };
    bool m_video { false };
    bool m_key { false };
    bool m_nonDisplaying { false };
    FloatSize m_size;
    MediaTime m_presentation, m_decode, m_duration;
    SharedBytes m_bytes;
};

class DescriptionWP81 final : public MediaDescription {
public:
    static Ref<DescriptionWP81> create(const Media::Track& track) { return adoptRef(*new DescriptionWP81(track)); }
    bool isVideo() const final { return m_video; }
    bool isAudio() const final { return !m_video; }
    bool isText() const final { return false; }

private:
    explicit DescriptionWP81(const Media::Track& track)
        : MediaDescription(String::fromUTF8(track.name.c_str()).isolatedCopy())
        , m_video(track.video)
    {
    }
    bool m_video;
};

class VideoTrackWP81 final : public VideoTrackPrivate {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(VideoTrackWP81);
public:
    static Ref<VideoTrackWP81> create(TrackID id) { return adoptRef(*new VideoTrackWP81(id)); }
    TrackID id() const final { return m_id; }
    Kind kind() const final { return Kind::Main; }

private:
    explicit VideoTrackWP81(TrackID id)
        : m_id(id)
    {
    }
    TrackID m_id;
};

class AudioTrackWP81 final : public AudioTrackPrivate {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(AudioTrackWP81);
public:
    static Ref<AudioTrackWP81> create(TrackID id) { return adoptRef(*new AudioTrackWP81(id)); }
    TrackID id() const final { return m_id; }
    Kind kind() const final { return Kind::Main; }

private:
    explicit AudioTrackWP81(TrackID id)
        : m_id(id)
    {
    }
    TrackID m_id;
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------

Ref<SourceBufferPrivateWP81> SourceBufferPrivateWP81::create(MediaSourcePrivateWP81& parent)
{
    return adoptRef(*new SourceBufferPrivateWP81(parent));
}

SourceBufferPrivateWP81::SourceBufferPrivateWP81(MediaSourcePrivateWP81& parent)
    : SourceBufferPrivate(parent)
#if !RELEASE_LOG_DISABLED
    , m_logger(parent.logger())
    , m_logIdentifier(parent.nextLogIdentifier())
#endif
    , m_parser(Media::StreamParser::create())
{
}

SourceBufferPrivateWP81::~SourceBufferPrivateWP81() = default;

#if !RELEASE_LOG_DISABLED
WTFLogChannel& SourceBufferPrivateWP81::logChannel() const
{
    return LogMediaSource;
}
#endif

RefPtr<MediaSourcePrivateWP81> SourceBufferPrivateWP81::mediaSource() const
{
    RefPtr source = m_mediaSource.get();
    return static_cast<MediaSourcePrivateWP81*>(source.get());
}

RefPtr<MediaQueue> SourceBufferPrivateWP81::queueFor(TrackID id, bool& video) const
{
    if (m_videoId && *m_videoId == id)
        video = true;
    else if (m_audioId && *m_audioId == id)
        video = false;
    else
        return nullptr;
    RefPtr source = mediaSource();
    return source ? source->queueIfOwner(*this, video) : nullptr;
}

Ref<MediaPromise> SourceBufferPrivateWP81::appendInternal(Ref<SharedBuffer>&& data)
{
    std::string error;
    auto bytes = data->span();
    if (!m_parser->append(bytes.data(), bytes.size(), *this, error)) {
        log("media source: append rifiutato: %s", error.c_str());
        return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
    }
    return MediaPromise::createAndResolve();
}

void SourceBufferPrivateWP81::resetParserStateInternal()
{
    m_parser->reset();
}

bool SourceBufferPrivateWP81::canSwitchToType(const ContentType& type)
{
    MediaEngineSupportParameters parameters { .platformType = PlatformMediaDecodingType::MediaSource, .type = type };
    return MediaPlayerPrivateWP81::supportsType(parameters) != MediaPlayer::SupportsType::IsNotSupported;
}

void SourceBufferPrivateWP81::initialization(const std::vector<Media::Track>& tracks, const std::vector<uint32_t>& ids, long long durationUs)
{
    InitializationSegment segment;
    segment.duration = durationUs > 0 ? timeFromUs(durationUs) : MediaTime::invalidTime();
    m_parsedTracks.clear();
    m_videoTrack = std::nullopt;
    m_audioTrack = std::nullopt;
    m_videoId = std::nullopt;
    m_audioId = std::nullopt;
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto& track = tracks[i];
        TrackID id = i < ids.size() ? ids[i] : i + 1;
        ParsedTrack parsed;
        parsed.id = id;
        parsed.video = track.video;
        if (track.video) {
            parsed.size = FloatSize(track.width, track.height);
            parsed.config = track.config;
            InitializationSegment::VideoTrackInformation info;
            info.track = VideoTrackWP81::create(id);
            info.description = DescriptionWP81::create(track);
            segment.videoTracks.append(WTF::move(info));
            m_videoTrack = track;
            m_videoId = id;
        } else {
            InitializationSegment::AudioTrackInformation info;
            info.track = AudioTrackWP81::create(id);
            info.description = DescriptionWP81::create(track);
            segment.audioTracks.append(WTF::move(info));
            m_audioTrack = track;
            m_audioId = id;
        }
        m_parsedTracks.append(WTF::move(parsed));
    }
    didReceiveInitializationSegment(WTF::move(segment));
}

void SourceBufferPrivateWP81::sample(size_t index, Media::Sample&& sample)
{
    if (index >= m_parsedTracks.size())
        return;
    auto& track = m_parsedTracks[index];
    // The parameter sets go with every key frame: a later initialization segment may bring others (a change of
    // quality), and the decoder starts from any key frame.
    if (track.video && sample.key && !track.config.empty())
        sample.data.insert(sample.data.begin(), track.config.begin(), track.config.end());
    didReceiveSample(SampleWP81::create(track.id, track.video, track.size, WTF::move(sample)));
}

void SourceBufferPrivateWP81::processInitializationSegment(std::optional<InitializationSegment>&& segment)
{
    if (!segment)
        return;
    m_hasInitialization = true;
    if (RefPtr source = mediaSource())
        source->initializationArrived(*this);
}

void SourceBufferPrivateWP81::removedFromMediaSource()
{
    if (RefPtr source = mediaSource())
        source->bufferRemoved(*this);
    SourceBufferPrivate::removedFromMediaSource();
}

void SourceBufferPrivateWP81::flush(TrackID id)
{
    bool video = false;
    if (RefPtr queue = queueFor(id, video))
        queue->flush(video);
}

void SourceBufferPrivateWP81::enqueueSample(Ref<MediaSample>&& sample, TrackID id)
{
    bool video = false;
    RefPtr queue = queueFor(id, video);
    if (!queue || sample->type() != MediaSample::Type::None)
        return;
    queue->enqueue(video, static_cast<SampleWP81&>(sample.get()).toSample());
}

void SourceBufferPrivateWP81::allSamplesInTrackEnqueued(TrackID id)
{
    bool video = false;
    if (RefPtr queue = queueFor(id, video))
        queue->setEnded(video);
}

bool SourceBufferPrivateWP81::isReadyForMoreSamples(TrackID id)
{
    bool video = false;
    RefPtr queue = queueFor(id, video);
    // A track nobody plays takes everything (and keeps nothing).
    return !queue || queue->readyForMore(video);
}

void SourceBufferPrivateWP81::notifyClientWhenReadyForMoreSamples(TrackID id)
{
    bool video = false;
    RefPtr queue = queueFor(id, video);
    if (!queue)
        return;
    queue->notifyWhenReady(video, [weakThis = ThreadSafeWeakPtr { *this }, id] {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->provideMediaData(id);
    });
}

// ---------------------------------------------------------------------------------------------------------------

Ref<MediaSourcePrivateWP81> MediaSourcePrivateWP81::create(MediaPlayerPrivateWP81& player, MediaSourcePrivateClient& client)
{
    Ref source = adoptRef(*new MediaSourcePrivateWP81(player, client));
    client.setPrivateAndOpen(source.copyRef());
    return source;
}

MediaSourcePrivateWP81::MediaSourcePrivateWP81(MediaPlayerPrivateWP81& player, MediaSourcePrivateClient& client)
    : MediaSourcePrivate(client)
    , m_player(player)
    , m_logger(player.logger())
{
}

MediaSourcePrivateWP81::~MediaSourcePrivateWP81() = default;

RefPtr<MediaPlayerPrivateInterface> MediaSourcePrivateWP81::player() const
{
    return m_player.get();
}

void MediaSourcePrivateWP81::setPlayer(MediaPlayerPrivateInterface* player)
{
    m_player = static_cast<MediaPlayerPrivateWP81*>(player);
    m_started = false;
}

MediaSourcePrivate::AddStatus MediaSourcePrivateWP81::addSourceBuffer(const ContentType& type, const MediaSourceConfiguration&, RefPtr<SourceBufferPrivate>& out)
{
    MediaEngineSupportParameters parameters { .platformType = PlatformMediaDecodingType::MediaSource, .type = type };
    if (MediaPlayerPrivateWP81::supportsType(parameters) == MediaPlayer::SupportsType::IsNotSupported)
        return AddStatus::NotSupported;
    {
        Locker locker { m_lock };
        m_sourceBuffers.append(SourceBufferPrivateWP81::create(*this));
        out = m_sourceBuffers.last();
    }
    out->setMediaSourceDuration(duration());
    return AddStatus::Ok;
}

void MediaSourcePrivateWP81::durationChanged(const MediaTime& duration)
{
    MediaSourcePrivate::durationChanged(duration);
    if (RefPtr player = m_player.get())
        player->mediaSourceDurationChanged(duration);
}

void MediaSourcePrivateWP81::notifyActiveSourceBuffersChanged()
{
    if (RefPtr player = m_player.get())
        player->mediaSourceActiveBuffersChanged();
}

RefPtr<MediaQueue> MediaSourcePrivateWP81::queueIfOwner(const SourceBufferPrivateWP81& buffer, bool video) const
{
    if ((video ? m_videoOwner : m_audioOwner) != &buffer)
        return nullptr;
    RefPtr player = m_player.get();
    return player ? player->mediaQueue() : nullptr;
}

void MediaSourcePrivateWP81::bufferRemoved(SourceBufferPrivateWP81& buffer)
{
    if (m_videoOwner == &buffer)
        m_videoOwner = nullptr;
    if (m_audioOwner == &buffer)
        m_audioOwner = nullptr;
}

// The tracks are known when every buffer the page made has had its initialization segment (the usual page makes one
// buffer for the pictures and one for the sound, and appends to both before it expects anything to play).
void MediaSourcePrivateWP81::initializationArrived(SourceBufferPrivateWP81& buffer)
{
    if (!m_videoOwner && buffer.videoTrack())
        m_videoOwner = &buffer;
    if (!m_audioOwner && buffer.audioTrack())
        m_audioOwner = &buffer;
    if (m_started)
        return;
    for (auto& other : sourceBuffers()) {
        if (!static_cast<SourceBufferPrivateWP81&>(other.get()).hasInitialization())
            return;
    }
    RefPtr player = m_player.get();
    if (!player || (!m_videoOwner && !m_audioOwner))
        return;
    std::vector<Media::Track> tracks;
    if (m_videoOwner)
        tracks.push_back(*m_videoOwner->videoTrack());
    if (m_audioOwner)
        tracks.push_back(*m_audioOwner->audioTrack());
    m_started = true;
    player->startMediaSource(WTF::move(tracks));
}

} // namespace Lumia

#endif // ENABLE(MEDIA_SOURCE)
