#include "config.h"
#include "MediaPlayerPrivateWP81.h"

#include "Engine.h"
#include <WebCore/ContentType.h>
#include <WebCore/CoordinatedPlatformLayerBufferProxy.h>
#include <WebCore/DestinationColorSpace.h>
#include <WebCore/GraphicsContext.h>
#include <WebCore/MediaPlayer.h>
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
                 "audio/x-mp3"_s, "audio/aac"_s, "audio/aacp"_s, "audio/x-aac"_s })
            set.add(type);
        return set;
    }();
    return types;
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
    // Media Source Extensions: not yet (PLAN §73.2, stage M2).
    if (parameters.platformType != PlatformMediaDecodingType::FileOrHLS)
        return MediaPlayer::SupportsType::IsNotSupported;
    auto container = parameters.type.containerType();
    if (container.isEmpty() || !containerTypes().contains(container))
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
    m_pipeline = MediaPipeline::create(*m_data, m_proxy.copyRef(), [weakThis = WeakPtr { *this }](MediaPipeline::Event event) {
        if (RefPtr protectedThis = weakThis.get())
            protectedThis->handle(event);
    });
    m_pipeline->setVisible(m_visible);
    m_pipeline->setRate(m_rate);
    applyVolume();
    m_pipeline->setPlaying(!m_paused);
    setNetworkState(MediaPlayer::NetworkState::Loading);
    setReadyState(MediaPlayer::ReadyState::HaveNothing);
    m_data->start();
    m_pipeline->start();
}

#if ENABLE(MEDIA_SOURCE)
void MediaPlayerPrivateWP81::load(const URL&, const LoadOptions&, MediaSourcePrivateClient&)
{
    m_error = "Media Source Extensions non ancora nel motore"_s;
    setNetworkState(MediaPlayer::NetworkState::FormatError);
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
    return m_pipeline ? FloatSize(m_pipeline->naturalSize()) : FloatSize();
}

bool MediaPlayerPrivateWP81::hasVideo() const
{
    return m_pipeline && m_pipeline->hasVideo();
}

bool MediaPlayerPrivateWP81::hasAudio() const
{
    return m_pipeline && m_pipeline->hasAudio();
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
    if (!m_pipeline || m_readyState < MediaPlayer::ReadyState::HaveMetadata)
        return MediaTime::zeroTime();
    long long us = m_pipeline->durationUs();
    return us > 0 ? timeFromUs(us) : MediaTime::positiveInfiniteTime();
}

MediaTime MediaPlayerPrivateWP81::currentTime() const
{
    return m_pipeline ? timeFromUs(m_pipeline->currentTimeUs()) : MediaTime::zeroTime();
}

Ref<MediaTimePromise> MediaPlayerPrivateWP81::seekToTarget(const SeekTarget& target)
{
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
    if (!m_pipeline || !m_data || !m_data->acceptsRanges())
        return MediaTime::zeroTime();
    long long us = m_pipeline->durationUs();
    return us > 0 ? timeFromUs(us) : MediaTime::zeroTime();
}

// The cached bytes around the playing position, as the time they stand for when the bytes are spread evenly.
const PlatformTimeRanges& MediaPlayerPrivateWP81::buffered() const
{
    m_buffered = PlatformTimeRanges();
    if (!m_pipeline || !m_data)
        return m_buffered;
    long long durationUs = m_pipeline->durationUs();
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
    return m_data && m_data->takeProgress();
}

bool MediaPlayerPrivateWP81::didPassCORSAccessCheck() const
{
    return m_data && m_data->didPassAccessControlCheck();
}

MediaPlayer::MovieLoadType MediaPlayerPrivateWP81::movieLoadType() const
{
    if (!m_pipeline || m_readyState < MediaPlayer::ReadyState::HaveMetadata)
        return MediaPlayer::MovieLoadType::Unknown;
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
