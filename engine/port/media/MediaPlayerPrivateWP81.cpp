#include "config.h"
#include "MediaPlayerPrivateWP81.h"

#include "Engine.h"
#include "MediaSourceWP81.h"
#include <WebCore/ContentType.h>
#include <WebCore/CoordinatedPlatformLayerBufferProxy.h>
#include <WebCore/DestinationColorSpace.h>
#include <WebCore/GraphicsContext.h>
#include <WebCore/MediaPlayer.h>
#include <WebCore/MediaSourcePrivateClient.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PlatformMediaResourceLoader.h>
#include <WebCore/PlatformTimeRanges.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkImage.h>
#include <skia/core/SkPixmap.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END

namespace Lumia {

using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerPrivateWP81);

namespace {

// How far ahead of the playing position the bytes are fetched.
constexpr uint64_t readAheadPlaying = 4 * 1024 * 1024;

MediaTime timeFromUs(long long us)
{
    return MediaTime(us, 1000000);
}

class Factory final : public MediaPlayerFactory {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(Factory);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(Factory);
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::MediaFoundation; }
    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final { return adoptRef(*new MediaPlayerPrivateWP81(player)); }
    void getSupportedTypes(HashSet<String>& types) const final { MediaPlayerPrivateWP81::getSupportedTypes(types); }
    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final { return MediaPlayerPrivateWP81::supportsType(parameters); }
};

void registerEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<Factory>());
}

const HashSet<String>& containerTypes()
{
    static NeverDestroyed<HashSet<String>> types = [] {
        HashSet<String> set;
        for (auto type : { "video/mp4"_s, "video/x-m4v"_s, "audio/mp4"_s, "audio/x-m4a"_s, "audio/m4a"_s, "audio/mpeg"_s, "audio/mp3"_s,
                 "audio/x-mp3"_s, "audio/aac"_s, "audio/aacp"_s, "audio/x-aac"_s,
                 // HLS play lists and their transport streams (media/Hls.cpp).
                 "application/vnd.apple.mpegurl"_s, "application/x-mpegurl"_s, "audio/mpegurl"_s, "audio/x-mpegurl"_s, "video/mp2t"_s })
            set.add(type);
        return set;
    }();
    return types;
}

// What a SourceBuffer takes: fragmented MP4 (Demux.cpp's StreamParser).
bool isMediaSourceContainer(const String& container)
{
    return container == "video/mp4"_s || container == "audio/mp4"_s;
}

// H.264, AAC (plain, with band replication, with parametric stereo) and MP3: what the phone decodes.
bool codecIsPlayable(const String& codec)
{
    if (codec.startsWith("avc1"_s) || codec.startsWith("avc3"_s))
        return true;
    if (codec == "mp4a.40.2"_s || codec == "mp4a.40.5"_s || codec == "mp4a.40.29"_s || codec == "mp4a.40.34"_s || codec == "mp4a.67"_s)
        return true;
    return equalLettersIgnoringASCIICase(codec, "mp3"_s) || equalLettersIgnoringASCIICase(codec, "mp4a.6b"_s) || equalLettersIgnoringASCIICase(codec, "mp4a.69"_s);
}

} // namespace

void installMediaEngine()
{
    g_wp81RegisterMediaEngines = registerEngine;
}

void MediaPlayerPrivateWP81::getSupportedTypes(HashSet<String>& types)
{
    types = containerTypes();
}

MediaPlayer::SupportsType MediaPlayerPrivateWP81::supportsType(const MediaEngineSupportParameters& parameters)
{
    bool mediaSource = parameters.platformType == PlatformMediaDecodingType::MediaSource;
    if (!mediaSource && parameters.platformType != PlatformMediaDecodingType::FileOrHLS)
        return MediaPlayer::SupportsType::IsNotSupported;
    auto container = parameters.type.containerType().convertToASCIILowercase();
    if (container.isEmpty() || !(mediaSource ? isMediaSourceContainer(container) : containerTypes().contains(container)))
        return MediaPlayer::SupportsType::IsNotSupported;
    auto codecs = parameters.type.codecs();
    if (codecs.isEmpty())
        return MediaPlayer::SupportsType::MayBeSupported;
    for (auto& codec : codecs) {
        if (!codecIsPlayable(codec))
            return MediaPlayer::SupportsType::IsNotSupported;
    }
    return MediaPlayer::SupportsType::IsSupported;
}

