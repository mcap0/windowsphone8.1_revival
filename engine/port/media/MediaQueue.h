// Media Source Extensions (PLAN §73.2 item 6, stage M2): the samples WebCore's track buffers hand over for decoding,
// between the Web thread (SourceBufferPrivateWP81::enqueueSample) and the pipeline's thread, which reads them through
// a Demuxer like any other source.
//
// One queue per kind. WebCore fills a queue while it is "ready for more" and is called back when the pipeline has
// read it down. A seek: beginSeek() empties the queues and holds the reader back until the pipeline has flushed its
// decoders (the demuxer's seek()), so that no sample of the new position is decoded before the flush.

#pragma once

#include "Demux.h"
#include <wtf/Deque.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace Lumia {

class MediaQueue final : public ThreadSafeRefCounted<MediaQueue> {
public:
    static Ref<MediaQueue> create() { return adoptRef(*new MediaQueue); }

    // Web thread.
    void enqueue(bool video, Media::Sample&&);
    void flush(bool video);
    void setEnded(bool video);
    bool readyForMore(bool video) const;
    // `callback` runs on the Web thread, once, when the queue has room again.
    void notifyWhenReady(bool video, Function<void()>&& callback);
    void beginSeek();
    void setDurationUs(long long);
    void shutdown();

    // Any thread.
    long long durationUs() const;
    // The track's last sample was handed over and read.
    bool finished(bool video) const;
    // Called (any thread) when samples arrive or a track ends: the pipeline's thread wakes.
    void setWake(Function<void()>&&);

    // The pipeline's thread: the reader's side, over the tracks the pipeline was started with.
    std::unique_ptr<Media::Demuxer> createDemuxer(std::vector<Media::Track>&&);

private:
    class Reader;
    friend class Reader;
    MediaQueue() = default;

    struct Track {
        Deque<Media::Sample> samples;
        size_t bytes { 0 };
        bool ended { false };
        Function<void()> ready;
    };

    static constexpr size_t highVideo = 48, highAudio = 96, highBytes = 6 * 1024 * 1024;

    bool read(bool video, Media::Sample&);
    bool pending(bool video);
    void acknowledgeSeek();
    bool hasRoom(const Track&, bool video) const WTF_REQUIRES_LOCK(m_lock);

    mutable Lock m_lock;
    Track m_tracks[2] WTF_GUARDED_BY_LOCK(m_lock); // 0 = audio, 1 = video
    unsigned m_seekSerial WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    unsigned m_acknowledged WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    long long m_durationUs WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_shutDown WTF_GUARDED_BY_LOCK(m_lock) { false };
    Function<void()> m_wake WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace Lumia
