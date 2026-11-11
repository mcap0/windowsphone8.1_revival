#include "CastSession.h"
#include "CastLog.h"
#include "TaskMessages.h"
#include "WinRtText.h"

#include <algorithm>
#include <cstdlib>

#include <windows.storage.h>

using namespace Windows::Storage;

namespace PerleCast {

namespace {

const int64 EndMarginSeconds = 15;

// The positions are shared with the phone player (Positions in AppState.cs): "pos:<url>" = seconds as text.
int64 SavedPosition(const std::string& videoUrl)
{
    try
    {
        Platform::Object^ value = ApplicationData::Current->LocalSettings->Values->Lookup(FromUtf8("pos:" + videoUrl));
        Platform::String^ text = dynamic_cast<Platform::String^>(value);
        return text == nullptr ? 0 : _wtoi64(text->Data());
    }
    catch (Platform::Exception^) { return 0; }
}

void SavePosition(const std::string& videoUrl, int64 seconds)
{
    if (videoUrl.empty() || seconds < 0) return;
    try
    {
        ApplicationData::Current->LocalSettings->Values->Insert(
            FromUtf8("pos:" + videoUrl), FromUtf8(std::to_string(seconds)));
    }
    catch (Platform::Exception^) { }
}

void Log(const std::string& text)
{
    CastLogLine(text);
}

TvStatus ToTvStatus(CastStatus status)
{
    switch (status)
    {
    case Playing: return TvPlaying;
    case Paused: return TvPaused;
    case Buffering: return TvTransitioning;
    case Stopped: return TvStopped;
    default: return TvOther;
    }
}

} // namespace

CastSession::CastSession()
    : worker_(concurrency::create_task([]() {})), active_(false), pollInFlight_(false), cacheLimit_(8),
      network_(nullptr), controller_(nullptr), server_(nullptr),
      startOffset_(0), duration_(-1), position_(0), polls_(0), failedPolls_(0), resumes_(0), maxHeight_(1080)
{
}

void CastSession::Run(std::function<void()> action)
{
    worker_ = worker_.then([action]()
    {
        try { action(); }
        catch (Platform::Exception^) { }
        catch (...) { }
    }, concurrency::task_continuation_context::use_arbitrary());
}

void CastSession::PollSoon()
{
    if (!active_ || pollInFlight_) return;
    pollInFlight_ = true;
    Run([this]()
    {
        Poll();
        pollInFlight_ = false;
    });
}

void CastSession::Dispatch(const std::wstring& command, const std::string& data)
{
    Unpacker in(data);
    CastSession* s = this;
    if (command == L"start")
    {
        DlnaDevice device = in.Device();
        CastQueueItem item = in.Item();
        Run([s, device, item]() { s->StartSession(device, item); });
    }
    else if (command == L"enqueue")
    {
        CastQueueItem item = in.Item();
        bool playNow = in.Int() != 0;
        Run([s, item, playNow]() { s->Enqueue(item, playNow); });
    }
    else if (command == L"playindex") { size_t i = static_cast<size_t>(in.Int()); Run([s, i]() { s->PlayIndex(i); }); }
    else if (command == L"next") Run([s]() { s->Next(); });
    else if (command == L"remove") { size_t i = static_cast<size_t>(in.Int()); Run([s, i]() { s->Remove(i); }); }
    else if (command == L"track") { size_t i = static_cast<size_t>(in.Int()); Run([s, i]() { s->SetAudio(i); }); }
    else if (command == L"subtitle") { int i = static_cast<int>(in.Int()); Run([s, i]() { s->SetSubtitle(i); }); }
    else if (command == L"quality") { int h = static_cast<int>(in.Int()); Run([s, h]() { s->SetQuality(h); }); }
    else if (command == L"seek") { int64 at = in.Int(); Run([s, at]() { s->Seek(at); }); }
    else if (command == L"seekto")
    {
        // the -30 / +30 squares: only the last of the taps in a row restarts the stream
        int64 at = in.Int();
        long request = InterlockedIncrement(&seekRequest_);
        Run([s, at, request]() { if (request == s->seekRequest_) s->Seek(at); });
    }
    else if (command == L"playpause") Run([s]() { s->PlayPause(); });
    else if (command == L"volume") { int amount = static_cast<int>(in.Int()); Run([s, amount]() { s->ChangeVolume(amount); }); }
    else if (command == L"stop") Run([s]() { s->EndSession(true, true); });
}

void CastSession::SetBackground(bool background)
{
    cacheLimit_ = background ? 4 : 8;
    Run([this]()
    {
        if (server_ != nullptr) server_->SetCacheLimit(cacheLimit_);
    });
}


void CastSession::StartSession(const DlnaDevice& device, const CastQueueItem& item)
{
    EndSession(true, false);
    deviceName_ = device.friendlyName;
    network_ = new Net();
    controller_ = new CastController(*network_, device);
    queue_.Reset(item);
    polls_ = 0;
    failedPolls_ = 0;
    active_ = true;

    if (!PlayCurrent(-1))
    {
        std::string error = error_;
        EndSession(false, false);
        if (Started) Started(false, error);
        return;
    }
    if (Started) Started(true, std::string());
}

// Sends the current video of the queue to the TV; start -1 = the item's start or the saved position.
bool CastSession::PlayCurrent(int64 start)
{
    if (controller_ == nullptr || !queue_.HasCurrent()) return false;
    CastQueueItem& item = queue_.Current();
    if (start < 0) start = item.startSeconds >= 0 ? item.startSeconds : SavedPosition(item.videoUrl);
    item.startSeconds = -1;  // played again later, it continues from the saved position
    resumes_ = 0;
    error_.clear();
    position_ = start;
    startOffset_ = start;
    Publish("Caricamento sul TV\xE2\x80\xA6");

    // tvlab: a new address while the TV still holds a /v.ts of the old server wedges it ("Transition not
    // available" for good). First the old server and all its connections, then Stop, then the new address
    // (seek, subtitles, audio, quality, next video; not the very first start)
    if (server_ != nullptr)
    {
        server_->Close();
        try { controller_->Stop(); } catch (...) { }
    }
    try
    {
        if (server_ != nullptr) server_->Close();
        server_ = ref new StreamServer();
        server_->SetMaxHeight(maxHeight_);
    server_->SetCacheLimit(cacheLimit_);
        Platform::String^ local = server_->Start(
            FromUtf8(item.hlsUrl), FromUtf8(item.audioLanguage), FromUtf8(item.subtitleUrl));
        localUrl_ = ToUtf8(local);
        duration_ = server_->DurationSeconds();
        item.durationSeconds = duration_ > 0 ? duration_ : -1;
        if (duration_ > 0 && start >= duration_ - EndMarginSeconds) start = 0;

        media_ = CastMedia();
        media_.title = item.title;
        media_.uploader = item.uploader;
        media_.mimeType = "video/mp2t";
        media_.durationSeconds = item.durationSeconds;
        if (!item.subtitleUrl.empty())
        {
            media_.subtitleUrl = localUrl_ + "/s.srt";  // StartAt adds the start, the cues follow the stream
            media_.dlnaFeatures = Didl::DefaultFeatures;
        }
    }
    catch (Platform::Exception^ ex)
    {
        error_ = ToUtf8(ex->Message);
        Log("prepare failed for \"" + item.title + "\": " + error_);
        Publish(error_);
        return false;
    }

    bool started = StartAt(start);
    if (!started) error_ = controller_->State().error;
    Log("play \"" + item.title + "\" from " + std::to_string(start) + "s of " + std::to_string(duration_) +
        "s, " + std::to_string(server_->Height()) + "p" + (started ? "" : ", FAILED: " + error_));
    Publish(started ? "Caricamento sul TV\xE2\x80\xA6" : error_);
    return started;
}

// The stream cannot seek by bytes: a new /v.ts starting at the wanted second replaces the current one.
bool CastSession::StartAt(int64 seconds)
{
    if (duration_ > 0 && seconds >= duration_) seconds = (std::max<int64>)(0, duration_ - 5);
    startOffset_ = seconds;
    position_ = seconds;
    media_.url = localUrl_ + "/v.ts?start=" + std::to_string(seconds);
    if (!media_.subtitleUrl.empty())
        media_.subtitleUrl = localUrl_ + "/s.srt?start=" + std::to_string(seconds);
    // the TV answers 701 ("Transition not available") while it is finishing the previous address: the
    // address again until it takes it (retrying only Play played the OLD address: the seek was lost), then
    // Play again until it starts (701 for ~2 s while it opens the stream). Measured with tools/tvlab.
    bool started = false;
    int uriTries = 0, playTries = 0;
    for (; uriTries < 15; ++uriTries)
    {
        if (controller_->SetUri(media_)) { started = true; break; }
        if (controller_->State().errorCode != 701) break;
        concurrency::wait(1000);
    }
    if (started)
    {
        started = false;
        for (; playTries < 15; ++playTries)
        {
            if (controller_->Play()) { started = true; break; }
            if (controller_->State().errorCode != 701) break;
            concurrency::wait(1000);
        }
    }
    if (uriTries > 0 || playTries > 0)
        Log("tv took the address after " + std::to_string(uriTries + 1) + " tries, play after " +
            std::to_string(playTries + 1));
    queue_.Started(seconds);
    return started;
}

int64 CastSession::CurrentPosition()
{
    return queue_.LastPosition() >= 0 ? queue_.LastPosition() : position_;
}

void CastSession::SaveProgress()
{
    if (queue_.HasCurrent() && queue_.HasPlayed())
        SavePosition(queue_.Current().videoUrl, CurrentPosition());
}

void CastSession::EndSession(bool stopTv, bool notify)
{
    bool wasActive = active_;
    if (wasActive) SaveProgress();
    active_ = false;
    if (stopTv && controller_ != nullptr)
    {
        try { controller_->Stop(); }
        catch (...) { }
    }
    if (server_ != nullptr) { server_->Close(); server_ = nullptr; }
    delete controller_;
    controller_ = nullptr;
    delete network_;
    network_ = nullptr;
    if (!notify || !wasActive) return;
    if (Ended) Ended();
}

void CastSession::Enqueue(const CastQueueItem& item, bool playNow)
{
    if (!active_) return;
    if (playNow)
    {
        SaveProgress();
        if (queue_.PlayNow(item) == QueuePlayCurrent) PlayCurrent(-1);
    }
    else
    {
        queue_.Add(item);
        Publish(std::string());
    }
}

void CastSession::PlayIndex(size_t index)
{
    if (!active_) return;
    SaveProgress();
    if (queue_.PlayIndex(index) == QueuePlayCurrent) PlayCurrent(-1);
}

void CastSession::Next()
{
    if (!active_) return;
    SaveProgress();
    if (queue_.Next() == QueuePlayCurrent) PlayCurrent(-1);
}

void CastSession::Remove(size_t index)
{
    if (queue_.Remove(index)) Publish(std::string());
}

void CastSession::SetAudio(size_t index)
{
    if (!active_ || server_ == nullptr || !queue_.HasCurrent()) return;
    const std::vector<StreamAudioOption>& options = server_->AudioOptions();
    if (index >= options.size()) return;
    std::string language = options[index].language;
    queue_.Current().audioLanguage = language;
    if (!language.empty())
    {
        try
        {
            ApplicationData::Current->LocalSettings->Values->Insert(
                ref new Platform::String(L"audioLang"), FromUtf8(language));
        }
        catch (Platform::Exception^) { }
    }
    PlayCurrent(CurrentPosition());
}

void CastSession::SetSubtitle(int index)
{
    if (!active_ || !queue_.HasCurrent()) return;
    CastQueueItem& item = queue_.Current();
    item.subtitleUrl = index >= 0 && static_cast<size_t>(index) < item.subtitles.size()
        ? item.subtitles[index].second : std::string();
    PlayCurrent(CurrentPosition());
}

void CastSession::SetQuality(int height)
{
    if (!active_) return;
    maxHeight_ = height;
    PlayCurrent(CurrentPosition());
}

void CastSession::Seek(int64 seconds)
{
    if (!active_) return;
    // fresh playlists like the phone player's reopen: the segment URLs read at the start expire (HTTP 403)
    PlayCurrent(seconds);
}

void CastSession::PlayPause()
{
    if (!active_ || controller_ == nullptr) return;
    if (controller_->State().status == Playing) controller_->Pause();
    else controller_->Play();
    Publish(std::string());
}

void CastSession::ChangeVolume(int amount)
{
    if (!active_ || controller_ == nullptr) return;
    int volume = 0;
    if (controller_->GetVolume(volume))
        controller_->SetVolume((std::max)(0, (std::min)(100, volume + amount)));
}

void CastSession::Poll()
{
    if (!active_ || controller_ == nullptr) return;
    bool ok = false;
    try { ok = controller_->Poll(); }
    catch (...) { }
    CastState state = controller_->State();
    if (!ok)
    {
        Log("poll failed: " + state.error);
        if (++failedPolls_ >= 3)
            Publish(state.error.empty() ? "Televisore non raggiungibile" : state.error);
        return;
    }
    failedPolls_ = 0;

    int64 position = state.positionSeconds >= 0 ? startOffset_ + state.positionSeconds : -1;
    if (position >= 0 && (state.status == Playing || state.status == Paused)) position_ = position;
    QueueAction action = queue_.OnPoll(ToTvStatus(state.status), position);
    if (++polls_ % 5 == 0 && state.status == Playing) SaveProgress();
    if (polls_ % 5 == 0)
        Log("tv " + CastController::StatusText(state.status) + " at " + std::to_string(position) + "/" +
            std::to_string(duration_) + "s, streams " + std::to_string(server_ == nullptr ? 0 : server_->Streams()) +
            ", queue " + std::to_string(queue_.Index() + 1) + "/" + std::to_string(queue_.Items().size()));

    if (action == QueuePlayCurrent)
    {
        // the previous video ended: watched to the end, it starts again from the beginning next time
        size_t previous = queue_.Index() - 1;
        SavePosition(queue_.Items()[previous].videoUrl, 0);
        PlayCurrent(-1);
        return;
    }
    if (action == QueueEnded && duration_ > 0 && CurrentPosition() < duration_ - EndMarginSeconds && resumes_ < 3)
    {
        // the TV stopped in the middle (lost stream, its own hiccup): continue from there
        ++resumes_;
        Log("tv stopped at " + std::to_string(CurrentPosition()) + "s, resuming (" + std::to_string(resumes_) + "/3)");
        Publish("Ripresa sul TV\xE2\x80\xA6");
        if (!StartAt(CurrentPosition())) error_ = controller_->State().error;
        return;
    }
    if (action == QueueEnded)
    {
        Log("tv stopped at " + std::to_string(CurrentPosition()) + "/" + std::to_string(duration_) + "s: session ends");
        if (queue_.HasCurrent() && duration_ > 0 && CurrentPosition() >= duration_ - EndMarginSeconds)
            SavePosition(queue_.Current().videoUrl, 0);
        else
            SaveProgress();
        EndSession(false, true);
        return;
    }
    Publish(error_.empty() ? CastController::StatusText(state.status) : error_);
}

// Sends the state of the session to the UI.
void CastSession::Publish(const std::string& status)
{
    ViewState view;
    view.device = deviceName_;
    view.status = status;
    view.duration = duration_;
    view.position = position_;
    view.playing = controller_ != nullptr && controller_->State().status == Playing;
    std::string url;
    if (queue_.HasCurrent())
    {
        const CastQueueItem& item = queue_.Current();
        url = item.videoUrl;
        view.title = item.title;
        view.uploader = item.uploader;
        for (size_t i = 0; i < item.subtitles.size(); ++i)
        {
            view.subtitleLabels.push_back(item.subtitles[i].first);
            if (!item.subtitleUrl.empty() && item.subtitles[i].second == item.subtitleUrl)
                view.subtitleIndex = static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < queue_.Items().size(); ++i)
        view.queueTitles.push_back(queue_.Items()[i].title);
    view.queueIndex = queue_.Index();
    if (server_ != nullptr)
    {
        const std::vector<StreamAudioOption>& options = server_->AudioOptions();
        for (size_t i = 0; i < options.size(); ++i)
        {
            std::string name = options[i].name.empty() ? "Traccia audio" : options[i].name;
            if (!options[i].language.empty() && options[i].language != name)
                name += " (" + options[i].language + ")";
            view.audioNames.push_back(name);
        }
        view.audioIndex = server_->AudioIndex();
        view.heights = server_->Heights();
        view.height = server_->Height();
    }

    if (Published) Published(view, url);
}

} // namespace PerleCast