MediaPlayerPrivateWP81::MediaPlayerPrivateWP81(MediaPlayer& player)
    : m_player(player)
    , m_proxy(CoordinatedPlatformLayerBufferProxy::create())
    , m_logger(player.mediaPlayerLogger())
{
}

MediaPlayerPrivateWP81::~MediaPlayerPrivateWP81()
{
    tearDown();
}

void MediaPlayerPrivateWP81::tearDown()
{
    m_seekPromise = std::nullopt;
    if (auto pipeline = std::exchange(m_pipeline, nullptr))
        pipeline->shutdown();
    if (auto data = std::exchange(m_data, nullptr))
        data->stop();
    if (auto fetcher = std::exchange(m_fetcher, nullptr))
        fetcher->stop();
    if (auto queue = std::exchange(m_queue, nullptr))
        queue->shutdown();
    if (auto source = std::exchange(m_mediaSource, nullptr))
        source->setPlayer(nullptr);
    m_seekWaitsForData = false;
    m_seekReady = std::nullopt;
    ++m_seekSerial;
}

MediaPlayer::ReadyState MediaPlayerPrivateWP81::readyState() const
{
    return m_mediaSource ? m_mediaSource->mediaPlayerReadyState() : m_readyState;
}

void MediaPlayerPrivateWP81::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateWP81::setReadyState(MediaPlayer::ReadyState state)
{
    if (m_readyState == state)
        return;
    m_readyState = state;
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

void MediaPlayerPrivateWP81::load(const String& url)
{
    tearDown();
    RefPtr player = m_player.get();
    if (!player)
        return;
    m_error = { };
    m_waiting = false;
    m_data = MediaData::create(player->mediaResourceLoader(), URL { url });
    m_data->setReadAhead(m_preload == MediaPlayer::Preload::Auto || !m_paused ? readAheadPlaying : 0);
    m_fetcher = MediaFetcher::create(player->mediaResourceLoader());
    MediaPipeline::Source source;
    source.data = m_data;
    source.fetcher = m_fetcher;
    source.url = url;
    m_pipeline = MediaPipeline::create(WTF::move(source), m_proxy.copyRef(), [weakThis = WeakPtr { *this }](MediaPipeline::Event event) {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->handle(event);
    });
    m_pipeline->setVisible(m_visible);
    m_pipeline->setRate(m_rate);
    applyVolume();
    m_pipeline->setPlaying(!m_paused);
    setNetworkState(MediaPlayer::NetworkState::Loading);
    setReadyState(MediaPlayer::ReadyState::HaveNothing);
    // NETWORK_IDLE while the fetch has nothing to ask (HTMLMediaElement fires "stalled" after 3 s of Loading without
    // progress, and some players then give up the source).
    m_data->onRestingChanged = [weakThis = WeakPtr { *this }](bool resting) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis)
            return;
        if (resting && protectedThis->m_networkState == MediaPlayer::NetworkState::Loading)
            protectedThis->setNetworkState(MediaPlayer::NetworkState::Idle);
        else if (!resting && protectedThis->m_networkState == MediaPlayer::NetworkState::Idle)
            protectedThis->setNetworkState(MediaPlayer::NetworkState::Loading);
    };
    m_data->start();
    m_pipeline->start();
}

#if ENABLE(MEDIA_SOURCE)
void MediaPlayerPrivateWP81::load(const URL&, const LoadOptions&, MediaSourcePrivateClient& client)
{
    tearDown();
    m_error = { };
    m_waiting = false;
    m_mediaSourceSize = { };
    m_lastBufferedEnd = -1;
    m_queue = MediaQueue::create();
    setNetworkState(MediaPlayer::NetworkState::Loading);
    if (RefPtr existing = client.mediaSourcePrivate()) {
        // A MediaSource that lived through the element's reload.
        m_mediaSource = static_cast<MediaSourcePrivateWP81*>(existing.get());
        m_mediaSource->setPlayer(this);
        client.reOpen();
    } else
        m_mediaSource = MediaSourcePrivateWP81::create(*this, client);
}

