#include "CastUi.h"
#include "Discovery.h"
#include "Net.h"
#include "StreamServer.h"
#include "WinRtText.h"
#include "core/CastController.h"
#include "core/CastQueue.h"
#include "CastLog.h"

#include <collection.h>
#include <ppltasks.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include <windows.applicationmodel.core.h>
#include <windows.foundation.h>
#include <windows.phone.ui.input.h>
#include <windows.storage.h>
#include <windows.system.display.h>
#include <windows.system.h>
#include <windows.web.http.h>
#include <windows.ui.h>
#include <windows.ui.core.h>
#include <windows.ui.xaml.h>
#include <windows.ui.xaml.controls.h>
#include <windows.ui.xaml.controls.primitives.h>
#include <windows.ui.xaml.input.h>
#include <windows.ui.xaml.media.h>

using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Phone::UI::Input;
using namespace Windows::Storage;
using namespace Windows::System::Display;
using namespace Windows::UI;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;

namespace PerleCast {

CastVideo::CastVideo()
{
    VideoUrl = "";
    Title = "";
    Uploader = "";
    HlsUrl = "";
    AudioLanguage = "";
    SubtitleUrl = "";
    StartSeconds = -1;
    SubtitleLabels = ref new Platform::Collections::Vector<Platform::String^>();
    SubtitleUrls = ref new Platform::Collections::Vector<Platform::String^>();
}

namespace {

const int64 EndMarginSeconds = 15;
const int IdleMinutes = 30;  // a finished session waits this long for the next video

// What the UI shows of the session; copied from the worker to the UI thread.
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

enum Screen { ScreenNone, ScreenDevices, ScreenChoice, ScreenCasting, ScreenAudio, ScreenSubtitles, ScreenQueue, ScreenQuality };

CastQueueItem ToItem(CastVideo^ video)
{
    CastQueueItem item;
    item.videoUrl = ToUtf8(video->VideoUrl);
    item.title = ToUtf8(video->Title);
    item.uploader = ToUtf8(video->Uploader);
    item.hlsUrl = ToUtf8(video->HlsUrl);
    item.audioLanguage = ToUtf8(video->AudioLanguage);
    item.subtitleUrl = ToUtf8(video->SubtitleUrl);
    item.startSeconds = video->StartSeconds < 0 ? -1 : static_cast<int64_t>(video->StartSeconds);
    if (video->SubtitleLabels != nullptr && video->SubtitleUrls != nullptr)
    {
        unsigned int count = (std::min)(video->SubtitleLabels->Size, video->SubtitleUrls->Size);
        for (unsigned int i = 0; i < count; ++i)
            item.subtitles.push_back(std::make_pair(ToUtf8(video->SubtitleLabels->GetAt(i)),
                                                    ToUtf8(video->SubtitleUrls->GetAt(i))));
    }
    return item;
}

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

ref class CastUiState sealed
{
internal:
    CastUiState()
        : popup_(nullptr), root_(nullptr), content_(nullptr), devicesPanel_(nullptr),
          statusText_(nullptr), positionText_(nullptr), slider_(nullptr), timer_(nullptr),
          displayRequest_(nullptr), screen_(ScreenNone), viewAt_(0), discovering_(false), pollInFlight_(false),
          sliderChanging_(false), backPressedRegistered_(false),
          worker_(concurrency::create_task([]() {})),
          active_(false), network_(nullptr), controller_(nullptr), server_(nullptr),
          startOffset_(0), duration_(-1), position_(0), polls_(0), failedPolls_(0), resumes_(0), maxHeight_(1080), idleSince_(0)
    {
    }

    void Show(CastVideo^ video)
    {
        CastQueueItem item = ToItem(video);
        OpenPopup();
        if (!active_)
        {
            pendingItem_ = item;
            BeginDiscovery();
        }
        else if (item.videoUrl == currentUrl_)
        {
            BuildCastingView();
        }
        else
        {
            BuildChoiceView(item);
        }
    }

    bool PlayOnTvIfCasting(CastVideo^ video)
    {
        if (!active_) return false;
        CastQueueItem item = ToItem(video);
        if (item.videoUrl == currentUrl_) return true;
        currentUrl_ = item.videoUrl;
        Run([this, item]() { Enqueue(item, true); });
        return true;
    }

    void AddToQueue(CastVideo^ video)
    {
        if (!active_) return;
        CastQueueItem item = ToItem(video);
        Run([this, item]() { Enqueue(item, false); });
    }

    void Reopen()
    {
        OpenPopup();
        if (active_) BuildCastingView();
        else if (!discovering_) BeginDiscovery();
    }

    bool IsActive()
    {
        return active_;
    }

    // ---- what the app's mini player shows while casting (UI thread) ----

    std::string TvTitle() { return view_.title; }
    std::string TvUploader() { return view_.uploader; }
    int64 TvDuration() { return view_.duration; }
    bool TvPlaying() { return active_ && view_.playing; }

    // the last position read from the TV, moved on by the time passed since then while it plays
    int64 TvPosition()
    {
        int64 position = view_.position;
        if (view_.playing) position += static_cast<int64>((GetTickCount64() - viewAt_) / 1000);
        if (view_.duration > 0) position = (std::min)(position, view_.duration);
        return position;
    }

    void TogglePlayPause()
    {
        if (!active_) return;
        // shown at once; the next poll of the TV corrects it
        view_.position = TvPosition();
        viewAt_ = GetTickCount64();
        view_.playing = !view_.playing;
        Run([this]() { PlayPause(); });
    }

private:
    // ---- UI thread -------------------------------------------------------------------------------

    Popup^ popup_;
    Grid^ root_;
    StackPanel^ content_;
    StackPanel^ devicesPanel_;
    TextBlock^ statusText_;
    TextBlock^ positionText_;
    Slider^ slider_;
    DispatcherTimer^ timer_;
    DisplayRequest^ displayRequest_;
    Screen screen_;
    std::vector<DlnaDevice> devices_;
    CastQueueItem pendingItem_;   // waiting for the choice of the TV
    ViewState view_;
    ULONGLONG viewAt_;            // when view_ was published
    std::string builtLayout_;
    std::string currentUrl_;      // the video on the TV, as known by the UI
    bool discovering_;
    bool pollInFlight_;
    bool sliderChanging_;
    bool backPressedRegistered_;
    Windows::Foundation::EventRegistrationToken backPressedToken_;

    // ---- worker (all the network work, in order) -------------------------------------------------

    concurrency::task<void> worker_;
    volatile bool active_;
    Net* network_;
    CastController* controller_;
    ULONGLONG idleSince_;         // the last video ended: the session waits on the TV for the next one (0 = not idle)
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
    volatile long seekRequest_ = 0;  // latest −30/+30 tap: older ones queued on the worker are dropped
    int resumes_;        // automatic restarts of the current video after the TV stopped by itself
    int maxHeight_;      // chosen on the remote, kept for the next videos of the queue

    void DispatchToUi(DispatchedHandler^ handler)
    {
        CoreApplication::MainView->CoreWindow->Dispatcher->RunAsync(
            CoreDispatcherPriority::Normal, handler);
    }

    // Runs action after the previous ones (call it on the UI thread).
    void Run(std::function<void()> action)
    {
        worker_ = worker_.then([action]()
        {
            try { action(); }
            catch (Platform::Exception^) { }
            catch (...) { }
        }, concurrency::task_continuation_context::use_arbitrary());
    }

    // ---- session (worker) ------------------------------------------------------------------------

    void StartSession(const DlnaDevice& device, const CastQueueItem& item)
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
            DispatchToUi(ref new DispatchedHandler([this, error]()
            {
                if (screen_ == ScreenDevices && statusText_ != nullptr)
                    statusText_->Text = FromUtf8(error);
            }));
            return;
        }

