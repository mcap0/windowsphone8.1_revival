#include "CastTask.h"
#include "CastLog.h"
#include "CastSession.h"
#include "TaskMessages.h"
#include "WinRtText.h"

#include <ppltasks.h>
#include <string>
#include <vector>

#include <windows.applicationmodel.background.h>
#include <windows.media.h>
#include <windows.media.playback.h>
#include <windows.storage.h>
#include <windows.storage.streams.h>
#include <windows.system.threading.h>

using namespace Platform;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Media;
using namespace Windows::Media::Playback;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::System::Threading;

namespace PerleCast {

namespace {

// One second of silence, 8 kHz mono 16-bit PCM, written to LocalFolder\silence.wav and played in a loop.
void WriteSilence()
{
    const unsigned int rate = 8000, samples = 8000, dataSize = samples * 2;
    std::vector<unsigned char> wav(44 + dataSize, 0);
    auto put32 = [&wav](size_t at, unsigned int v) { for (int i = 0; i < 4; ++i) wav[at + i] = (v >> (8 * i)) & 0xFF; };
    auto put16 = [&wav](size_t at, unsigned int v) { wav[at] = v & 0xFF; wav[at + 1] = (v >> 8) & 0xFF; };
    memcpy(&wav[0], "RIFF", 4); put32(4, 36 + dataSize); memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
    memcpy(&wav[36], "data", 4); put32(40, dataSize);

    StorageFile^ file = concurrency::create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(
        "silence.wav", CreationCollisionOption::ReplaceExisting)).get();
    concurrency::create_task(FileIO::WriteBytesAsync(file,
        ref new Platform::Array<unsigned char>(wav.data(), static_cast<unsigned int>(wav.size())))).get();
}

// Perle TV holds AppAliveMutex while its process exists (also suspended). Opening it here keeps it only for a moment.
bool AppAlive()
{
    HANDLE handle = CreateMutexExW(nullptr, AppAliveMutex, 0, SYNCHRONIZE);
    if (handle == nullptr) return true;  // cannot tell: never stop on this
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    CloseHandle(handle);
    return existed;
}

void SavePosition(const std::string& videoUrl, long long seconds)
{
    if (videoUrl.empty() || seconds < 0) return;
    try
    {
        ApplicationData::Current->LocalSettings->Values->Insert(
            FromUtf8("pos:" + videoUrl), FromUtf8(std::to_string(seconds)));
    }
    catch (Platform::Exception^) { }
}

void Send(ValueSet^ message)
{
    try { BackgroundMediaPlayer::SendMessageToForeground(message); }
    catch (Platform::Exception^) { }
}

} // namespace

