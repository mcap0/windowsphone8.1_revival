// The media engine of the Lumia port (PLAN §75): what an <audio> or <video> element drives. It lives on the Web
// thread and only keeps the element's view of things; the work is the MediaPipeline's thread, the bytes come through
// MediaData.

#pragma once

#include "MediaPipeline.h"
#include <WebCore/MediaPlayerPrivate.h>
#include <WebCore/PlatformLayer.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>

namespace WebCore {
class CoordinatedPlatformLayerBufferProxy;
}

namespace Lumia {

// Web thread, once, before the first media element: hooks the engine into WebCore's list.
void installMediaEngine();

class MediaPlayerPrivateWP81 final
    : public WebCore::MediaPlayerPrivateInterface
    , public CanMakeWeakPtr<MediaPlayerPrivateWP81>
    , public RefCounted<MediaPlayerPrivateWP81> {
    WTF_MAKE_TZONE_ALLOCATED(MediaPlayerPrivateWP81);
public:
    explicit MediaPlayerPrivateWP81(WebCore::MediaPlayer&);
    ~MediaPlayerPrivateWP81();

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    static WebCore::MediaPlayer::SupportsType supportsType(const WebCore::MediaEngineSupportParameters&);
    static void getSupportedTypes(HashSet<String>&);

private:
    constexpr WebCore::MediaPlayerType mediaPlayerType() const final { return WebCore::MediaPlayerType::MediaFoundation; }

    void load(const String&) final;
#if ENABLE(MEDIA_SOURCE)
    void load(const URL&, const LoadOptions&, WebCore::MediaSourcePrivateClient&) final;
#endif
#if ENABLE(MEDIA_STREAM)
    void load(WebCore::MediaStreamPrivate&) final;
#endif
    void cancelLoad() final;
    void play() final;
    void pause() final;
    bool paused() const final { return m_paused; }
    bool ended() const final;

    PlatformLayer* platformLayer() const final;
    bool supportsAcceleratedRendering() const final { return true; }
    WebCore::FloatSize naturalSize() const final;
    bool hasVideo() const final;
    bool hasAudio() const final;
    void setPageIsVisible(bool) final;

    MediaTime duration() const final;
    MediaTime currentTime() const final;
    Ref<WebCore::MediaTimePromise> seekToTarget(const WebCore::SeekTarget&) final;
    void setRateDouble(double) final;
    double rate() const final { return m_rate; }
    void setVolume(float) final;
    void setMuted(bool) final;
    void setPreload(WebCore::MediaPlayer::Preload) final;

    WebCore::MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    WebCore::MediaPlayer::ReadyState readyState() const final { return m_readyState; }
    MediaTime maxTimeSeekable() const final;
    const WebCore::PlatformTimeRanges& buffered() const final;
    unsigned long long totalBytes() const final;
    bool didLoadingProgress() const final;
    bool didPassCORSAccessCheck() const final;
    WebCore::MediaPlayer::MovieLoadType movieLoadType() const final;

    void paint(WebCore::GraphicsContext&, const WebCore::FloatRect&) final;
    RefPtr<WebCore::NativeImage> nativeImageForCurrentTime() final;
    WebCore::DestinationColorSpace colorSpace() final;
    unsigned decodedFrameCount() const final;
    unsigned droppedFrameCount() const final;
    String engineDescription() const final { return "Lumia"_s; }
    String errorMessage() const final { return m_error; }

    void handle(MediaPipeline::Event);
    void tearDown();
    void setNetworkState(WebCore::MediaPlayer::NetworkState);
    void setReadyState(WebCore::MediaPlayer::ReadyState);
    void applyVolume();

    ThreadSafeWeakPtr<WebCore::MediaPlayer> m_player;
    const Ref<WebCore::CoordinatedPlatformLayerBufferProxy> m_proxy;
    RefPtr<MediaData> m_data;
    RefPtr<MediaPipeline> m_pipeline;
    WebCore::MediaPlayer::NetworkState m_networkState { WebCore::MediaPlayer::NetworkState::Empty };
    WebCore::MediaPlayer::ReadyState m_readyState { WebCore::MediaPlayer::ReadyState::HaveNothing };
    WebCore::MediaPlayer::Preload m_preload { WebCore::MediaPlayer::Preload::Auto };
    std::optional<WebCore::MediaTimePromise::AutoRejectProducer> m_seekPromise;
    MediaTime m_seekTime;
    bool m_paused { true };
    bool m_waiting { false };
    bool m_visible { true };
    double m_rate { 1 };
    float m_volume { 1 };
    bool m_muted { false };
    String m_error;
    mutable WebCore::PlatformTimeRanges m_buffered;
};

} // namespace Lumia
