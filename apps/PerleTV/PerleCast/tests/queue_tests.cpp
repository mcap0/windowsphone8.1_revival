#include "Test.h"
#include "../core/CastQueue.h"

namespace {
PerleCast::CastQueueItem Item(const std::string& url, int64_t duration)
{
    PerleCast::CastQueueItem item;
    item.videoUrl = url;
    item.title = url;
    item.durationSeconds = duration;
    return item;
}
}

int RunQueueTests()
{
    using namespace PerleCast;
    const int failuresBefore = TestFailures();

    CastQueue queue;
    CastQueueItem first = Item("video-1", 100);
    CastQueueItem second = Item("video-2", 120);
    CastQueueItem third = Item("video-3", 90);
    queue.Reset(first);
    queue.Add(second);
    queue.Add(second);
    queue.Add(third);
    Equal(static_cast<size_t>(3), queue.Items().size(), "Add appends items and ignores duplicate pending URL");

    CastQueueItem now = Item("video-now", 80);
    Equal(QueuePlayCurrent, queue.PlayNow(now), "PlayNow selects and plays the inserted item");
    Equal(static_cast<size_t>(1), queue.Index(), "PlayNow makes the inserted item current");
    Equal(std::string("video-now"), queue.Current().videoUrl, "PlayNow inserts immediately after current");
    Equal(std::string("video-2"), queue.Items()[2].videoUrl, "PlayNow preserves pending order after insertion");
    Equal(QueueNone, queue.PlayNow(now), "PlayNow of current URL is a no-op");

    Equal(QueueNone, queue.PlayIndex(queue.Items().size()), "PlayIndex rejects an out-of-range index");
    Equal(QueueNone, queue.PlayIndex(queue.Index()), "PlayIndex of current item is a no-op");
    Equal(QueuePlayCurrent, queue.PlayIndex(0), "PlayIndex selects a valid item");
    Equal(static_cast<size_t>(0), queue.Index(), "PlayIndex updates the current index");
    Equal(QueuePlayCurrent, queue.Next(), "Next selects the following item");
    Equal(std::string("video-now"), queue.Current().videoUrl, "Next selects the expected item");
    Equal(QueuePlayCurrent, queue.PlayIndex(queue.Items().size() - 1), "PlayIndex can select the last item");
    Equal(QueueNone, queue.Next(), "Next at the end returns QueueNone");

    queue.Reset(first);
    queue.Add(second);
    queue.Add(third);
    queue.PlayIndex(1);
    Check(!queue.Remove(queue.Index()), "Remove refuses to remove the current item");
    Check(queue.Remove(0), "Remove can remove an item before current");
    Equal(static_cast<size_t>(0), queue.Index(), "Remove before current adjusts the index");
    Equal(std::string("video-2"), queue.Current().videoUrl, "Remove before current preserves the current item");
    Check(queue.Remove(1), "Remove can remove an item after current");
    Equal(static_cast<size_t>(1), queue.Items().size(), "Remove after current preserves queue size");
    Equal(static_cast<size_t>(0), queue.Index(), "Remove after current leaves index unchanged");

    queue.Reset(first);
    queue.Add(second);
    queue.Started(0);
    Equal(QueueNone, queue.OnPoll(TvStopped, -1), "STOPPED before playback is ignored");
    Check(!queue.HasPlayed(), "STOPPED before playback does not mark the item played");
    Equal(QueueNone, queue.OnPoll(TvPlaying, 30), "PLAYING poll does not end the item");
    Check(queue.HasPlayed(), "PLAYING marks the item as played");
    Equal(static_cast<int64_t>(30), queue.LastPosition(), "LastPosition tracks playing position");
    Equal(QueueNone, queue.OnPoll(TvPaused, 35), "PAUSED poll does not end the item");
    Equal(static_cast<int64_t>(35), queue.LastPosition(), "LastPosition tracks paused position");
    Equal(QueueNone, queue.OnPoll(TvPlaying, -1), "Unknown position leaves LastPosition unchanged");
    Equal(static_cast<int64_t>(35), queue.LastPosition(), "LastPosition ignores unknown position");
    Equal(QueueNone, queue.OnPoll(TvStopped, -1), "One STOPPED poll alone does nothing");
    queue.OnPoll(TvPlaying, 92);
    Equal(QueueNone, queue.OnPoll(TvStopped, -1), "First STOPPED poll at the end is deferred");
    Equal(QueuePlayCurrent, queue.OnPoll(TvStopped, -1), "End of video advances to the next item");
    Equal(std::string("video-2"), queue.Current().videoUrl, "End detection selects the next item");
    Check(!queue.HasPlayed(), "Advancing resets played detection");

    queue.Reset(second);
    queue.Started(0);
    queue.OnPoll(TvPlaying, 110);
    Equal(QueueNone, queue.OnPoll(TvStopped, -1), "First final STOPPED poll is deferred");
    Equal(QueueEnded, queue.OnPoll(TvStopped, -1), "Stopping at the end of the last item ends the queue");
    Equal(QueueNone, queue.OnPoll(TvStopped, -1), "QueueEnded is returned only once");

    queue.Reset(first);
    queue.Started(0);
    queue.OnPoll(TvPlaying, 30);
    Equal(QueueNone, queue.OnPoll(TvStopped, -1), "First middle-stop poll is deferred");
    Equal(QueueEnded, queue.OnPoll(TvStopped, -1), "Stopping in the middle ends the session");

    queue.Reset(first);
    queue.Started(12);
    Check(!queue.HasPlayed(), "Started resets hasPlayed");
    Equal(static_cast<int64_t>(12), queue.LastPosition(), "Started records the requested starting position");
    queue.OnPoll(TvPlaying, 18);
    queue.Started(5);
    Check(!queue.HasPlayed(), "Started clears previous playback detection");
    Equal(static_cast<int64_t>(5), queue.LastPosition(), "Started resets the saved position");

    // the queue ended at the end of the video: the session stays on the TV and takes the next video
    queue.Reset(first);
    queue.Started(0);
    queue.OnPoll(TvPlaying, 95);
    queue.OnPoll(TvStopped, -1);
    Equal(QueueEnded, queue.OnPoll(TvStopped, -1), "Stopping at the end of the only item ends the queue");
    Equal(QueuePlayCurrent, queue.PlayNow(first), "After the end, PlayNow of the same URL plays it again");
    Equal(QueueNone, queue.PlayNow(first), "Playing again, the same URL is a no-op");
    queue.Started(0);
    queue.OnPoll(TvPlaying, 95);
    queue.OnPoll(TvStopped, -1);
    queue.OnPoll(TvStopped, -1);
    Equal(QueuePlayCurrent, queue.PlayIndex(queue.Index()), "After the end, PlayIndex of the current item replays it");
    queue.Started(0);
    queue.OnPoll(TvPlaying, 95);
    queue.OnPoll(TvStopped, -1);
    queue.OnPoll(TvStopped, -1);
    Equal(QueuePlayCurrent, queue.PlayNow(second), "After the end, PlayNow of another URL plays it");
    Equal(std::string("video-2"), queue.Current().videoUrl, "The new video is current");

    return TestFailures() - failuresBefore;
}