void CastTask::Run(IBackgroundTaskInstance^ taskInstance)
{
    deferral_ = taskInstance->GetDeferral();
    session_ = new CastSession();
    mode_ = ModeIdle;
    finished_ = false;
    appSeen_ = AppAlive();
    appMissing_ = 0;
    ticks_ = 0;
    audioStart_ = 0;
    taskInstance->Canceled += ref new BackgroundTaskCanceledEventHandler(this, &CastTask::OnCanceled);

    session_->Published = [this](const ViewState& view, const std::string& url)
    {
        if (view.title != tvTitle_)
        {
            tvTitle_ = view.title;
            ShowOnLockScreen(view.title, "TV \xC2\xB7 " + view.device);
        }
        Send(TaskMessage(L"view", Packer().Put(view).Put(url).Text()));
    };
    session_->Started = [this](bool ok, const std::string& error)
    {
        if (!ok)
        {
            std::lock_guard<std::recursive_mutex> guard(lock_);
            if (mode_ == ModeCast) { mode_ = ModeIdle; player_->Pause(); }
        }
        Send(TaskMessage(L"started", Packer().Put(ok ? 1 : 0).Put(error).Text()));
    };
    session_->Ended = [this]()
    {
        {
            std::lock_guard<std::recursive_mutex> guard(lock_);
            if (mode_ == ModeCast) { mode_ = ModeIdle; player_->Pause(); }
        }
        Send(TaskMessage(L"ended"));
    };

    try
    {
        player_ = BackgroundMediaPlayer::Current;
        player_->MediaOpened += ref new TypedEventHandler<MediaPlayer^, Object^>(this, &CastTask::OnMediaOpened);
        player_->MediaEnded += ref new TypedEventHandler<MediaPlayer^, Object^>(this, &CastTask::OnMediaEnded);
        player_->MediaFailed += ref new TypedEventHandler<MediaPlayer^, MediaPlayerFailedEventArgs^>(
            this, &CastTask::OnMediaFailed);
        player_->CurrentStateChanged += ref new TypedEventHandler<MediaPlayer^, Object^>(this, &CastTask::OnStateChanged);

        controls_ = SystemMediaTransportControls::GetForCurrentView();
        controls_->IsEnabled = true;
        controls_->IsPlayEnabled = true;
        controls_->IsPauseEnabled = true;
        controls_->ButtonPressed += ref new TypedEventHandler<SystemMediaTransportControls^,
            SystemMediaTransportControlsButtonPressedEventArgs^>(this, &CastTask::OnButton);

        BackgroundMediaPlayer::MessageReceivedFromForeground +=
            ref new EventHandler<MediaPlayerDataReceivedEventArgs^>(this, &CastTask::OnMessage);
    }
    catch (Platform::Exception^ ex) { CastLogLine("task: setup failed: " + ToUtf8(ex->Message)); }

    TimeSpan period;
    period.Duration = 2 * 10000000LL;
    timer_ = ThreadPoolTimer::CreatePeriodicTimer(ref new TimerElapsedHandler(this, &CastTask::OnTick), period);
    CastLogLine(std::string("task: run, app ") + (appSeen_ ? "seen" : "NOT seen"));
    Send(TaskMessage(L"ready"));
}

void CastTask::OnMessage(Object^, MediaPlayerDataReceivedEventArgs^ args)
{
    if (finished_ || args == nullptr) return;
    std::wstring command = TaskCommand(args->Data);
    std::string data = TaskData(args->Data);

    if (command == L"ping") { Send(TaskMessage(L"ready")); return; }
    if (command == L"hello") { SendState(); return; }
    if (command == L"visible")
    {
        session_->SetBackground(data != "1");
        return;
    }
    if (command == L"bgaudio") { StartAudio(data); return; }
    if (command == L"bgaudiostop") { StopAudio("stopped"); return; }
    if (command == L"quit") { Finish("app closed with Back"); return; }
    HandleCast(command, data);
}

void CastTask::HandleCast(const std::wstring& command, const std::string& data)
{
    if (command == L"start")
    {
        StopAudio("stopped");
        PlaySilence();
    }
    session_->Dispatch(command, data);
}

void CastTask::SendState()
{
    bool cast, audio;
    {
        std::lock_guard<std::recursive_mutex> guard(lock_);
        cast = session_->Active();
        audio = mode_ == ModeAudio;
    }
    Send(TaskMessage(L"state", Packer().Put(cast ? 1 : 0).Put(audio ? 1 : 0).Put(audioVideoUrl_).Text()));
    if (cast) session_->Run([this]() { session_->Publish(std::string()); });
}

void CastTask::PlaySilence()
{
    std::lock_guard<std::recursive_mutex> guard(lock_);
    mode_ = ModeCast;
    tvTitle_.clear();
    try
    {
        WriteSilence();
        player_->IsLoopingEnabled = true;
        player_->AutoPlay = true;
        player_->SetUriSource(ref new Uri("ms-appdata:///local/silence.wav"));
    }
    catch (Platform::Exception^ ex) { CastLogLine("task: silence failed: " + ToUtf8(ex->Message)); }
}

