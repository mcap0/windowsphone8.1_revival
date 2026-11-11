#include "TaskLink.h"
#include "CastUi.h"
#include "CastLog.h"
#include "TaskMessages.h"
#include "WinRtText.h"

#include <mutex>
#include <vector>

#include <windows.applicationmodel.core.h>
#include <windows.media.playback.h>
#include <windows.system.threading.h>
#include <windows.ui.core.h>
#include <windows.ui.xaml.h>

using namespace Platform;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Media::Playback;
using namespace Windows::System::Threading;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;

namespace PerleCast {

namespace {

std::mutex g_lock;
bool g_started = false;     // Init done
bool g_ready = false;       // the task answered "ready"
ULONGLONG g_pingSince = 0;  // handshake start, 0 = none
std::vector<ValueSet^> g_pending;
std::vector<std::function<void(const std::wstring&, const std::string&)> > g_handlers;
ThreadPoolTimer^ g_pingTimer = nullptr;
CoreDispatcher^ g_dispatcher = nullptr;

void Flush()
{
    std::vector<ValueSet^> pending;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_ready) return;
        pending.swap(g_pending);
    }
    for (size_t i = 0; i < pending.size(); ++i)
    {
        try { BackgroundMediaPlayer::SendMessageToBackground(pending[i]); }
        catch (Platform::Exception^ ex) { CastLogLine("link: send failed: " + ToUtf8(ex->Message)); }
    }
}

void OnTaskMessage(Object^, MediaPlayerDataReceivedEventArgs^ args)
{
    std::wstring command = TaskCommand(args->Data);
    std::string data = TaskData(args->Data);
    if (command == L"ready")
    {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_ready = true;
            g_pingSince = 0;
        }
        Flush();
        return;
    }
    if (g_dispatcher == nullptr) return;
    g_dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([command, data]()
    {
        for (size_t i = 0; i < g_handlers.size(); ++i) g_handlers[i](command, data);
    }));
}

// Touching BackgroundMediaPlayer::Current starts the task; ping until it answers (it may still be starting).
void Ping(ThreadPoolTimer^ timer)
{
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (g_ready || g_pingSince == 0) { timer->Cancel(); g_pingTimer = nullptr; return; }
        if (GetTickCount64() - g_pingSince > 15000)
        {
            timer->Cancel();
            g_pingTimer = nullptr;
            g_pingSince = 0;
            g_pending.clear();
            CastLogLine("link: the task did not start");
            if (g_dispatcher != nullptr)
                g_dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([]()
                {
                    for (size_t i = 0; i < g_handlers.size(); ++i) g_handlers[i](L"taskfailed", std::string());
                }));
            return;
        }
    }
    try
    {
        BackgroundMediaPlayer::Current;
        BackgroundMediaPlayer::SendMessageToBackground(TaskMessage(L"ping"));
    }
    catch (Platform::Exception^) { }
}

void BeginHandshake()
{
    // under g_lock
    if (g_pingSince != 0) return;
    g_pingSince = GetTickCount64();
    TimeSpan period;
    period.Duration = 5000000;  // 0.5 s
    g_pingTimer = ThreadPoolTimer::CreatePeriodicTimer(ref new TimerElapsedHandler(&Ping), period);
}

void SendVisible(bool visible)
{
    if (!g_ready) return;  // a task started later begins with the app on screen
    try { BackgroundMediaPlayer::SendMessageToBackground(TaskMessage(L"visible", visible ? "1" : "0")); }
    catch (Platform::Exception^) { }
}

void Init()
{
    if (g_started) return;
    g_started = true;
    g_dispatcher = CoreApplication::MainView->CoreWindow->Dispatcher;
    // held until the process ends: the task stops when it is gone (app closed, not just in the background)
    CreateMutexExW(nullptr, AppAliveMutex, 0, SYNCHRONIZE);
    BackgroundMediaPlayer::MessageReceivedFromBackground +=
        ref new EventHandler<MediaPlayerDataReceivedEventArgs^>(&OnTaskMessage);
    Window::Current->VisibilityChanged += ref new WindowVisibilityChangedEventHandler(
        [](Object^, VisibilityChangedEventArgs^ args) { SendVisible(args->Visible); });
    // messages sent while the app was suspended are lost: ask the task how things are
    CoreApplication::Resuming += ref new EventHandler<Object^>([](Object^, Object^)
    {
        if (!CastUi::IsActive && !BackgroundAudio::IsActive) return;  // nothing running: do not start the task
        {
            std::lock_guard<std::mutex> guard(g_lock);
            g_ready = false;
        }
        TaskLink::Send(TaskMessage(L"hello"));
    });
}

} // namespace