        DispatchToUi(ref new DispatchedHandler([this]()
        {
            if (displayRequest_ == nullptr)
            {
                displayRequest_ = ref new DisplayRequest();
                displayRequest_->RequestActive();
            }
            StartTimer();
            BuildCastingView();
            CastUi::RaiseActiveChanged(true);
        }));
    }

    // Sends the current video of the queue to the TV; start -1 = the item's start or the saved position.
    bool PlayCurrent(int64 start)
    {
        if (controller_ == nullptr || !queue_.HasCurrent()) return false;
        CastQueueItem& item = queue_.Current();
        if (start < 0) start = item.startSeconds >= 0 ? item.startSeconds : SavedPosition(item.videoUrl);
        item.startSeconds = -1;  // played again later, it continues from the saved position
        resumes_ = 0;
        if (idleSince_ != 0)
        {
            idleSince_ = 0;
            DispatchToUi(ref new DispatchedHandler([this]()
            {
                if (displayRequest_ == nullptr && active_)
                {
                    displayRequest_ = ref new DisplayRequest();
                    displayRequest_->RequestActive();
                }
            }));
        }
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
    bool StartAt(int64 seconds)
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

    int64 CurrentPosition()
    {
        return queue_.LastPosition() >= 0 ? queue_.LastPosition() : position_;
    }

    void SaveProgress()
    {
        // idle: the finished video was saved at 0 when it ended
        if (idleSince_ == 0 && queue_.HasCurrent() && queue_.HasPlayed())
            SavePosition(queue_.Current().videoUrl, CurrentPosition());
    }

    void EndSession(bool stopTv, bool notify)
    {
        bool wasActive = active_;
        if (wasActive) SaveProgress();
        active_ = false;
        idleSince_ = 0;
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

        DispatchToUi(ref new DispatchedHandler([this]()
        {
            currentUrl_.clear();
            if (displayRequest_ != nullptr)
            {
                displayRequest_->RequestRelease();
                displayRequest_ = nullptr;
            }
            if (timer_ != nullptr) timer_->Stop();
            ClosePopup();
            CastUi::RaiseActiveChanged(false);
        }));
    }

    void Enqueue(const CastQueueItem& item, bool playNow)
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

    void PlayIndex(size_t index)
    {
        if (!active_) return;
        SaveProgress();
        if (queue_.PlayIndex(index) == QueuePlayCurrent) PlayCurrent(-1);
    }

    void Next()
    {
        if (!active_) return;
        SaveProgress();
        if (queue_.Next() == QueuePlayCurrent) PlayCurrent(-1);
    }

    void Remove(size_t index)
    {
        if (queue_.Remove(index)) Publish(std::string());
    }

    void SetAudio(size_t index)
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

    void SetSubtitle(int index)
    {
        if (!active_ || !queue_.HasCurrent()) return;
        CastQueueItem& item = queue_.Current();
        item.subtitleUrl = index >= 0 && static_cast<size_t>(index) < item.subtitles.size()
            ? item.subtitles[index].second : std::string();
        PlayCurrent(CurrentPosition());
    }

    void SetQuality(int height)
    {
        if (!active_) return;
        maxHeight_ = height;
        PlayCurrent(CurrentPosition());
    }

    void Seek(int64 seconds)
    {
        if (!active_) return;
        // fresh playlists like the phone player's reopen: the segment URLs read at the start expire (HTTP 403)
        PlayCurrent(seconds);
    }

    void PlayPause()
    {
        if (!active_ || controller_ == nullptr) return;
        if (controller_->State().status == Playing) controller_->Pause();
        else controller_->Play();
        Publish(std::string());
    }

    void ChangeVolume(int amount)
    {
        if (!active_ || controller_ == nullptr) return;
        int volume = 0;
        if (controller_->GetVolume(volume))
            controller_->SetVolume((std::max)(0, (std::min)(100, volume + amount)));
    }

    // The queue ended at the end of a video. The session stays: a video opened in the app (or the same one again)
    // goes to the TV like during the play. No polling meanwhile (the TV sits in STOPPED); after IdleMinutes the
    // session ends by itself.
    void GoIdle()
    {
        if (server_ != nullptr) { server_->Close(); server_ = nullptr; }
        idleSince_ = GetTickCount64();
        Log("queue finished: session waits for the next video");
        Publish("Finito \xE2\x80\x94 apri un altro video per mandarlo al TV");
        DispatchToUi(ref new DispatchedHandler([this]()
        {
            if (displayRequest_ != nullptr)
            {
                displayRequest_->RequestRelease();  // nothing plays: the screen may turn off
                displayRequest_ = nullptr;
            }
        }));
    }

    void Poll()
    {
        if (!active_ || controller_ == nullptr) return;
        if (idleSince_ != 0)
        {
            if (GetTickCount64() - idleSince_ > IdleMinutes * 60000ULL)
            {
                Log("idle for " + std::to_string(IdleMinutes) + " min: session ends");
                EndSession(false, true);
            }
            return;
        }
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
            if (duration_ <= 0 || CurrentPosition() >= duration_ - EndMarginSeconds)
                GoIdle();  // watched to the end: the next video goes to the TV without choosing it again
            else
                EndSession(false, true);
            return;
        }
        Publish(error_.empty() ? CastController::StatusText(state.status) : error_);
    }

