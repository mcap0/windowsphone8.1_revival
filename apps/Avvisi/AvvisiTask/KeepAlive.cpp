#define NOMINMAX
#include "KeepAlive.h"
#include "Shared.h"
#include "MailPoll.h"
#include "TgPoll.h"

#include <windows.h>
#include <ppltasks.h>
#include <string>
#include <vector>

using namespace AvvisiTask;
using namespace Platform;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Media;
using namespace Windows::Media::Playback;
using namespace Windows::Storage;
using namespace Windows::System::Threading;

namespace
{
    const int TickSeconds = 120;

    // 60 s of silence, 8 kHz mono 8-bit PCM (unsigned: silence = 128), in LocalFolder\silence.wav. A long file
    // because every loop restart made an audible tick (the 1 s file ticked every second).
    void WriteSilence()
    {
        const unsigned int rate = 8000, dataSize = rate * 60;
        std::vector<unsigned char> wav(44 + dataSize, 128);
        auto put32 = [&wav](size_t at, unsigned int v) { for (int i = 0; i < 4; ++i) wav[at + i] = (v >> (8 * i)) & 0xFF; };
        auto put16 = [&wav](size_t at, unsigned int v) { wav[at] = v & 0xFF; wav[at + 1] = (v >> 8) & 0xFF; };
        memcpy(&wav[0], "RIFF", 4); put32(4, 36 + dataSize); memcpy(&wav[8], "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate); put16(32, 1); put16(34, 8);
        memcpy(&wav[36], "data", 4); put32(40, dataSize);

        std::wstring path = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\silence.wav";
        HANDLE file = CreateFile2(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw ref new Exception(HRESULT_FROM_WIN32(GetLastError()));
        DWORD written = 0;
        WriteFile(file, wav.data(), static_cast<DWORD>(wav.size()), &written, nullptr);
        CloseHandle(file);
    }

    const wchar_t* StateName(MediaPlayerState state)
    {
        switch (state)
        {
        case MediaPlayerState::Closed: return L"chiuso";
        case MediaPlayerState::Opening: return L"apertura";
        case MediaPlayerState::Buffering: return L"buffer";
        case MediaPlayerState::Playing: return L"suona";
        case MediaPlayerState::Paused: return L"pausa";
        case MediaPlayerState::Stopped: return L"fermo";
        default: return L"?";
        }
    }
}

void KeepAlive::Run(IBackgroundTaskInstance^ taskInstance)
{
    deferral_ = taskInstance->GetDeferral();
    ticks_ = 0;
    started_ = NowUtc();
    busy_ = 0;
    batteryStart_ = BatteryPercent();
    userPaused_ = false;
    finished_ = false;
    taskInstance->Canceled += ref new BackgroundTaskCanceledEventHandler(this, &KeepAlive::OnCanceled);
    SetToastContext(L"audio");
    Log(L"keepalive: avvio, batteria " + std::to_wstring(batteryStart_) + L"%, rete " + NetworkText() +
        L", memoria " + MemoryText());
    Log(L"telegram: cartella " + TelegramFolder());
    SetStatus(L"attivo");
    SetTick(0, started_, batteryStart_, batteryStart_);

    try
    {
        WriteSilence();
        player_ = BackgroundMediaPlayer::Current;
        player_->CurrentStateChanged += ref new TypedEventHandler<MediaPlayer^, Object^>(this, &KeepAlive::OnStateChanged);
        player_->MediaFailed += ref new TypedEventHandler<MediaPlayer^, MediaPlayerFailedEventArgs^>(this, &KeepAlive::OnFailed);
        BackgroundMediaPlayer::MessageReceivedFromForeground +=
            ref new EventHandler<MediaPlayerDataReceivedEventArgs^>(this, &KeepAlive::OnMessage);

        // WP8.1 stops background audio whose transport controls don't allow play and pause.
        controls_ = SystemMediaTransportControls::GetForCurrentView();
        controls_->IsEnabled = true;
        controls_->IsPlayEnabled = true;
        controls_->IsPauseEnabled = true;
        controls_->ButtonPressed += ref new TypedEventHandler<SystemMediaTransportControls^,
            SystemMediaTransportControlsButtonPressedEventArgs^>(this, &KeepAlive::OnButton);
        controls_->DisplayUpdater->Type = MediaPlaybackType::Music;
        controls_->DisplayUpdater->MusicProperties->Title = "Avvisi attivi";
        controls_->DisplayUpdater->MusicProperties->Artist = "Telegram e Posta";
        controls_->DisplayUpdater->Update();

        player_->IsLoopingEnabled = true;
        player_->AutoPlay = true;
        player_->Volume = 0.0;   // nothing audible even at the loop restart; the player still counts as playing
        player_->SetUriSource(ref new Uri("ms-appdata:///local/silence.wav"));
    }
    catch (Exception^ ex)
    {
        Log(L"keepalive: errore player " + Hex(ex->HResult) + L" " + ex->Message->Data());
    }

    Toast(L"Avvisi", L"attivo: controllo ogni 2 minuti", true, L"start");

    auto tick = ref new TimerElapsedHandler([this](ThreadPoolTimer^)
    {
        if (InterlockedExchange(&busy_, 1) != 0) return;     // the previous check is still running
        try { Tick(); }
        catch (Exception^ ex) { Log(L"tick: errore " + Hex(ex->HResult)); }
        catch (...) { Log(L"tick: errore"); }
        InterlockedExchange(&busy_, 0);
    });
    TimeSpan period;
    period.Duration = TickSeconds * 10000000LL;
    timer_ = ThreadPoolTimer::CreatePeriodicTimer(tick, period);
    TimeSpan soon;
    soon.Duration = 5 * 10000000LL;                           // first check right away, not after 2 minutes
    ThreadPoolTimer::CreateTimer(tick, soon);
}

void KeepAlive::Tick()
{
    if (finished_) return;
    ++ticks_;
    int battery = BatteryPercent();
    long long minutes = (NowUtc() - started_) / 600000000LL;
    std::wstring network = NetworkText();
    std::wstring checks;
    if (network == L"nessuna")
    {
        checks = L"nessun controllo (senza rete)";
    }
    else
    {
        std::wstring telegram = PollTelegram();
        std::wstring mail = PollMail();
        SetChecks(telegram, mail);
        checks = telegram + L"; " + mail;
    }
    std::wstring state = player_ != nullptr ? StateName(player_->CurrentState) : L"nessun player";
    Log(L"tick " + std::to_wstring(ticks_) + L", " + std::to_wstring(minutes) + L" min, batteria " +
        std::to_wstring(battery) + L"%, rete " + network + L"; " + checks + L"; audio " + state + L", memoria " + MemoryText());
    SetTick(ticks_, started_, batteryStart_, battery);

    // No automatic Play() here: a pause comes from a call or another app's audio, and the silence must never fight them.
}

void KeepAlive::Resume(const wchar_t* why)
{
    try
    {
        player_->Play();
        Log(std::wstring(L"audio: ripreso (") + why + L")");
    }
    catch (Exception^ ex)
    {
        Log(std::wstring(L"audio: ripresa fallita (") + why + L") " + Hex(ex->HResult));
    }
}

void KeepAlive::OnStateChanged(MediaPlayer^ sender, Object^)
{
    MediaPlayerState state = sender->CurrentState;
    Log(std::wstring(L"audio: ") + StateName(state));
    try
    {
        if (state == MediaPlayerState::Playing) controls_->PlaybackStatus = MediaPlaybackStatus::Playing;
        else if (state == MediaPlayerState::Paused) controls_->PlaybackStatus = MediaPlaybackStatus::Paused;
        else if (state == MediaPlayerState::Stopped || state == MediaPlayerState::Closed)
            controls_->PlaybackStatus = MediaPlaybackStatus::Stopped;
    }
    catch (...) { }
    SetStatus(state == MediaPlayerState::Playing ? L"attivo" : (userPaused_ ? L"in pausa" : std::wstring(L"audio ") + StateName(state)));
}

void KeepAlive::OnFailed(MediaPlayer^, MediaPlayerFailedEventArgs^ args)
{
    Log(L"audio: errore " + Hex(args->ExtendedErrorCode.Value) + L" " + args->ErrorMessage->Data());
}

void KeepAlive::OnMessage(Object^, MediaPlayerDataReceivedEventArgs^ args)
{
    ValueSet^ data = args->Data;
    String^ command = data->HasKey("cmd") ? dynamic_cast<String^>(data->Lookup("cmd")) : nullptr;
    if (command == nullptr) return;
    Log(std::wstring(L"messaggio: ") + command->Data());
    if (command == "stop")
    {
        Finish(L"fermato dall'app");
    }
    else if (command == "start")
    {
        userPaused_ = false;
        if (player_ != nullptr && player_->CurrentState != MediaPlayerState::Playing) Resume(L"app");
    }
}

void KeepAlive::OnButton(SystemMediaTransportControls^, SystemMediaTransportControlsButtonPressedEventArgs^ args)
{
    if (args->Button == SystemMediaTransportControlsButton::Pause)
    {
        userPaused_ = true;
        Log(L"tasto: pausa");
        try { player_->Pause(); } catch (...) { }
    }
    else if (args->Button == SystemMediaTransportControlsButton::Play)
    {
        userPaused_ = false;
        Log(L"tasto: play");
        Resume(L"tasto");
    }
}

void KeepAlive::Finish(const wchar_t* why)
{
    if (finished_) return;
    finished_ = true;
    Log(std::wstring(L"keepalive: fine, ") + why + L", " + std::to_wstring(ticks_) + L" tick, batteria " +
        std::to_wstring(batteryStart_) + L"% -> " + std::to_wstring(BatteryPercent()) + L"%");
    SetStatus(std::wstring(L"fermo: ") + why);
    if (timer_ != nullptr) timer_->Cancel();
    try { if (player_ != nullptr) player_->Pause(); } catch (...) { }
    if (deferral_ != nullptr)
    {
        deferral_->Complete();
        deferral_ = nullptr;
    }
}

void KeepAlive::OnCanceled(IBackgroundTaskInstance^, BackgroundTaskCancellationReason reason)
{
    std::wstring why = L"annullato dal sistema, motivo " + std::to_wstring(static_cast<int>(reason));
    Finish(why.c_str());
}