// data: source (URL, or a local path as "file:<path>"), video URL, title, uploader, start seconds
void CastTask::StartAudio(const std::string& data)
{
    Unpacker in(data);
    std::string source = in.Str();
    std::string videoUrl = in.Str();
    std::string title = in.Str();
    std::string uploader = in.Str();
    long long start = in.Int();
    if (session_->Active()) session_->Run([this]() { session_->EndSession(true, true); });

    std::lock_guard<std::recursive_mutex> guard(lock_);
    mode_ = ModeAudio;
    audioVideoUrl_ = videoUrl;
    audioStart_ = start;
    CastLogLine("task: audio \"" + title + "\" from " + std::to_string(start) + "s, " +
        (source.compare(0, 5, "file:") == 0 ? source : std::string("online")));
    ShowOnLockScreen(title, uploader);
    try
    {
        // like Musica's task (which plays): AutoPlay, Play right after the source, lock screen told "playing"; the
        // start position is applied when the source is open. Waiting for MediaOpened to call Play left it paused.
        player_->IsLoopingEnabled = false;
        player_->AutoPlay = true;
        if (source.compare(0, 5, "file:") == 0)
        {
            StorageFile^ file = concurrency::create_task(StorageFile::GetFileFromPathAsync(FromUtf8(source.substr(5)))).get();
            player_->SetFileSource(file);
        }
        else
        {
            player_->SetUriSource(ref new Uri(FromUtf8(source)));
        }
        player_->Play();
        if (controls_ != nullptr) controls_->PlaybackStatus = MediaPlaybackStatus::Playing;
    }
    catch (Platform::Exception^ ex)
    {
        mode_ = ModeIdle;
        CastLogLine("task: audio failed: " + ToUtf8(ex->Message));
        Send(TaskMessage(L"bgaudio", Packer().Put("error: " + ToUtf8(ex->Message)).Put(start).Text()));
    }
}

long long CastTask::AudioSeconds()
{
    try { return player_->Position.Duration / 10000000LL; }
    catch (Platform::Exception^) { return -1; }
}

// status = what the app is told (nullptr: nothing, another job takes the player)
void CastTask::StopAudio(const char* status)
{
    std::lock_guard<std::recursive_mutex> guard(lock_);
    if (mode_ != ModeAudio) return;
    long long position = AudioSeconds();
    SavePosition(audioVideoUrl_, position);
    try { player_->Pause(); } catch (Platform::Exception^) { }
    mode_ = ModeIdle;
    CastLogLine("task: audio stopped at " + std::to_string(position) + "s");
    if (status != nullptr) Send(TaskMessage(L"bgaudio", Packer().Put(status).Put(position).Text()));
}

void CastTask::ShowOnLockScreen(const std::string& title, const std::string& artist)
{
    if (controls_ == nullptr) return;
    try
    {
        SystemMediaTransportControlsDisplayUpdater^ updater = controls_->DisplayUpdater;
        updater->Type = MediaPlaybackType::Music;
        updater->MusicProperties->Title = FromUtf8(title);
        updater->MusicProperties->Artist = FromUtf8(artist);
        updater->Update();
    }
    catch (Platform::Exception^) { }
}

void CastTask::OnMediaOpened(MediaPlayer^ sender, Object^)
{
    std::lock_guard<std::recursive_mutex> guard(lock_);
    if (mode_ != ModeAudio) return;
    try
    {
        if (sender->CurrentState != MediaPlayerState::Playing) sender->Play();
        // the start position is applied once it plays (set here, while AutoPlay starts it, it was ignored)
        CastLogLine("task: audio opened, playing from " + std::to_string(audioStart_) + "s");
        Send(TaskMessage(L"bgaudio", Packer().Put("playing").Put(audioStart_).Text()));
    }
    catch (Platform::Exception^ ex) { CastLogLine("task: audio start failed: " + ToUtf8(ex->Message)); }
}

void CastTask::OnMediaEnded(MediaPlayer^, Object^)
{
    std::lock_guard<std::recursive_mutex> guard(lock_);
    if (mode_ != ModeAudio) return;
    SavePosition(audioVideoUrl_, 0);  // heard to the end: from the beginning next time
    mode_ = ModeIdle;
    Send(TaskMessage(L"bgaudio", Packer().Put("ended").Put(0).Text()));
}

void CastTask::OnMediaFailed(MediaPlayer^, MediaPlayerFailedEventArgs^ args)
{
    std::string error = ToUtf8(args->ErrorMessage);
    CastLogLine("task: player failed: " + error);
    std::lock_guard<std::recursive_mutex> guard(lock_);
    if (mode_ != ModeAudio) return;
    mode_ = ModeIdle;
    Send(TaskMessage(L"bgaudio", Packer().Put("error: " + error).Put(audioStart_).Text()));
}