    // Sends the state of the session to the UI.
    void Publish(const std::string& status)
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

        DispatchToUi(ref new DispatchedHandler([this, view, url]()
        {
            bool keepStatus = view.status.empty();
            std::string status = view_.status;
            view_ = view;
            viewAt_ = GetTickCount64();
            if (keepStatus) view_.status = status;
            if (!url.empty()) currentUrl_ = url;
            Refresh();
        }));
    }

    // ---- UI --------------------------------------------------------------------------------------

    void OpenPopup()
    {
        if (popup_ == nullptr)
        {
            popup_ = ref new Popup();
            popup_->IsLightDismissEnabled = false;
            root_ = ref new Grid();
            root_->Background = ref new SolidColorBrush(Colors::Black);
            popup_->Child = root_;
            popup_->Opened += ref new EventHandler<Object^>(
                this, &CastUiState::OnPopupOpened);
            popup_->Closed += ref new EventHandler<Object^>(
                this, &CastUiState::OnPopupClosed);
        }

        Rect bounds = Window::Current->Bounds;
        root_->Width = bounds.Width;
        root_->Height = bounds.Height;
        popup_->IsOpen = true;
    }

    void OnPopupOpened(Object^, Object^)
    {
        if (!backPressedRegistered_)
        {
            backPressedToken_ = HardwareButtons::BackPressed +=
                ref new Windows::Foundation::EventHandler<BackPressedEventArgs^>(
                    this, &CastUiState::OnBackPressed);
            backPressedRegistered_ = true;
        }
    }

    void OnPopupClosed(Object^, Object^)
    {
        screen_ = ScreenNone;
        if (backPressedRegistered_)
        {
            HardwareButtons::BackPressed -= backPressedToken_;
            backPressedRegistered_ = false;
        }
    }

    void OnBackPressed(Object^, BackPressedEventArgs^ args)
    {
        if (popup_ == nullptr || !popup_->IsOpen) return;
        args->Handled = true;
        if (active_ && (screen_ == ScreenAudio || screen_ == ScreenSubtitles || screen_ == ScreenQueue || screen_ == ScreenQuality))
            BuildCastingView();
        else
            popup_->IsOpen = false;
    }

    void ClosePopup()
    {
        if (popup_ != nullptr) popup_->IsOpen = false;
    }

    void StartTimer()
    {
        if (timer_ == nullptr)
        {
            timer_ = ref new DispatcherTimer();
            TimeSpan interval;
            interval.Duration = 2 * 10000000;
            timer_->Interval = interval;
            timer_->Tick += ref new EventHandler<Object^>(
                this, &CastUiState::OnTimerTick);
        }
        timer_->Start();
    }

    void OnTimerTick(Object^, Object^)
    {
        if (!active_ || pollInFlight_) return;
        pollInFlight_ = true;
        Run([this]()
        {
            Poll();
            DispatchToUi(ref new DispatchedHandler([this]() { pollInFlight_ = false; }));
        });
    }

    void Refresh()
    {
        if (screen_ != ScreenCasting) return;
        if (view_.LayoutKey() != builtLayout_)
        {
            BuildCastingView();
            return;
        }
        UpdateCastingView();
    }

    void BeginDiscovery()
    {
        discovering_ = true;
        BuildDeviceView();

        concurrency::create_task([]()
        {
            return Discovery::FindRenderers();
        }).then([this](std::vector<DlnaDevice> result)
        {
            DispatchToUi(ref new DispatchedHandler([this, result]()
            {
                devices_ = result;
                discovering_ = false;
                if (popup_ == nullptr || !popup_->IsOpen || screen_ != ScreenDevices) return;
                ShowDevices();
            }));
        });
    }

    void BuildBase(const std::wstring& header)
    {
        root_->Children->Clear();
        ScrollViewer^ scroll = ref new ScrollViewer();
        scroll->VerticalScrollBarVisibility = ScrollBarVisibility::Auto;
        content_ = ref new StackPanel();
        content_->Margin = Thickness(18, 28, 18, 24);

        TextBlock^ heading = ref new TextBlock();
        heading->Text = ref new Platform::String(header.c_str());
        heading->FontSize = 28;
        heading->TextWrapping = TextWrapping::Wrap;
        heading->Margin = Thickness(0, 0, 0, 16);
        content_->Children->Append(heading);

        statusText_ = ref new TextBlock();
        statusText_->FontSize = 18;
        statusText_->TextWrapping = TextWrapping::Wrap;
        statusText_->Margin = Thickness(0, 4, 0, 16);
        content_->Children->Append(statusText_);

        scroll->Content = content_;
        root_->Children->Append(scroll);
        slider_ = nullptr;
        positionText_ = nullptr;
        devicesPanel_ = nullptr;
    }

    void BuildDeviceView()
    {
        screen_ = ScreenDevices;
        BuildBase(L"Trasmetti");
        statusText_->Text = ref new Platform::String(L"cerco dispositivi…");
        devicesPanel_ = ref new StackPanel();
        content_->Children->Append(devicesPanel_);
        AddText(content_, L"Tieni Perle TV aperta durante la trasmissione", 14, 0.65);
    }

    void ShowDevices()
    {
        if (devicesPanel_ == nullptr) return;
        devicesPanel_->Children->Clear();
        if (devices_.empty())
        {
            statusText_->Text = ref new Platform::String(L"nessun dispositivo trovato");
            AddTapText(devicesPanel_, L"riprova", [this]() { BeginDiscovery(); });
            return;
        }

        statusText_->Text = ref new Platform::String(L"Scegli un televisore");
        for (size_t i = 0; i < devices_.size(); ++i)
        {
            AddTapText(devicesPanel_, FromUtf8(devices_[i].Label()), [this, i]() { SelectDevice(i); });
        }
    }

    void SelectDevice(size_t index)
    {
        if (index >= devices_.size()) return;
        statusText_->Text = ref new Platform::String(L"Connessione al televisore…");
        DlnaDevice device = devices_[index];
        CastQueueItem item = pendingItem_;
        currentUrl_ = item.videoUrl;
        Run([this, device, item]() { StartSession(device, item); });
    }

    // While casting, another video: play it now, queue it, or the other choices.
    void BuildChoiceView(const CastQueueItem& item)
    {
        screen_ = ScreenChoice;
        BuildBase(L"Sul televisore");
        statusText_->Text = FromUtf8(view_.device);
        AddText(content_, FromUtf8(item.title), 20, 1);
        AddTapText(content_, L"riproduci ora sul TV", [this, item]()
        {
            currentUrl_ = item.videoUrl;
            Run([this, item]() { Enqueue(item, true); });
            BuildCastingView();
        });
        AddTapText(content_, L"aggiungi alla coda TV", [this, item]()
        {
            Run([this, item]() { Enqueue(item, false); });
            BuildCastingView();
        });
        AddTapText(content_, L"apri telecomando", [this]() { BuildCastingView(); });
        AddTapText(content_, L"altro televisore", [this, item]()
        {
            pendingItem_ = item;
            BeginDiscovery();
        });
        AddTapText(content_, L"annulla", [this]() { ClosePopup(); });
    }

    void BuildCastingView()
    {
        screen_ = ScreenCasting;
        builtLayout_ = view_.LayoutKey();
        BuildBase(L"Trasmissione");
        statusText_->Text = FromUtf8(view_.status.empty() ? std::string("Connessione\xE2\x80\xA6") : view_.status);

        AddText(content_, FromUtf8(view_.title), 20, 1);
        AddText(content_, FromUtf8(view_.device), 16, 0.75);

        StackPanel^ controls = ref new StackPanel();
        controls->Orientation = Orientation::Horizontal;
        controls->HorizontalAlignment = HorizontalAlignment::Center;
        controls->Margin = Thickness(0, 12, 0, 12);
        AddTapText(controls, L"−30", [this]() { SeekBy(-30); });
        AddTapText(controls, L"▶ / ❚❚", [this]() { Run([this]() { PlayPause(); }); });
        AddTapText(controls, L"■", [this]() { Run([this]() { EndSession(true, true); }); });
        AddTapText(controls, L"+30", [this]() { SeekBy(30); });
        content_->Children->Append(controls);
        if (view_.queueIndex + 1 < view_.queueTitles.size())
        {
            // own row: the four transport buttons already fill the screen width
            StackPanel^ next = ref new StackPanel();
            next->Orientation = Orientation::Horizontal;
            next->HorizontalAlignment = HorizontalAlignment::Center;
            AddTapText(next, L"Successivo", [this]() { Run([this]() { Next(); }); });
            content_->Children->Append(next);
        }

        slider_ = ref new Slider();
        slider_->Minimum = 0;
        slider_->Maximum = static_cast<double>(view_.duration > 0 ? view_.duration : 1);
        slider_->Margin = Thickness(0, 4, 0, 0);
        // the Slider handles its own pointer events: listen to the handled ones too
        slider_->AddHandler(UIElement::PointerPressedEvent, ref new PointerEventHandler(
            [this](Object^, PointerRoutedEventArgs^) { sliderChanging_ = true; }), true);
        slider_->AddHandler(UIElement::PointerReleasedEvent, ref new PointerEventHandler(
            [this](Object^, PointerRoutedEventArgs^) { SeekToSlider(); }), true);
        slider_->AddHandler(UIElement::PointerCaptureLostEvent, ref new PointerEventHandler(
            [this](Object^, PointerRoutedEventArgs^) { SeekToSlider(); }), true);
        content_->Children->Append(slider_);

        positionText_ = ref new TextBlock();
        positionText_->HorizontalAlignment = HorizontalAlignment::Center;
        content_->Children->Append(positionText_);

        StackPanel^ volume = ref new StackPanel();
        volume->Orientation = Orientation::Horizontal;
        volume->HorizontalAlignment = HorizontalAlignment::Center;
        AddTapText(volume, L"volume −", [this]() { Run([this]() { ChangeVolume(-5); }); });
        AddTapText(volume, L"volume +", [this]() { Run([this]() { ChangeVolume(5); }); });
        content_->Children->Append(volume);

        if (view_.heights.size() > 1)
            AddTapText(content_, FromUtf8("qualit\xC3\xA0: " + std::to_string(view_.height) + "p"), [this]() { BuildQualityView(); });
        if (view_.audioNames.size() > 1)
        {
            std::string current = view_.audioIndex < view_.audioNames.size() ? view_.audioNames[view_.audioIndex] : "";
            AddTapText(content_, FromUtf8("audio: " + current), [this]() { BuildAudioView(); });
        }
        if (!view_.subtitleLabels.empty())
        {
            std::string current = view_.subtitleIndex >= 0 ? view_.subtitleLabels[view_.subtitleIndex] : "nessuno";
            AddTapText(content_, FromUtf8("sottotitoli: " + current), [this]() { BuildSubtitleView(); });
        }
        AddTapText(content_, FromUtf8("coda TV (" + std::to_string(view_.queueTitles.size()) + ")"),
            [this]() { BuildQueueView(); });

        AddTapText(content_, L"chiudi", [this]() { ClosePopup(); });
        AddText(content_, L"tieni Perle TV aperta durante la trasmissione", 14, 0.65);
        UpdateCastingView();
    }

    void UpdateCastingView()
    {
        if (!view_.status.empty() && statusText_ != nullptr) statusText_->Text = FromUtf8(view_.status);
        if (slider_ != nullptr && !sliderChanging_)
        {
            if (view_.duration > 0) slider_->Maximum = static_cast<double>(view_.duration);
            slider_->Value = (std::min)(slider_->Maximum, static_cast<double>((std::max<int64>)(0, view_.position)));
        }
        if (positionText_ != nullptr)
            positionText_->Text = FromUtf8(FormatSeconds(view_.position) + " / " + FormatSeconds(view_.duration));
    }

    void BuildAudioView()
    {
        screen_ = ScreenAudio;
        BuildBase(L"Traccia audio");
        for (size_t i = 0; i < view_.audioNames.size(); ++i)
        {
            std::string label = (i == view_.audioIndex ? "\xE2\x9C\x93 " : "") + view_.audioNames[i];
            AddTapText(content_, FromUtf8(label), [this, i]()
            {
                Run([this, i]() { SetAudio(i); });
                BuildCastingView();
            });
        }
        AddTapText(content_, L"indietro", [this]() { BuildCastingView(); });
    }

    void BuildQualityView()
    {
        screen_ = ScreenQuality;
        BuildBase(L"Qualità sul TV");
        for (size_t i = 0; i < view_.heights.size(); ++i)
        {
            int height = view_.heights[i];
            std::string label = (height == view_.height ? "\xE2\x9C\x93 " : "") + std::to_string(height) + "p";
            AddTapText(content_, FromUtf8(label), [this, height]()
            {
                Run([this, height]() { SetQuality(height); });
                BuildCastingView();
            });
        }
        AddTapText(content_, L"indietro", [this]() { BuildCastingView(); });
    }

    void BuildSubtitleView()
    {
        screen_ = ScreenSubtitles;
        BuildBase(L"Sottotitoli");
        for (int i = -1; i < static_cast<int>(view_.subtitleLabels.size()); ++i)
        {
            std::string label = (i == view_.subtitleIndex ? "\xE2\x9C\x93 " : "") +
                (i < 0 ? std::string("nessuno") : view_.subtitleLabels[i]);
            AddTapText(content_, FromUtf8(label), [this, i]()
            {
                Run([this, i]() { SetSubtitle(i); });
                BuildCastingView();
            });
        }
        AddTapText(content_, L"indietro", [this]() { BuildCastingView(); });
    }

    void BuildQueueView()
    {
        screen_ = ScreenQueue;
        BuildBase(L"Coda TV");
        statusText_->Text = ref new Platform::String(
            L"tocca un video per guardarlo; per aggiungerne apri un video e premi trasmetti");
        for (size_t i = 0; i < view_.queueTitles.size(); ++i)
        {
            bool current = i == view_.queueIndex;
            std::string label = (current ? "\xE2\x96\xB6 " : std::to_string(i + 1) + ". ") + view_.queueTitles[i];
            AddTapText(content_, FromUtf8(label), [this, i, current]()
            {
                if (!current) Run([this, i]() { PlayIndex(i); });
                BuildCastingView();
            });
            if (!current)
            {
                AddTapText(content_, L"   rimuovi", [this, i]()
                {
                    view_.queueTitles.erase(view_.queueTitles.begin() + i);
                    if (i < view_.queueIndex) --view_.queueIndex;
                    Run([this, i]() { Remove(i); });
                    BuildQueueView();
                }, 0.6);
            }
        }
        AddTapText(content_, L"indietro", [this]() { BuildCastingView(); });
    }

    void SeekToSlider()
    {
        if (!sliderChanging_) return;
        sliderChanging_ = false;
        if (!active_ || slider_ == nullptr) return;
        int64 seconds = static_cast<int64>(slider_->Value);
        view_.position = seconds;
        statusText_->Text = ref new Platform::String(L"Caricamento sul TV…");
        Run([this, seconds]() { Seek(seconds); });
    }

    // the −30 / +30 squares: taps in a row add up on the position shown, only the last one restarts the stream
    void SeekBy(int64 delta)
    {
        if (!active_) return;
        int64 seconds = (std::max<int64>)(0, view_.position + delta);
        if (view_.duration > 0) seconds = (std::min)(seconds, view_.duration - 5);
        view_.position = seconds;
        UpdateCastingView();
        statusText_->Text = ref new Platform::String(L"Caricamento sul TV…");
        long request = InterlockedIncrement(&seekRequest_);
        Run([this, seconds, request]() { if (request == seekRequest_) Seek(seconds); });
    }

    static std::string FormatSeconds(int64 seconds)
    {
        if (seconds < 0) seconds = 0;
        std::ostringstream text;
        text << (seconds / 60) << ":";
        if ((seconds % 60) < 10) text << "0";
        text << (seconds % 60);
        return text.str();
    }

    void AddText(StackPanel^ panel, Platform::String^ text, double size, double opacity)
    {
        TextBlock^ block = ref new TextBlock();
        block->Text = text;
        block->FontSize = size;
        block->Opacity = opacity;
        block->TextWrapping = TextWrapping::Wrap;
        block->Margin = Thickness(0, 10, 0, 8);
        panel->Children->Append(block);
    }

    void AddTapText(StackPanel^ panel, Platform::String^ text,
                    std::function<void()> action, double opacity = 1)
    {
        Border^ border = ref new Border();
        border->Background = ref new SolidColorBrush(ColorHelper::FromArgb(255, 205, 32, 31));
        border->Padding = Thickness(18, 12, 18, 12);
        border->Margin = Thickness(0, 5, 8, 5);
        border->Opacity = opacity;

        TextBlock^ label = ref new TextBlock();
        label->Text = text;
        label->FontSize = 18;
        label->TextWrapping = TextWrapping::Wrap;
        border->Child = label;
        border->Tapped += ref new TappedEventHandler(
            [action](Object^, TappedRoutedEventArgs^) { action(); });
        panel->Children->Append(border);
    }
};