// Every buffer of the page has declared its tracks: the pipeline starts, reading what WebCore queues for it.
void MediaPlayerPrivateWP81::startMediaSource(std::vector<Media::Track>&& tracks)
{
    if (m_pipeline || !m_queue)
        return;
    for (auto& track : tracks) {
        if (track.video)
            m_mediaSourceSize = FloatSize(track.width, track.height);
    }
    MediaPipeline::Source source;
    source.queue = m_queue;
    source.tracks = WTF::move(tracks);
    m_pipeline = MediaPipeline::create(WTF::move(source), m_proxy.copyRef(), [weakThis = WeakPtr { *this }](MediaPipeline::Event event) {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->handle(event);
    });
    m_pipeline->setVisible(m_visible);
    m_pipeline->setRate(m_rate);
    applyVolume();
    m_pipeline->setPlaying(!m_paused);
    if (m_seekWaitsForData)
        m_pipeline->suspendForSeek(m_seekTime.toMicroseconds());
    m_pipeline->start();
    if (auto ready = std::exchange(m_seekReady, std::nullopt))
        m_pipeline->seek(ready->toMicroseconds());
}

void MediaPlayerPrivateWP81::mediaSourceDurationChanged(const MediaTime& duration)
{
    if (m_queue)
        m_queue->setDurationUs(duration.isValid() && duration.isFinite() ? duration.toMicroseconds() : 0);
    if (RefPtr player = m_player.get())
        player->durationChanged();
}

void MediaPlayerPrivateWP81::mediaSourceActiveBuffersChanged()
{
    if (RefPtr player = m_player.get())
        player->activeSourceBuffersChanged();
}