void CastTask::OnStateChanged(MediaPlayer^ sender, Object^)
{
    try
    {
        static const char* names[] = { "closed", "opening", "buffering", "playing", "paused", "stopped" };
        int index = static_cast<int>(sender->CurrentState);
        CastLogLine(std::string("task: player ") + (index >= 0 && index < 6 ? names[index] : "?") +
            (mode_ == ModeAudio ? " (audio)" : mode_ == ModeCast ? " (cast)" : ""));
    }
    catch (Platform::Exception^) { }
    if (mode_ == ModeAudio && audioStart_ > 0 && sender->CurrentState == MediaPlayerState::Playing)
    {
        std::lock_guard<std::recursive_mutex> guard(lock_);
        try
        {
            TimeSpan at;
            at.Duration = audioStart_ * 10000000LL;
            sender->Position = at;
            CastLogLine("task: audio seek to " + std::to_string(audioStart_) + "s");
        }
        catch (Platform::Exception^ ex) { CastLogLine("task: audio seek failed: " + ToUtf8(ex->Message)); }
        audioStart_ = 0;
    }
    if (controls_ == nullptr) return;
    try
    {
        MediaPlayerState state = sender->CurrentState;
        if (mode_ == ModeCast) controls_->PlaybackStatus = MediaPlaybackStatus::Playing;  // the TV's state is on the remote
        else if (state == MediaPlayerState::Playing) controls_->PlaybackStatus = MediaPlaybackStatus::Playing;
        else if (state == MediaPlayerState::Paused) controls_->PlaybackStatus = MediaPlaybackStatus::Paused;
    }
    catch (Platform::Exception^) { }
}

// Lock screen / volume overlay buttons: the TV while casting, the audio otherwise.
void CastTask::OnButton(SystemMediaTransportControls^, SystemMediaTransportControlsButtonPressedEventArgs^ args)
{
    if (mode_ == ModeCast)
    {
        CastSession* s = session_;
        if (args->Button == SystemMediaTransportControlsButton::Play ||
            args->Button == SystemMediaTransportControlsButton::Pause)
            s->Run([s]() { s->PlayPause(); });
        return;
    }
    if (mode_ != ModeAudio) return;
    try
    {
        if (args->Button == SystemMediaTransportControlsButton::Play) player_->Play();
        else if (args->Button == SystemMediaTransportControlsButton::Pause) player_->Pause();
    }
    catch (Platform::Exception^) { }
}

void CastTask::OnTick(ThreadPoolTimer^)
{
    if (finished_) return;
    ++ticks_;
    session_->PollSoon();
    if (mode_ == ModeAudio && ticks_ % 5 == 0)
    {
        std::lock_guard<std::recursive_mutex> guard(lock_);
        if (mode_ == ModeAudio)
        {
            int state = static_cast<int>(player_->CurrentState);
            CastLogLine("task: audio at " + std::to_string(AudioSeconds()) + "s, state " + std::to_string(state));
            if (player_->CurrentState == MediaPlayerState::Playing) SavePosition(audioVideoUrl_, AudioSeconds());
        }
    }

    // The app's mutex also disappeared while Perle TV was only in the background (2026-10-22 test): logged, not
    // acted on. Closing = "quit" from the app (Back on the main page).
    bool alive = AppAlive();
    if (alive != appSeen_) CastLogLine(std::string("task: app mutex ") + (alive ? "back" : "gone"));
    appSeen_ = alive;
}

void CastTask::Finish(const std::string& reason)
{
    if (finished_) return;
    finished_ = true;
    CastLogLine("task: finish, " + reason);
    if (timer_ != nullptr) timer_->Cancel();
    StopAudio("stopped");  // the app goes back to its own player
    CastSession* s = session_;
    BackgroundTaskDeferral^ deferral = deferral_;
    MediaPlayer^ player = player_;
    s->Run([s, deferral, player]()
    {
        s->EndSession(true, true);  // "ended" to the app (no use when it was closed)
        try { player->Pause(); } catch (Platform::Exception^) { }
        if (deferral != nullptr) deferral->Complete();
    });
}

void CastTask::OnCanceled(IBackgroundTaskInstance^, BackgroundTaskCancellationReason reason)
{
    Finish("canceled, reason " + std::to_string(static_cast<int>(reason)));
}

} // namespace PerleCast