static CastUiState^ g_state = nullptr;

static CastUiState^ State()
{
    if (g_state == nullptr) g_state = ref new CastUiState();
    return g_state;
}

void CastUi::Show(CastVideo^ video)
{
    if (video == nullptr) return;
    State()->Show(video);
}

bool CastUi::PlayOnTvIfCasting(CastVideo^ video)
{
    return video != nullptr && g_state != nullptr && g_state->PlayOnTvIfCasting(video);
}

void CastUi::AddToQueue(CastVideo^ video)
{
    if (video != nullptr && g_state != nullptr) g_state->AddToQueue(video);
}

bool CastUi::IsActive::get()
{
    return g_state != nullptr && g_state->IsActive();
}

void CastUi::Reopen()
{
    if (g_state != nullptr) g_state->Reopen();
}

Platform::String^ CastUi::TvTitle::get()
{
    return g_state == nullptr ? ref new Platform::String() : FromUtf8(g_state->TvTitle());
}

Platform::String^ CastUi::TvUploader::get()
{
    return g_state == nullptr ? ref new Platform::String() : FromUtf8(g_state->TvUploader());
}

double CastUi::TvPositionSeconds::get()
{
    return g_state == nullptr ? 0 : static_cast<double>(g_state->TvPosition());
}

double CastUi::TvDurationSeconds::get()
{
    return g_state == nullptr ? -1 : static_cast<double>(g_state->TvDuration());
}

bool CastUi::TvPlaying::get()
{
    return g_state != nullptr && g_state->TvPlaying();
}

void CastUi::TogglePlayPause()
{
    if (g_state != nullptr) g_state->TogglePlayPause();
}

void CastUi::RaiseActiveChanged(bool active)
{
    Platform::Object^ boxedActive = active;
    ActiveChanged(nullptr, boxedActive);
}

} // namespace PerleCast