void MediaPlayerPrivateWP81::readyStateFromMediaSourceChanged()
{
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

void MediaPlayerPrivateWP81::characteristicsFromMediaSourceChanged()
{
    if (RefPtr player = m_player.get())
        player->characteristicChanged();
}

void MediaPlayerPrivateWP81::mediaSourceHasRetrievedAllData()
{
    setNetworkState(MediaPlayer::NetworkState::Loaded);
}

// The samples of the position are in the queue (or the pipeline has yet to start: it seeks when it does).
void MediaPlayerPrivateWP81::finishMediaSourceSeek(const MediaTime& time)
{
    m_seekWaitsForData = false;
    m_seekTime = time;
    if (m_pipeline)
        m_pipeline->seek(time.toMicroseconds());
    else {
        // Nothing plays yet: the time is where the pipeline will start.
        m_seekReady = time;
        if (auto promise = std::exchange(m_seekPromise, std::nullopt))
            promise->resolve(time);
        if (RefPtr player = m_player.get())
            player->timeChanged();
    }
}
#endif

#if ENABLE(MEDIA_STREAM)
void MediaPlayerPrivateWP81::load(MediaStreamPrivate&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}
#endif

void MediaPlayerPrivateWP81::cancelLoad()
{
    tearDown();
}

void MediaPlayerPrivateWP81::handle(MediaPipeline::Event event)
{
    RefPtr player = m_player.get();
    if (!player || !m_pipeline)
        return;
    if (m_mediaSource) {
        // The ready state is the MediaSource's (what is buffered); the pipeline tells the rest.
        switch (event) {
        case MediaPipeline::Event::Metadata:
        case MediaPipeline::Event::SizeChanged:
            player->sizeChanged();
            player->characteristicChanged();
            break;
        case MediaPipeline::Event::Ready:
            if (m_pipeline->hasVideo())
                player->firstVideoFrameAvailable();
            break;
        case MediaPipeline::Event::SeekDone:
            if (auto promise = std::exchange(m_seekPromise, std::nullopt))
                promise->resolve(m_seekTime);
            player->timeChanged();
            break;
        case MediaPipeline::Event::Ended:
            player->timeChanged();
            break;
        case MediaPipeline::Event::Failed:
            m_error = m_pipeline->failure();
            m_seekPromise = std::nullopt;
            setNetworkState(MediaPlayer::NetworkState::DecodeError);
            break;
        case MediaPipeline::Event::Waiting:
        case MediaPipeline::Event::Resumed:
        case MediaPipeline::Event::DurationChanged:
            break;
        }
        return;
    }
    switch (event) {
    case MediaPipeline::Event::Metadata:
        m_readyState = MediaPlayer::ReadyState::HaveMetadata;
        player->durationChanged();
        player->sizeChanged();
        player->characteristicChanged();
        player->readyStateChanged();
        break;
    case MediaPipeline::Event::Ready:
        if (m_pipeline->hasVideo())
            player->firstVideoFrameAvailable();
        setReadyState(m_waiting ? MediaPlayer::ReadyState::HaveCurrentData : MediaPlayer::ReadyState::HaveEnoughData);
        break;
    case MediaPipeline::Event::Waiting:
        m_waiting = true;
        if (m_readyState > MediaPlayer::ReadyState::HaveCurrentData)
            setReadyState(MediaPlayer::ReadyState::HaveCurrentData);
        break;
    case MediaPipeline::Event::Resumed:
        m_waiting = false;
        if (m_readyState == MediaPlayer::ReadyState::HaveCurrentData)
            setReadyState(MediaPlayer::ReadyState::HaveEnoughData);
        break;
    case MediaPipeline::Event::SeekDone:
        if (auto promise = std::exchange(m_seekPromise, std::nullopt))
            promise->resolve(m_seekTime);
        player->timeChanged();
        break;
    case MediaPipeline::Event::Ended:
        player->timeChanged();
        break;
    case MediaPipeline::Event::DurationChanged:
        player->durationChanged();
        break;
    case MediaPipeline::Event::SizeChanged:
        player->sizeChanged();
        break;
    case MediaPipeline::Event::Failed:
        m_error = m_pipeline->failure();
        m_seekPromise = std::nullopt;
        if (m_readyState == MediaPlayer::ReadyState::HaveNothing)
            setNetworkState(m_pipeline->failedInNetwork() ? MediaPlayer::NetworkState::NetworkError : MediaPlayer::NetworkState::FormatError);
        else
            setNetworkState(m_pipeline->failedInNetwork() ? MediaPlayer::NetworkState::NetworkError : MediaPlayer::NetworkState::DecodeError);
        break;
    }
}

void MediaPlayerPrivateWP81::play()
{
    m_paused = false;
    if (m_data)
        m_data->setReadAhead(readAheadPlaying);
    if (m_pipeline)
        m_pipeline->setPlaying(true);
}

void MediaPlayerPrivateWP81::pause()
{
    m_paused = true;
    if (m_pipeline)
        m_pipeline->setPlaying(false);
}

bool MediaPlayerPrivateWP81::ended() const
{
    return m_pipeline && m_pipeline->ended();
}

PlatformLayer* MediaPlayerPrivateWP81::platformLayer() const
{
    return m_proxy.ptr();
}

FloatSize MediaPlayerPrivateWP81::naturalSize() const
{
    if (m_pipeline && !m_pipeline->naturalSize().isEmpty())
        return FloatSize(m_pipeline->naturalSize());
    return m_mediaSourceSize;
}

bool MediaPlayerPrivateWP81::hasVideo() const
{
    return (m_pipeline && m_pipeline->hasVideo()) || (m_mediaSource && m_mediaSource->hasVideo());
}

bool MediaPlayerPrivateWP81::hasAudio() const
{
    return (m_pipeline && m_pipeline->hasAudio()) || (m_mediaSource && m_mediaSource->hasAudio());
}

void MediaPlayerPrivateWP81::setPageIsVisible(bool visible)
{
    // No sound from a page that is not shown: the phone gives a suspended app no audio, and a tab left behind would
    // go on unseen.
    m_visible = visible;
    if (m_pipeline)
        m_pipeline->setVisible(visible);
}

MediaTime MediaPlayerPrivateWP81::duration() const
{
    if (m_mediaSource) {
        auto duration = m_mediaSource->duration();
        return duration.isValid() ? duration : MediaTime::zeroTime();
    }
    if (!m_pipeline || m_readyState < MediaPlayer::ReadyState::HaveMetadata)
        return MediaTime::zeroTime();
    long long us = m_pipeline->durationUs();
    return us > 0 ? timeFromUs(us) : MediaTime::positiveInfiniteTime();
}

MediaTime MediaPlayerPrivateWP81::currentTime() const
{
    if (m_mediaSource && (m_seekWaitsForData || m_seekReady || !m_pipeline))
        return m_seekWaitsForData || m_seekReady ? m_seekTime : MediaTime::zeroTime();
    return m_pipeline ? timeFromUs(m_pipeline->currentTimeUs()) : MediaTime::zeroTime();
}

Ref<MediaTimePromise> MediaPlayerPrivateWP81::seekToTarget(const SeekTarget& target)
{
#if ENABLE(MEDIA_SOURCE)
    if (m_mediaSource) {
        // The page may have to bring the samples first: playback stops at once, the queues are emptied and held, and
        // the pipeline seeks when WebCore has queued the position's samples again (from the key frame before it).
        m_seekPromise.emplace(PlatformMediaError::Cancelled);
        m_seekTime = target.time;
        m_seekWaitsForData = true;
        m_seekReady = std::nullopt;
        unsigned serial = ++m_seekSerial;
        m_queue->beginSeek();
        if (m_pipeline)
            m_pipeline->suspendForSeek(target.time.toMicroseconds());
        Ref promise = m_seekPromise->promise();
        m_mediaSource->waitForTarget(target)->whenSettled(RunLoop::mainSingleton(), [weakThis = WeakPtr { *this }, serial](auto&& result) {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis || protectedThis->m_seekSerial != serial || !protectedThis->m_mediaSource)
                return;
            if (!result) {
                protectedThis->m_seekWaitsForData = false;
                if (auto seekPromise = std::exchange(protectedThis->m_seekPromise, std::nullopt))
                    seekPromise->reject(result.error());
                return;
            }
            MediaTime time = *result;
            protectedThis->m_mediaSource->reenqueueMediaForTime(time)->whenSettled(RunLoop::mainSingleton(), [weakThis, serial, time](auto&& result) {
                RefPtr protectedThis = weakThis.get();
                if (!protectedThis || protectedThis->m_seekSerial != serial || !result)
                    return;
                protectedThis->finishMediaSourceSeek(time);
            });
        });
        return promise;
    }
#endif
    if (!m_pipeline || m_readyState < MediaPlayer::ReadyState::HaveMetadata)
        return MediaTimePromise::createAndReject(PlatformMediaError::Cancelled);
    // A seek still on its way is given up for this one (its promise rejects as cancelled).
    m_seekPromise.emplace(PlatformMediaError::Cancelled);
    m_seekTime = target.time;
    m_pipeline->seek(static_cast<long long>(target.time.toDouble() * 1000000));
    return *m_seekPromise;
}

