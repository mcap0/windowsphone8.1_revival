#include "config.h"
#include "MediaQueue.h"

#include <wtf/MainThread.h>

namespace Lumia {

class MediaQueue::Reader final : public Media::Demuxer {
public:
    Reader(MediaQueue& queue, std::vector<Media::Track>&& tracks)
        : m_queue(queue)
    {
        m_tracks = std::move(tracks);
    }

    const char* container() const final { return "media source"; }
    bool read(size_t track, Media::Sample& sample) final { return track < m_tracks.size() && m_queue->read(m_tracks[track].video, sample); }
    bool pending(size_t track) final { return track < m_tracks.size() && m_queue->pending(m_tracks[track].video); }
    void seek(long long) final { m_queue->acknowledgeSeek(); }
    bool configInBand() const final { return true; }

private:
    const Ref<MediaQueue> m_queue;
};

std::unique_ptr<Media::Demuxer> MediaQueue::createDemuxer(std::vector<Media::Track>&& tracks)
{
    return makeUniqueWithoutFastMallocCheck<Reader>(*this, std::move(tracks));
}

bool MediaQueue::hasRoom(const Track& track, bool video) const
{
    return track.samples.size() < (video ? highVideo : highAudio) && track.bytes < highBytes;
}

void MediaQueue::enqueue(bool video, Media::Sample&& sample)
{
    Locker locker { m_lock };
    if (m_shutDown)
        return;
    auto& track = m_tracks[video];
    track.bytes += sample.data.size();
    track.samples.append(WTF::move(sample));
    track.ended = false;
    if (m_wake && track.samples.size() == 1)
        m_wake();
}

void MediaQueue::flush(bool video)
{
    Locker locker { m_lock };
    auto& track = m_tracks[video];
    track.samples.clear();
    track.bytes = 0;
    track.ended = false;
}

void MediaQueue::setEnded(bool video)
{
    Locker locker { m_lock };
    m_tracks[video].ended = true;
    if (m_wake)
        m_wake();
}

bool MediaQueue::readyForMore(bool video) const
{
    Locker locker { m_lock };
    return hasRoom(m_tracks[video], video);
}

void MediaQueue::notifyWhenReady(bool video, Function<void()>&& callback)
{
    Locker locker { m_lock };
    if (m_shutDown)
        return;
    auto& track = m_tracks[video];
    if (hasRoom(track, video)) {
        callOnMainThread(WTF::move(callback));
        return;
    }
    track.ready = WTF::move(callback);
}

void MediaQueue::beginSeek()
{
    Locker locker { m_lock };
    ++m_seekSerial;
    for (auto& track : m_tracks) {
        track.samples.clear();
        track.bytes = 0;
        track.ended = false;
    }
}

void MediaQueue::acknowledgeSeek()
{
    Locker locker { m_lock };
    m_acknowledged = m_seekSerial;
}

void MediaQueue::setDurationUs(long long duration)
{
    Locker locker { m_lock };
    m_durationUs = duration;
}

long long MediaQueue::durationUs() const
{
    Locker locker { m_lock };
    return m_durationUs;
}

void MediaQueue::setWake(Function<void()>&& wake)
{
    Locker locker { m_lock };
    m_wake = WTF::move(wake);
}

void MediaQueue::shutdown()
{
    Function<void()> wake, readyAudio, readyVideo;
    {
        Locker locker { m_lock };
        m_shutDown = true;
        wake = WTF::move(m_wake);
        readyAudio = WTF::move(m_tracks[0].ready);
        readyVideo = WTF::move(m_tracks[1].ready);
        for (auto& track : m_tracks) {
            track.samples.clear();
            track.bytes = 0;
        }
    }
}

bool MediaQueue::read(bool video, Media::Sample& sample)
{
    Locker locker { m_lock };
    auto& track = m_tracks[video];
    if (m_acknowledged != m_seekSerial || track.samples.isEmpty())
        return false;
    sample = track.samples.takeFirst();
    track.bytes -= std::min(track.bytes, sample.data.size());
    // Half empty: WebCore brings more.
    if (track.ready && track.samples.size() * 2 <= (video ? highVideo : highAudio) && track.bytes * 2 <= highBytes)
        callOnMainThread(std::exchange(track.ready, nullptr));
    return true;
}

bool MediaQueue::finished(bool video) const
{
    Locker locker { m_lock };
    auto& track = m_tracks[video];
    return track.ended && track.samples.isEmpty();
}

bool MediaQueue::pending(bool video)
{
    Locker locker { m_lock };
    auto& track = m_tracks[video];
    return m_acknowledged != m_seekSerial || (track.samples.isEmpty() && !track.ended);
}

} // namespace Lumia
