#ifndef PERLECAST_CASTSESSION_H
#define PERLECAST_CASTSESSION_H

#include "Net.h"
#include "StreamServer.h"
#include "core/CastController.h"
#include "core/CastQueue.h"
#include "core/DlnaDevice.h"

#include <ppltasks.h>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace PerleCast {

// What the remote shows of the session; sent from the background task to the app.
struct ViewState
{
    std::string title;
    std::string uploader;
    std::string device;
    std::string status;
    int64 duration;
    int64 position;
    bool playing;
    std::vector<std::string> queueTitles;
    size_t queueIndex;
    std::vector<std::string> audioNames;
    size_t audioIndex;
    std::vector<std::string> subtitleLabels;
    int subtitleIndex;
    std::vector<int> heights;
    int height;
    ViewState() : duration(-1), position(0), playing(false), queueIndex(0), audioIndex(0), subtitleIndex(-1), height(0) {}

    // the parts that need the casting view to be built again
    std::string LayoutKey() const
    {
        std::ostringstream key;
        key << title << '|' << queueTitles.size() << '|' << queueIndex << '|' << audioNames.size() << '|'
            << audioIndex << '|' << subtitleLabels.size() << '|' << subtitleIndex << '|' << device << '|' << height
            << '|' << heights.size();
        return key.str();
    }
};

// The casting session: the TV, the stream server and the queue. It lives in the background audio task (CastTask), so
// the TV keeps being fed while Perle TV is in the background; the app only shows it (CastUi).
class CastSession
{
public:
    CastSession();

    std::function<void(const ViewState&, const std::string&)> Published;  // the view and the video on the TV
    std::function<void(bool, const std::string&)> Started;                  // result of StartSession
    std::function<void()> Ended;                                            // ■, or the TV stopped for good

    // Runs action after the previous ones, on the worker.
    void Run(std::function<void()> action);
    // Timer tick: a poll of the TV, unless the previous one is still running.
    void PollSoon();
    // Fewer segments kept in memory while the app is not on screen.
    void SetBackground(bool background);
    bool Active() const { return active_; }
    // A command of the remote (TaskMessages: start, enqueue, seek, ...), queued on the worker.
    void Dispatch(const std::wstring& command, const std::string& data);
    volatile long seekRequest_ = 0;  // latest −30/+30 tap: older ones queued on the worker are dropped

    // ---- worker ----
    void StartSession(const DlnaDevice& device, const CastQueueItem& item);
    bool PlayCurrent(int64 start);
    bool StartAt(int64 seconds);
    int64 CurrentPosition();
    void SaveProgress();
    void EndSession(bool stopTv, bool notify);
    void Enqueue(const CastQueueItem& item, bool playNow);
    void PlayIndex(size_t index);
    void Next();
    void Remove(size_t index);
    void SetAudio(size_t index);
    void SetSubtitle(int index);
    void SetQuality(int height);
    void Seek(int64 seconds);
    void PlayPause();
    void ChangeVolume(int amount);
    void Poll();
    void Publish(const std::string& status);

private:
    concurrency::task<void> worker_;
    volatile bool active_;
    volatile bool pollInFlight_;
    volatile size_t cacheLimit_;
    Net* network_;
    CastController* controller_;
    StreamServer^ server_;
    CastQueue queue_;
    CastMedia media_;
    std::string localUrl_;
    std::string error_;
    std::string deviceName_;
    int64 startOffset_;  // where the current /v.ts starts: the TV counts from there
    int64 duration_;
    int64 position_;
    int polls_;
    int failedPolls_;
    int resumes_;        // automatic restarts of the current video after the TV stopped by itself
    int maxHeight_;      // chosen on the remote, kept for the next videos of the queue
};

} // namespace PerleCast

#endif