void MediaPlayerPrivateWP81::setRateDouble(double rate)
{
    m_rate = rate;
    if (m_pipeline)
        m_pipeline->setRate(rate);
    if (RefPtr player = m_player.get())
        player->rateChanged();
}

void MediaPlayerPrivateWP81::applyVolume()
{
    if (m_pipeline)
        m_pipeline->setVolume(m_muted ? 0 : m_volume);
}

void MediaPlayerPrivateWP81::setVolume(float volume)
{
    m_volume = volume;
    applyVolume();
}

void MediaPlayerPrivateWP81::setMuted(bool muted)
{
    m_muted = muted;
    applyVolume();
}

void MediaPlayerPrivateWP81::setPreload(MediaPlayer::Preload preload)
{
    m_preload = preload;
    if (m_data && preload == MediaPlayer::Preload::Auto)
        m_data->setReadAhead(readAheadPlaying);
}

MediaTime MediaPlayerPrivateWP81::maxTimeSeekable() const
{
    if (m_mediaSource) {
        auto duration = m_mediaSource->duration();
        if (duration.isValid() && duration.isFinite())
            return duration;
        auto seekable = m_mediaSource->seekable();
        return seekable.length() ? seekable.maximumBufferedTime() : MediaTime::zeroTime();
    }
    if (!m_pipeline || !m_data || m_pipeline->live())
        return MediaTime::zeroTime();
    // A play list is fetched whole whatever the server says about ranges: its segments are what a seek asks for.
    if (!m_data->acceptsRanges() && m_pipeline->bufferedEndUs() < 0)
        return MediaTime::zeroTime();
    long long us = m_pipeline->durationUs();
    return us > 0 ? timeFromUs(us) : MediaTime::zeroTime();
}

// The cached bytes around the playing position, as the time they stand for when the bytes are spread evenly.
const PlatformTimeRanges& MediaPlayerPrivateWP81::buffered() const
{
    m_buffered = PlatformTimeRanges();
    if (m_mediaSource) {
        m_buffered = m_mediaSource->buffered();
        return m_buffered;
    }
    if (!m_pipeline || !m_data)
        return m_buffered;
    long long durationUs = m_pipeline->durationUs();
    // HLS: the segments taken apart so far, from the playing position.
    if (long long endUs = m_pipeline->bufferedEndUs(); endUs >= 0) {
        long long nowUs = m_pipeline->currentTimeUs();
        if (endUs > nowUs)
            m_buffered.add(timeFromUs(nowUs), timeFromUs(endUs));
        return m_buffered;
    }
    long long total = m_data->size();
    if (durationUs <= 0 || total <= 0)
        return m_buffered;
    uint64_t start = 0, end = 0;
    m_data->cachedRange(start, end);
    if (end <= start)
        return m_buffered;
    double perByte = static_cast<double>(durationUs) / static_cast<double>(total);
    m_buffered.add(timeFromUs(static_cast<long long>(start * perByte)), timeFromUs(std::min<long long>(durationUs, static_cast<long long>(end * perByte))));
    return m_buffered;
}

