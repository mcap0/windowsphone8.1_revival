#ifndef PERLECAST_CASTQUEUE_H
#define PERLECAST_CASTQUEUE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace PerleCast {

struct CastQueueItem {
    std::string videoUrl;      // YouTube page URL, identifies the video
    std::string title;
    std::string uploader;
    std::string hlsUrl;        // HLS master playlist
    std::string audioLanguage; // wanted audio language, "" = default
    std::string subtitleUrl;   // WebVTT URL, "" = none
    int64_t startSeconds;      // where to start when it is played; -1 = from the saved position (decided by the caller)
    int64_t durationSeconds;   // -1 unknown
    std::vector<std::pair<std::string, std::string> > subtitles; // label and WebVTT URL of each choice

    CastQueueItem() : startSeconds(-1), durationSeconds(-1) {}
};

enum TvStatus { TvPlaying, TvPaused, TvTransitioning, TvStopped, TvOther };

enum QueueAction { QueueNone, QueuePlayCurrent, QueueEnded };

class CastQueue {
public:
    CastQueue();

    void Reset(const CastQueueItem& first);
    QueueAction PlayNow(const CastQueueItem& item);
    void Add(const CastQueueItem& item);
    QueueAction PlayIndex(size_t index);
    QueueAction Next();
    bool Remove(size_t index);

    const std::vector<CastQueueItem>& Items() const;
    size_t Index() const;
    bool HasCurrent() const;
    CastQueueItem& Current();

    void Started(int64_t startSeconds);
    QueueAction OnPoll(TvStatus status, int64_t positionSeconds);
    int64_t LastPosition() const;
    bool HasPlayed() const;

private:
    void ResetDetection();
    void FinishSession();

    std::vector<CastQueueItem> items_;
    size_t index_;
    bool hasCurrent_;
    bool hasPlayed_;
    bool endReported_;
    int stoppedPolls_;
    int64_t lastPosition_;
};

} // namespace PerleCast

#endif