namespace TaskLink {

void Send(ValueSet^ message)
{
    Init();
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_ready)
        {
            g_pending.push_back(message);
            BeginHandshake();
            return;
        }
    }
    try { BackgroundMediaPlayer::SendMessageToBackground(message); }
    catch (Platform::Exception^)
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_ready = false;
        g_pending.push_back(message);
        BeginHandshake();
    }
}

void ReleaseIfIdle()
{
    if (BackgroundAudio::IsActive || CastUi::IsActive) return;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_ready = false;
        g_pingSince = 0;
        g_pending.clear();
    }
    // also when the task died on its own: a fresh one starts next time (otherwise "the task did not start")
    try { BackgroundMediaPlayer::Shutdown(); }
    catch (Platform::Exception^) { }
}

void AddHandler(std::function<void(const std::wstring&, const std::string&)> handler)
{
    g_handlers.push_back(handler);
}

} // namespace TaskLink

// ---- BackgroundAudio ----------------------------------------------------------------------------

namespace {
bool g_audioActive = false;
std::string g_audioVideoUrl;
bool g_audioHandler = false;
}

void BackgroundAudio::Play(Platform::String^ source, Platform::String^ videoUrl, Platform::String^ title,
                           Platform::String^ uploader, double startSeconds)
{
    if (!g_audioHandler)
    {
        g_audioHandler = true;
        TaskLink::AddHandler(&BackgroundAudio::OnTaskMessage);
    }
    g_audioActive = true;
    g_audioVideoUrl = ToUtf8(videoUrl);
    TaskLink::Send(TaskMessage(L"bgaudio", Packer().Put(ToUtf8(source)).Put(g_audioVideoUrl).Put(ToUtf8(title))
        .Put(ToUtf8(uploader)).Put(static_cast<int64>(startSeconds < 0 ? 0 : startSeconds)).Text()));
}

// Immediate on the app's side ("torna al video" must not wait for a task that may be gone): the task saves the
// position when it gets the message, the task is shut down a moment later.
void BackgroundAudio::Stop()
{
    if (!g_audioActive) return;
    try { BackgroundMediaPlayer::SendMessageToBackground(TaskMessage(L"bgaudiostop")); }
    catch (Platform::Exception^) { }
    g_audioActive = false;
    TimeSpan delay;
    delay.Duration = 15000000;  // 1.5 s
    ThreadPoolTimer::CreateTimer(ref new TimerElapsedHandler([](ThreadPoolTimer^)
    {
        if (g_dispatcher == nullptr) return;
        g_dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([]()
        {
            TaskLink::ReleaseIfIdle();
            StatusChanged(nullptr, ref new Platform::String(L"stopped"));
        }));
    }), delay);
}

void BackgroundAudio::AppClosing()
{
    if (!g_audioActive) return;
    try { BackgroundMediaPlayer::SendMessageToBackground(TaskMessage(L"quit")); }
    catch (Platform::Exception^) { }
}

bool BackgroundAudio::IsActive::get()
{
    return g_audioActive;
}

Platform::String^ BackgroundAudio::VideoUrl::get()
{
    return FromUtf8(g_audioVideoUrl);
}

void BackgroundAudio::OnTaskMessage(const std::wstring& command, const std::string& data)
{
    if (command == L"state")
    {
        // after a resume: the audio may have ended meanwhile
        Unpacker in(data);
        in.Int();
        bool audio = in.Int() != 0;
        if (g_audioActive && !audio)
        {
            g_audioActive = false;
            StatusChanged(nullptr, ref new Platform::String(L"stopped"));
            TaskLink::ReleaseIfIdle();
        }
        return;
    }
    if (command == L"taskfailed")
    {
        if (!g_audioActive) return;
        g_audioActive = false;
        StatusChanged(nullptr, ref new Platform::String(L"error: il lettore in sottofondo non parte"));
        return;
    }
    if (command != L"bgaudio") return;
    Unpacker in(data);
    std::string status = in.Str();
    if (!g_audioActive && status != "playing") return;  // already stopped from here (Stop)
    if (status != "playing")
    {
        g_audioActive = false;
        TaskLink::ReleaseIfIdle();
    }
    StatusChanged(nullptr, FromUtf8(status));
}

} // namespace PerleCast
