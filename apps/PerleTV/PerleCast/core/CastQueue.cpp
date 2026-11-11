#include "CastQueue.h"

namespace PerleCast {

namespace {
const int StoppedPollsBeforeEnd = 2;
const int64_t EndMarginSeconds = 15;
}

CastQueue::CastQueue()
    : index_(0),
      hasCurrent_(false),
      hasPlayed_(false),
      endReported_(false),
      stoppedPolls_(0),
      lastPosition_(-1)
{
}

void CastQueue::Reset(const CastQueueItem& first)
{
    items_.clear();
    items_.push_back(first);
    index_ = 0;
    hasCurrent_ = true;
    ResetDetection();
    endReported_ = false;
}

QueueAction CastQueue::PlayNow(const CastQueueItem& item)
{
    if (hasCurrent_ && items_[index_].videoUrl == item.videoUrl) {
        if (!endReported_)
            return QueueNone;
        // the queue ended on this video and the session waits on the TV: the same video plays again
        items_[index_] = item;
        ResetDetection();
        endReported_ = false;
        return QueuePlayCurrent;
    }

    if (!hasCurrent_) {
        items_.push_back(item);
        index_ = items_.size() - 1;
        hasCurrent_ = true;
    } else {
        const size_t insertAt = index_ + 1;
        items_.insert(items_.begin() + insertAt, item);
        index_ = insertAt;
    }

    ResetDetection();
    endReported_ = false;
    return QueuePlayCurrent;
}

void CastQueue::Add(const CastQueueItem& item)
{
    const size_t firstPending = hasCurrent_ ? index_ + 1 : 0;
    for (size_t i = firstPending; i < items_.size(); ++i) {
        if (items_[i].videoUrl == item.videoUrl)
            return;
    }
    items_.push_back(item);
}

QueueAction CastQueue::PlayIndex(size_t index)
{
    if (!hasCurrent_ || index >= items_.size() || (index == index_ && !endReported_))
        return QueueNone;

    index_ = index;
    ResetDetection();
    endReported_ = false;
    return QueuePlayCurrent;
}

QueueAction CastQueue::Next()
{
    if (!hasCurrent_ || index_ + 1 >= items_.size())
        return QueueNone;

    ++index_;
    ResetDetection();
    endReported_ = false;
    return QueuePlayCurrent;
}

bool CastQueue::Remove(size_t index)
{
    if (index >= items_.size() || (hasCurrent_ && index == index_))
        return false;

    items_.erase(items_.begin() + index);
    if (hasCurrent_ && index < index_)
        --index_;
    return true;
}

const std::vector<CastQueueItem>& CastQueue::Items() const
{
    return items_;
}

size_t CastQueue::Index() const
{
    return index_;
}

bool CastQueue::HasCurrent() const
{
    return hasCurrent_ && index_ < items_.size();
}

CastQueueItem& CastQueue::Current()
{
    return items_[index_];
}

void CastQueue::ResetDetection()
{
    hasPlayed_ = false;
    stoppedPolls_ = 0;
    lastPosition_ = -1;
}

void CastQueue::FinishSession()
{
    stoppedPolls_ = 0;
    endReported_ = true;
}

void CastQueue::Started(int64_t startSeconds)
{
    ResetDetection();
    lastPosition_ = startSeconds;
    endReported_ = false;
}

QueueAction CastQueue::OnPoll(TvStatus status, int64_t positionSeconds)
{
    if (endReported_)
        return QueueNone;

    if (status == TvPlaying || status == TvPaused) {
        hasPlayed_ = true;
        stoppedPolls_ = 0;
        if (positionSeconds >= 0)
            lastPosition_ = positionSeconds;
        return QueueNone;
    }

    if (status != TvStopped) {
        stoppedPolls_ = 0;
        return QueueNone;
    }

    if (!hasPlayed_)
        return QueueNone;

    ++stoppedPolls_;
    if (stoppedPolls_ < StoppedPollsBeforeEnd)
        return QueueNone;

    const int64_t duration = HasCurrent() ? items_[index_].durationSeconds : -1;
    const bool ended = duration <= 0 ||
        lastPosition_ >= duration - EndMarginSeconds;

    if (ended && HasCurrent() && index_ + 1 < items_.size()) {
        ++index_;
        ResetDetection();
        endReported_ = false;
        return QueuePlayCurrent;
    }

    FinishSession();
    return QueueEnded;
}

int64_t CastQueue::LastPosition() const
{
    return lastPosition_;
}

bool CastQueue::HasPlayed() const
{
    return hasPlayed_;
}

} // namespace PerleCast
