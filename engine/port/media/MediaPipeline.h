// One media element's playback (PLAN §75): a thread that reads the demuxer, decodes, feeds the audio output and
// hands the picture that is due to the compositor.
//
// The clock is the audio device's (samples played) while there is sound, a monotonic one otherwise. Queues decide the
// work: the thread decodes while less than 0.4 s of sound is queued and fewer than four pictures wait, and sleeps
// until the next picture is due. Play, pause, rate and volume act on the output at once from the Web thread; a seek
// is done by the thread, which then shows the picture at the new time before it reports back.

#pragma once

#include "AudioOutput.h"
#include "Demux.h"
#include "MediaData.h"
#include "MediaDecoder.h"
#include "VideoLayer.h"
#include <WebCore/IntSize.h>
#include <wtf/Condition.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>

namespace WebCore {
class CoordinatedPlatformLayerBufferProxy;
}

namespace Lumia {

class MediaPipeline final : public ThreadSafeRefCounted<MediaPipeline, WTF::DestructionThread::Main> {
public:
    enum class Event : uint8_t { Metadata, Ready, Waiting, Resumed, SeekDone, Ended, DurationChanged, SizeChanged, Failed };

    // `notify` is called on the Web thread.
    static Ref<MediaPipeline> create(Ref<MediaData>&&, Ref<WebCore::CoordinatedPlatformLayerBufferProxy>&&, Function<void(Event)>&& notify);
    ~MediaPipeline();

    // Web thread.
    void start();
    void shutdown();
    void setPlaying(bool);
    void seek(long long timeUs);
    void setRate(double);
    void setVolume(float);
    void setVisible(bool);

    // Any thread.
    long long currentTimeUs() const;
    long long durationUs() const;
    bool hasAudio() const;
    bool hasVideo() const;
    WebCore::IntSize naturalSize() const;
    RefPtr<VideoPicture> currentPicture() const;
    bool ended() const;
    String failure() const;
    bool failedInNetwork() const;
    unsigned decodedFrames() const { return m_decodedFrames; }
    unsigned droppedFrames() const { return m_droppedFrames; }

private:
    MediaPipeline(Ref<MediaData>&&, Ref<WebCore::CoordinatedPlatformLayerBufferProxy>&&, Function<void(Event)>&&);

    void run();
    bool open();
    void loop();
    void performSeek(long long timeUs);
    bool feedAudio();
    bool feedVideo();
    void submitAudio();
    void collectPictures();
    // Shows what is due; returns how long until the next picture is.
    Seconds present();
    void show(Ref<VideoPicture>&&);
    void finishPreroll();
    void checkEnd();
    void catchUp(long long nowUs);
    void fail(const String&, bool network);
    void notify(Event);
    void report(bool final);

    long long clockUs() const WTF_REQUIRES_LOCK(m_lock);
    long long audioClockUs() const WTF_REQUIRES_LOCK(m_lock);
    void updateRunning() WTF_REQUIRES_LOCK(m_lock);

    const Ref<MediaData> m_data;
    const Ref<WebCore::CoordinatedPlatformLayerBufferProxy> m_proxy;
    const Ref<VideoTextures> m_textures;
    Function<void(Event)> m_notify; // Web thread
    RefPtr<Thread> m_thread;        // Web thread

    // The thread's own.
    std::unique_ptr<Media::Demuxer> m_demuxer;
    std::unique_ptr<MediaDecoder> m_audioDecoder, m_videoDecoder;
    size_t m_audioTrack { 0 }, m_videoTrack { 0 };
    std::vector<uint8_t> m_videoConfig;
    Vector<Ref<VideoPicture>> m_pictures; // in presentation order, the next one first
    Ref<VideoPicture> takeNextPicture();
    bool m_audioEnded { false }, m_videoEnded { false };
    long long m_startUs { 0 };          // where playback (re)started: the time of the audio output's first frame
    long long m_lastPictureUs { 0 };
    bool m_reportedReady { false };
    bool m_seeking { false };
    unsigned m_audioFailures { 0 }, m_videoFailures { 0 };
    // Figures for the log.
    double m_decodeMs { 0 }, m_decodeMaxMs { 0 };
    unsigned m_decodeCount { 0 };
    MonotonicTime m_lastReport;
    std::atomic<unsigned> m_decodedFrames { 0 }, m_droppedFrames { 0 };

    AudioOutput m_audio;

    mutable Lock m_lock;
    Condition m_condition;
    bool m_stop WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_playing WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_visible WTF_GUARDED_BY_LOCK(m_lock) { true };
    bool m_stalled WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_prerolled WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_ended WTF_GUARDED_BY_LOCK(m_lock) { false };
    std::optional<long long> m_seekTargetUs WTF_GUARDED_BY_LOCK(m_lock);
    long long m_shownSeekUs WTF_GUARDED_BY_LOCK(m_lock) { -1 }; // the time to report while a seek is on its way
    double m_rate WTF_GUARDED_BY_LOCK(m_lock) { 1 };
    // Clock: the audio output's, or a base time and the monotonic time it was read at.
    bool m_audioMaster WTF_GUARDED_BY_LOCK(m_lock) { false };
    long long m_audioBaseUs WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    long long m_wallBaseUs WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    MonotonicTime m_wallStart WTF_GUARDED_BY_LOCK(m_lock);
    bool m_wallRunning WTF_GUARDED_BY_LOCK(m_lock) { false };
    long long m_endUs WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    // What the element asks about.
    long long m_durationUs WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_hasAudio WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_hasVideo WTF_GUARDED_BY_LOCK(m_lock) { false };
    WebCore::IntSize m_size WTF_GUARDED_BY_LOCK(m_lock);
    RefPtr<VideoPicture> m_picture WTF_GUARDED_BY_LOCK(m_lock);
    String m_failure WTF_GUARDED_BY_LOCK(m_lock);
    bool m_failedInNetwork WTF_GUARDED_BY_LOCK(m_lock) { false };
};

} // namespace Lumia