unsigned long long MediaPlayerPrivateWP81::totalBytes() const
{
    long long total = m_data ? m_data->size() : 0;
    return total > 0 ? total : 0;
}

bool MediaPlayerPrivateWP81::didLoadingProgress() const
{
    if (m_mediaSource) {
        // Progress is what the page appended.
        auto buffered = m_mediaSource->buffered();
        double end = buffered.length() ? buffered.maximumBufferedTime().toDouble() : 0;
        return std::exchange(m_lastBufferedEnd, end) != end;
    }
    bool progress = m_data && m_data->takeProgress();
    if (m_fetcher && m_fetcher->takeProgress())
        progress = true;
    return progress;
}

bool MediaPlayerPrivateWP81::didPassCORSAccessCheck() const
{
    // What a MediaSource plays came through the page's own script.
    return m_mediaSource || (m_data && m_data->didPassAccessControlCheck());
}

MediaPlayer::MovieLoadType MediaPlayerPrivateWP81::movieLoadType() const
{
    if (m_mediaSource)
        return MediaPlayer::MovieLoadType::StoredStream;
    if (!m_pipeline || m_readyState < MediaPlayer::ReadyState::HaveMetadata)
        return MediaPlayer::MovieLoadType::Unknown;
    if (m_pipeline->bufferedEndUs() >= 0)
        return MediaPlayer::MovieLoadType::HttpLiveStream;
    return m_pipeline->durationUs() > 0 ? MediaPlayer::MovieLoadType::Download : MediaPlayer::MovieLoadType::LiveStream;
}

// The current picture as an image (a canvas that draws the video, a page painted without layers): converted on the
// CPU, for these uses only.
RefPtr<NativeImage> MediaPlayerPrivateWP81::nativeImageForCurrentTime()
{
    RefPtr picture = m_pipeline ? m_pipeline->currentPicture() : nullptr;
    if (!picture)
        return nullptr;
    int width = picture->width, height = picture->height, chromaWidth = picture->chromaWidth();
    Vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    uint8_t* out = pixels.mutableSpan().data();
    const uint8_t* yPlane = picture->y();
    const uint8_t* uPlane = picture->u();
    const uint8_t* vPlane = picture->v();
    // Video range; BT.709 for high definition, BT.601 below (fixed point, 8 bits of fraction).
    int rv = picture->bt709 ? 459 : 409, gu = picture->bt709 ? 55 : 100, gv = picture->bt709 ? 136 : 208, bu = picture->bt709 ? 541 : 516;
    auto clamp = [](int value) { return static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value); };
    for (int row = 0; row < height; ++row) {
        const uint8_t* y = yPlane + static_cast<size_t>(row) * width;
        const uint8_t* u = uPlane + static_cast<size_t>(row / 2) * chromaWidth;
        const uint8_t* v = vPlane + static_cast<size_t>(row / 2) * chromaWidth;
        uint8_t* pixel = out + static_cast<size_t>(row) * width * 4;
        for (int x = 0; x < width; ++x, pixel += 4) {
            int c = (y[x] - 16) * 298 + 128;
            int d = u[x / 2] - 128;
            int e = v[x / 2] - 128;
            pixel[0] = clamp((c + bu * d) >> 8);
            pixel[1] = clamp((c - gu * d - gv * e) >> 8);
            pixel[2] = clamp((c + rv * e) >> 8);
            pixel[3] = 255;
        }
    }
    auto info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
    return NativeImage::create(SkImages::RasterFromPixmapCopy(SkPixmap(info, out, static_cast<size_t>(width) * 4)));
}

void MediaPlayerPrivateWP81::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled())
        return;
    if (RefPtr image = nativeImageForCurrentTime())
        context.drawNativeImage(*image, rect, FloatRect { { }, image->size() });
}

DestinationColorSpace MediaPlayerPrivateWP81::colorSpace()
{
    return DestinationColorSpace::SRGB();
}

unsigned MediaPlayerPrivateWP81::decodedFrameCount() const
{
    return m_pipeline ? m_pipeline->decodedFrames() : 0;
}

unsigned MediaPlayerPrivateWP81::droppedFrameCount() const
{
    return m_pipeline ? m_pipeline->droppedFrames() : 0;
}

} // namespace Lumia
