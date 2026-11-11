#include "pch.h"
#include "MainPage.xaml.h"
#include "Core/ClockText.h"
#include "Alarms.h"

#include <windows.h>
#include <cwchar>
#include <stdint.h>

#include <Windows.Networking.h>
#include <Windows.Networking.Sockets.h>
#include <Windows.Storage.h>
#include <Windows.Storage.Streams.h>
#include <Windows.UI.Notifications.h>
#include <Windows.Data.Xml.Dom.h>
#include <Windows.Phone.Devices.Notification.h>

using namespace Orologio;
using namespace ClockText;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::Data::Xml::Dom;
using namespace Windows::Foundation;
using namespace Windows::Networking;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Notifications;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::Phone::Devices::Notification;
using namespace concurrency;

namespace
{
    // settings values: C++/CX boxes value types when they are assigned to Object^
    template <typename T> Object^ ToObject(T value) { Object^ boxed = value; return boxed; }
    template <typename T> T FromObject(Object^ value) { return safe_cast<T>(value); }

    const int64_t WindowsToUnixEpochTicks = 116444736000000000LL;
    const int64_t TicksPerMillisecond = 10000LL;
    const int64_t TicksPerSecond = 10000000LL;

    String^ MakeString(const std::wstring& value)
    {
        return ref new String(value.c_str());
    }

    std::wstring WideString(String^ value)
    {
        return value == nullptr ? std::wstring() : std::wstring(value->Data());
    }

    int64_t WallClockTicks()
    {
        FILETIME fileTime;
        GetSystemTimeAsFileTime(&fileTime);
        ULARGE_INTEGER value;
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        return static_cast<int64_t>(value.QuadPart);
    }

    int64_t UnixMillisecondsNow()
    {
        return (WallClockTicks() - WindowsToUnixEpochTicks) / TicksPerMillisecond;
    }

    int64_t NtpTimestampMilliseconds(const uint8_t* data)
    {
        uint64_t seconds = (static_cast<uint64_t>(data[0]) << 24) |
                           (static_cast<uint64_t>(data[1]) << 16) |
                           (static_cast<uint64_t>(data[2]) << 8) |
                           static_cast<uint64_t>(data[3]);
        uint64_t fraction = (static_cast<uint64_t>(data[4]) << 24) |
                            (static_cast<uint64_t>(data[5]) << 16) |
                            (static_cast<uint64_t>(data[6]) << 8) |
                            static_cast<uint64_t>(data[7]);
        return static_cast<int64_t>(seconds * 1000ULL +
                                    ((fraction * 1000ULL) >> 32));
    }

    SYSTEMTIME LocalSystemTimeFromUnixMs(int64_t unixMs)
    {
        SYSTEMTIME local = {};
        int64_t ticks = unixMs * TicksPerMillisecond + WindowsToUnixEpochTicks;
        FILETIME fileTime;
        ULARGE_INTEGER value;
        value.QuadPart = static_cast<uint64_t>(ticks);
        fileTime.dwLowDateTime = value.LowPart;
        fileTime.dwHighDateTime = value.HighPart;

        SYSTEMTIME utc = {};
        if (FileTimeToSystemTime(&fileTime, &utc))
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
        return local;
    }

    SYSTEMTIME LocalSystemTimeFromFileTicks(int64_t ticks)
    {
        SYSTEMTIME local = {};
        FILETIME fileTime;
        ULARGE_INTEGER value;
        value.QuadPart = static_cast<uint64_t>(ticks);
        fileTime.dwLowDateTime = value.LowPart;
        fileTime.dwHighDateTime = value.HighPart;

        // FileTimeToLocalFileTime is not in the phone API set: convert through UTC SYSTEMTIME
        SYSTEMTIME utc = {};
        if (FileTimeToSystemTime(&fileTime, &utc))
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
        return local;
    }

    std::wstring TwoDigits(int value)
    {
        std::wostringstream stream;
        stream << std::setw(2) << std::setfill(L'0') << value;
        return stream.str();
    }

    std::wstring CheckedStatus(int64_t offsetMs, int64_t checkedAt)
    {
        SYSTEMTIME checked = LocalSystemTimeFromFileTicks(checkedAt);
        std::wstring result = OffsetText(offsetMs);
        result += L" \u00b7 verificato alle ";
        result += TwoDigits(checked.wHour);
        result += L":";
        result += TwoDigits(checked.wMinute);
        return result;
    }

    int64_t CurrentStopwatchElapsed(int64_t start, int64_t accumulated, bool running)
    {
        if (!running) return accumulated > 0 ? accumulated : 0;
        int64_t elapsed = WallClockTicks() - start;
        return elapsed > 0 ? elapsed : 0;
    }
}

MainPage::MainPage()
    : _settings(ApplicationData::Current->LocalSettings),
      _clockTimer(ref new DispatcherTimer()),
      _stopwatchTimer(ref new DispatcherTimer()),
      _timerTimer(ref new DispatcherTimer()),
      _ntpTimeout(ref new DispatcherTimer()),
      _ntpSocket(nullptr),
      _restored(false),
      _clockHasSecond(false),
      _hasNtpOffset(false),
      _ntpTryingFallback(false),
      _stopwatchRunning(false),
      _timerExpired(false),
      _timerRinging(false),
      _ntpOffsetMs(0),
      _ntpCheckedAt(0),
      _lastClockSecond(0),
      _ntpT0(0),
      _stopwatchStart(0),
      _stopwatchAccum(0),
      _timerEnd(0),
      _timerPausedLeft(0),
      _timerTotal(0),
      _timerExpiredAt(0),
      _ringStartedAt(0),
      _lastVibrationAt(0),
      _timerState(0),
      _ntpRequestId(0),
      _lapsSerialized(ref new String(L""))
{
    InitializeComponent();

    SetPicked(0, 0, 0);

    _clockTimer->Interval = TimeSpan{ 1000000 };
    _clockTimer->Tick += ref new EventHandler<Object^>(this, &MainPage::ClockTimer_Tick);
    _clockTimer->Start();

    _stopwatchTimer->Interval = TimeSpan{ 500000 };
    _stopwatchTimer->Tick += ref new EventHandler<Object^>(this, &MainPage::StopwatchTimer_Tick);

    _timerTimer->Interval = TimeSpan{ 1000000 };
    _timerTimer->Tick += ref new EventHandler<Object^>(this, &MainPage::TimerTimer_Tick);
    _timerTimer->Start();

    _ntpTimeout->Interval = TimeSpan{ 50000000 };
    _ntpTimeout->Tick += ref new EventHandler<Object^>(this, &MainPage::NtpTimeout_Tick);

    UpdateClock(true);
    UpdateStopwatchButtons();
    UpdateTimer();
}

bool MainPage::HandleHardwareBack()
{
    if (Tabs->SelectedIndex != 0)
    {
        Tabs->SelectedIndex = 0;
        return true;
    }
    return false;
}

namespace
{
    std::wstring QueryValue(Uri^ uri, const wchar_t* name)
    {
        try
        {
            return WideString(uri->QueryParsed->GetFirstValueByName(ref new String(name)));
        }
        catch (Exception^)
        {
            return std::wstring();
        }
    }

    // alarm results also go to the PC (tools/inbox, share/inbox/orologio.txt) for diagnostics
    void SendToPc(const std::wstring& text)
    {
        try
        {
            auto client = ref new Windows::Web::Http::HttpClient();
            auto content = ref new Windows::Web::Http::HttpStringContent(ref new String((text + L"\n").c_str()));
            create_task(client->PostAsync(ref new Uri(L"http://192.168.1.148:8765/orologio"), content)).then(
                [client](task<Windows::Web::Http::HttpResponseMessage^> t)
                {
                    try { t.get(); } catch (Exception^) {}
                });
        }
        catch (Exception^) {}
    }

    void ShowMessage(const std::wstring& text)
    {
        SendToPc(text);
        auto dialog = ref new Windows::UI::Popups::MessageDialog(ref new String(text.c_str()));
        create_task(dialog->ShowAsync());
    }
}

// orologio:sveglia?ora=HH:MM[&giorno=oggi|domani|dopodomani|AAAA-MM-GG][&giorni=mask][&etichetta=...]
void MainPage::CreateAlarmFromUri(String^ text)
{
    Uri^ uri = ref new Uri(text);
    std::wstring time = QueryValue(uri, L"ora");
    int hour = -1, minute = -1;
    if (swscanf_s(time.c_str(), L"%d:%d", &hour, &minute) != 2 || hour < 0 || hour > 23 || minute < 0 || minute > 59)
    {
        ShowMessage(L"Ora della sveglia non valida: " + time);
        return;
    }
    AlarmInfo info = {};
    info.slot = -1;
    info.hour = hour;
    info.minute = minute;
    info.enabled = true;
    info.label = QueryValue(uri, L"etichetta");
    info.days = static_cast<unsigned char>(_wtoi(QueryValue(uri, L"giorni").c_str()) & 0x7f);
    std::wstring day = QueryValue(uri, L"giorno");
    int offset = day == L"domani" ? 1 : day == L"dopodomani" ? 2 : day == L"oggi" ? 0 : -1;
    if (offset >= 0)
    {
        SYSTEMTIME now;
        GetLocalTime(&now);
        FILETIME ft;
        SystemTimeToFileTime(&now, &ft);
        unsigned long long t = (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        t += 864000000000ULL * offset;
        ft.dwLowDateTime = static_cast<DWORD>(t);
        ft.dwHighDateTime = static_cast<DWORD>(t >> 32);
        FileTimeToSystemTime(&ft, &now);
        info.year = now.wYear;
        info.month = now.wMonth;
        info.day = now.wDay;
    }
    else if (swscanf_s(day.c_str(), L"%d-%d-%d", &info.year, &info.month, &info.day) != 3)
    {
        info.year = 0;
    }

    std::wstring error;
    int slot = Alarms::Save(info, error);
    wchar_t when[8];
    swprintf_s(when, L"%02d:%02d", hour, minute);
    if (slot < 0)
    {
        ShowMessage(std::wstring(L"Sveglia non creata: ") + error);
        return;
    }
    SendToPc(std::wstring(L"sveglia ") + when + L" nel posto " + std::to_wstring(slot));
    Tabs->SelectedIndex = 3;
    RefreshAlarms();
}

void MainPage::ShowAlarmList()
{
    std::wstring error;
    std::vector<AlarmInfo> list = Alarms::List(error);
    std::wstring text = error.empty() ? L"" : L"Errore: " + error + L"\n";
    for (const AlarmInfo& a : list)
    {
        wchar_t line[96];
        swprintf_s(line, L"%d) %02d:%02d %04d-%02d-%02d giorni=%d %s ", a.slot, a.hour, a.minute, a.year, a.month,
                   a.day, a.days, a.enabled ? L"attiva" : L"spenta");
        text += line + a.label + L"\n";
    }
    SendToPc(text.empty() ? L"Nessuna sveglia." : text);
}

void MainPage::OnLaunchArguments(String^ args)
{
    if (args == nullptr) return;
    std::wstring text = WideString(args);
    if (text == L"timer")
        Tabs->SelectedIndex = 2;
    else if (text.compare(0, 17, L"orologio:sveglia?") == 0)
        CreateAlarmFromUri(args);
    else if (text == L"orologio:sveglie")
    {
        Tabs->SelectedIndex = 3;
        ShowAlarmList();
    }
    else if (text.compare(0, 15, L"orologio:timer?") == 0)
    {
        size_t at = text.find(L"secondi=");
        int seconds = at == std::wstring::npos ? 0 : _wtoi(text.c_str() + at + 8);
        Tabs->SelectedIndex = 2;
        if (text.find(L"annulla=1") != std::wstring::npos)  // from the Assistente: "annulla il timer"
        {
            StopTimerRinging();
            StopTimer();
            UpdateTimerButtons();
            return;
        }
        if (seconds <= 0 || seconds > 86399) return;
        StopTimer();
        SetTimerDuration(seconds);
        StartTimer();
        UpdateTimerButtons();
    }
}

void MainPage::OnNavigatedTo(NavigationEventArgs^ e)
{
    Page::OnNavigatedTo(e);
    if (!_restored)
    {
        RestoreSettings();
        _restored = true;
        StartNtpAttempt(false);
    }
    OnLaunchArguments(dynamic_cast<String^>(e->Parameter));
}

void MainPage::RestoreSettings()
{
    LoadNtpState();
    RestoreStopwatch();
    RestoreTimer();
}

void MainPage::LoadNtpState()
{
    try
    {
        if (_settings->Values->HasKey(L"ntpOffsetMs") &&
            _settings->Values->HasKey(L"ntpCheckedAt"))
        {
            _ntpOffsetMs = FromObject<int64_t>(_settings->Values->Lookup(L"ntpOffsetMs"));
            _ntpCheckedAt = FromObject<int64_t>(_settings->Values->Lookup(L"ntpCheckedAt"));
            _hasNtpOffset = true;
            SetNtpStatus(MakeString(CheckedStatus(_ntpOffsetMs, _ntpCheckedAt)));
        }
        else
        {
            _hasNtpOffset = false;
            SetNtpStatus(ref new String(L"ora non verificata (rete)"));
        }
    }
    catch (...)
    {
        _hasNtpOffset = false;
        SetNtpStatus(ref new String(L"ora non verificata (rete)"));
    }
}

void MainPage::RestoreStopwatch()
{
    try
    {
        auto values = _settings->Values;
        _stopwatchRunning = values->HasKey(L"swRunning") &&
                            FromObject<bool>(values->Lookup(L"swRunning"));
        if (values->HasKey(L"swStart"))
            _stopwatchStart = FromObject<int64_t>(values->Lookup(L"swStart"));
        if (values->HasKey(L"swAccum"))
            _stopwatchAccum = FromObject<int64_t>(values->Lookup(L"swAccum"));
        if (values->HasKey(L"swLaps"))
        {
            String^ saved = dynamic_cast<String^>(values->Lookup(L"swLaps"));
            if (saved != nullptr) _lapsSerialized = saved;
        }
    }
    catch (...)
    {
        _stopwatchRunning = false;
        _stopwatchAccum = 0;
        _stopwatchStart = 0;
        _lapsSerialized = ref new String(L"");
    }

    if (_stopwatchRunning)
        _stopwatchTimer->Start();
    else
        _stopwatchTimer->Stop();

    RebuildLapList();
    UpdateStopwatch();
    UpdateStopwatchButtons();
}

void MainPage::RestoreTimer()
{
    try
    {
        auto values = _settings->Values;
        if (values->HasKey(L"tmState"))
            _timerState = FromObject<int>(values->Lookup(L"tmState"));
        if (values->HasKey(L"tmEnd"))
            _timerEnd = FromObject<int64_t>(values->Lookup(L"tmEnd"));
        if (values->HasKey(L"tmPausedLeft"))
            _timerPausedLeft = FromObject<int64_t>(values->Lookup(L"tmPausedLeft"));
        if (values->HasKey(L"tmTotal"))
            _timerTotal = FromObject<int64_t>(values->Lookup(L"tmTotal"));
    }
    catch (...)
    {
        _timerState = 0;
        _timerEnd = 0;
        _timerPausedLeft = 0;
        _timerTotal = 0;
    }

    if (_timerState < 0 || _timerState > 2) _timerState = 0;

    if (_timerTotal > 0)
    {
        int64_t seconds = _timerTotal / TicksPerSecond;
        SetTimerDuration(static_cast<int>(seconds > 86399 ? 86399 : seconds));
    }

    _timerExpired = false;
    _timerRinging = false;
    if (_timerState == 1 && _timerEnd <= WallClockTicks())
    {
        _timerExpired = true;
        _timerExpiredAt = _timerEnd;
        RemoveTimerToast();
    }

    UpdateTimer();
    UpdateTimerButtons();
}

void MainPage::SaveStopwatch()
{
    auto values = _settings->Values;
    values->Insert(L"swRunning", ToObject(_stopwatchRunning));
    values->Insert(L"swStart", ToObject(_stopwatchStart));
    values->Insert(L"swAccum", ToObject(_stopwatchAccum));
    values->Insert(L"swLaps", _lapsSerialized);
}

void MainPage::SaveTimer()
{
    auto values = _settings->Values;
    values->Insert(L"tmEnd", ToObject(_timerEnd));
    values->Insert(L"tmPausedLeft", ToObject(_timerPausedLeft));
    values->Insert(L"tmState", ToObject(_timerState));
    values->Insert(L"tmTotal", ToObject(_timerTotal));
}

void MainPage::SetNtpStatus(String^ text)
{
    NtpStatusTextBlock->Text = text;
}

void MainPage::UpdateClock(bool force)
{
    int64_t unixMs = UnixMillisecondsNow() + (_hasNtpOffset ? _ntpOffsetMs : 0);
    int64_t displayedSecond = unixMs / 1000;
    if (!force && _clockHasSecond && displayedSecond == _lastClockSecond)
        return;

    _clockHasSecond = true;
    _lastClockSecond = displayedSecond;
    SYSTEMTIME local = LocalSystemTimeFromUnixMs(unixMs);
    ClockTextBlock->Text = MakeString(FormatClock(local.wHour, local.wMinute, local.wSecond));
    DateTextBlock->Text = MakeString(ItalianDate(local.wDayOfWeek,
                                                  local.wDay,
                                                  local.wMonth,
                                                  local.wYear));
}

void MainPage::ClockTimer_Tick(Object^, Object^)
{
    UpdateClock(false);
}

void MainPage::StopwatchTimer_Tick(Object^, Object^)
{
    UpdateStopwatch();
}

void MainPage::TimerTimer_Tick(Object^, Object^)
{
    if (_timerState == 1 && !_timerExpired && WallClockTicks() >= _timerEnd)
        BeginTimerRinging();

    if (_timerRinging)
    {
        int64_t now = WallClockTicks();
        if (now - _ringStartedAt >= 60 * TicksPerSecond)
        {
            StopTimerRinging();
        }
        else if (_lastVibrationAt == 0 || now - _lastVibrationAt >= 2 * TicksPerSecond)
        {
            try
            {
                auto vibration = VibrationDevice::GetDefault();
                if (vibration != nullptr)
                {
                    TimeSpan duration;
                    duration.Duration = 800 * 10000;
                    vibration->Vibrate(duration);
                }
            }
            catch (...) {}
            _lastVibrationAt = now;
        }
    }

    UpdateTimer();
}

void MainPage::UpdateStopwatch()
{
    int64_t elapsed = CurrentStopwatchElapsed(_stopwatchStart,
                                               _stopwatchAccum,
                                               _stopwatchRunning);
    StopwatchTextBlock->Text = MakeString(FormatStopwatch(elapsed));
}

void MainPage::UpdateStopwatchButtons()
{
    StopwatchToggleButton->Content = ref new String(_stopwatchRunning ? L"pausa" : L"avvia");
    StopwatchLapButton->IsEnabled = _stopwatchRunning;
    StopwatchResetButton->IsEnabled = !_stopwatchRunning && _stopwatchAccum > 0;
}

void MainPage::RebuildLapList()
{
    LapsList->Items->Clear();
    std::vector<int64_t> laps = SplitLaps(WideString(_lapsSerialized));
    for (size_t i = 0; i < laps.size(); ++i)
    {
        std::wostringstream line;
        line << L"Giro " << (laps.size() - i)
             << L"   " << FormatStopwatch(laps[i])
             << L"   totale " << FormatStopwatch(laps[i]);
        LapsList->Items->Append(MakeString(line.str()));
    }
}

void MainPage::StopwatchToggle_Click(Object^, RoutedEventArgs^)
{
    int64_t now = WallClockTicks();
    if (_stopwatchRunning)
    {
        _stopwatchAccum = CurrentStopwatchElapsed(_stopwatchStart, 0, true);
        _stopwatchRunning = false;
        _stopwatchTimer->Stop();
    }
    else
    {
        if (_stopwatchAccum < 0) _stopwatchAccum = 0;
        _stopwatchStart = now - _stopwatchAccum;
        _stopwatchRunning = true;
        _stopwatchTimer->Start();
    }

    SaveStopwatch();
    UpdateStopwatch();
    UpdateStopwatchButtons();
}

void MainPage::StopwatchLap_Click(Object^, RoutedEventArgs^)
{
    if (!_stopwatchRunning) return;

    int64_t elapsed = CurrentStopwatchElapsed(_stopwatchStart, 0, true);
    std::vector<int64_t> laps = SplitLaps(WideString(_lapsSerialized));
    laps.insert(laps.begin(), elapsed);
    _lapsSerialized = MakeString(JoinLaps(laps));
    SaveStopwatch();
    RebuildLapList();
}

void MainPage::StopwatchReset_Click(Object^, RoutedEventArgs^)
{
    if (_stopwatchRunning || _stopwatchAccum <= 0) return;

    _stopwatchAccum = 0;
    _stopwatchStart = 0;
    _lapsSerialized = ref new String(L"");
    SaveStopwatch();
    RebuildLapList();
    UpdateStopwatch();
    UpdateStopwatchButtons();
}

void MainPage::SetTimerDuration(int seconds)
{
    if (seconds < 0) seconds = 0;
    if (seconds > 86399) seconds = 86399;
    SetPicked(seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

void MainPage::QuickTimer_Click(Object^ sender, RoutedEventArgs^)
{
    if (_timerState != 0 || _timerExpired) return;
    Button^ button = dynamic_cast<Button^>(sender);
    if (button == nullptr) return;
    String^ tag = dynamic_cast<String^>(button->Tag);
    if (tag == nullptr) return;
    long seconds = wcstol(tag->Data(), nullptr, 10);
    SetTimerDuration(static_cast<int>(seconds));
    UpdateTimer();
}

void MainPage::UpdateTimer()
{
    if (_timerExpired)
    {
        if (_timerRinging)
        {
            TimerTextBlock->Text = L"Tempo scaduto";
        }
        else
        {
            SYSTEMTIME local = LocalSystemTimeFromFileTicks(_timerExpiredAt);
            std::wstring message = L"Tempo scaduto alle ";
            message += TwoDigits(local.wHour);
            message += L":";
            message += TwoDigits(local.wMinute);
            TimerTextBlock->Text = MakeString(message);
        }
        UpdateTimerButtons();
        return;
    }

    int64_t remaining = 0;
    if (_timerState == 1)
    {
        int64_t ticks = _timerEnd - WallClockTicks();
        remaining = ticks > 0 ? ticks : 0;
    }
    else if (_timerState == 2)
    {
        remaining = _timerPausedLeft > 0 ? _timerPausedLeft : 0;
    }
    else
    {
        remaining = _timerTotal;
        if (remaining <= 0)
        {
            int hours = _pickHours;
            int minutes = _pickMinutes;
            int seconds = _pickSeconds;
            remaining = static_cast<int64_t>(hours * 3600 + minutes * 60 + seconds) *
                        TicksPerSecond;
        }
    }

    TimerTextBlock->Text = MakeString(FormatTimer(remaining));
    UpdateTimerButtons();
}

void MainPage::UpdateTimerButtons()
{
    if (_timerExpired)
    {
        TimerActionButton->Content = L"ferma";
        TimerActionButton->IsEnabled = true;
    }
    else if (_timerState == 1)
    {
        TimerActionButton->Content = L"pausa";
        TimerActionButton->IsEnabled = true;
    }
    else if (_timerState == 2)
    {
        TimerActionButton->Content = L"riprendi";
        TimerActionButton->IsEnabled = true;
    }
    else
    {
        TimerActionButton->Content = L"avvia";
        TimerActionButton->IsEnabled = true;
    }

    TimerCancelButton->IsEnabled = _timerState != 0 || _timerExpired;
    // a panel has no IsEnabled on 8.1: hide the pickers while the timer runs
    TimerPickerPanel->Visibility = _timerState == 0 && !_timerExpired ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::TimerAction_Click(Object^, RoutedEventArgs^)
{
    if (_timerExpired)
    {
        StopTimer();
        return;
    }

    if (_timerState == 0)
        StartTimer();
    else if (_timerState == 1)
        PauseTimer();
    else
        ResumeTimer();
}

void MainPage::TimerCancel_Click(Object^, RoutedEventArgs^)
{
    if (_timerState != 0 || _timerExpired)
        StopTimer();
}

void MainPage::StartTimer()
{
    int hours = _pickHours;
    int minutes = _pickMinutes;
    int seconds = _pickSeconds;
    int totalSeconds = hours * 3600 + minutes * 60 + seconds;
    if (totalSeconds <= 0) return;

    RemoveTimerToast();
    _timerTotal = static_cast<int64_t>(totalSeconds) * TicksPerSecond;
    _timerEnd = WallClockTicks() + _timerTotal;
    _timerPausedLeft = 0;
    _timerState = 1;
    _timerExpired = false;
    _timerRinging = false;
    SaveTimer();
    ScheduleTimerToast();
    UpdateTimer();
}

void MainPage::PauseTimer()
{
    int64_t now = WallClockTicks();
    if (now >= _timerEnd)
    {
        BeginTimerRinging();
        return;
    }

    _timerPausedLeft = _timerEnd - now;
    _timerState = 2;
    RemoveTimerToast();
    SaveTimer();
    UpdateTimer();
}

void MainPage::ResumeTimer()
{
    if (_timerPausedLeft <= 0)
    {
        StopTimer();
        return;
    }

    _timerEnd = WallClockTicks() + _timerPausedLeft;
    _timerState = 1;
    SaveTimer();
    ScheduleTimerToast();
    UpdateTimer();
}

void MainPage::StopTimer()
{
    RemoveTimerToast();
    StopTimerRinging();
    _timerState = 0;
    _timerEnd = 0;
    _timerPausedLeft = 0;
    _timerTotal = 0;
    _timerExpiredAt = 0;
    _timerExpired = false;
    _timerRinging = false;
    SaveTimer();
    UpdateTimer();
}

void MainPage::BeginTimerRinging()
{
    if (_timerExpired) return;

    _timerExpired = true;
    _timerExpiredAt = _timerEnd;
    _timerRinging = true;
    _ringStartedAt = WallClockTicks();
    _lastVibrationAt = 0;
    RemoveTimerToast();
    try { AlarmPlayer->Position = TimeSpan(); AlarmPlayer->Play(); }
    catch (...) {}
    Tabs->SelectedIndex = 2;
    UpdateTimer();
}

void MainPage::StopTimerRinging()
{
    _timerRinging = false;
    _lastVibrationAt = 0;
    try { AlarmPlayer->Stop(); }
    catch (...) {}
    try
    {
        auto vibration = VibrationDevice::GetDefault();
        if (vibration != nullptr) vibration->Cancel();
    }
    catch (...) {}
}

void MainPage::ScheduleTimerToast()
{
    int64_t now = WallClockTicks();
    if (_timerEnd <= now + TicksPerSecond) return;

    try
    {
        XmlDocument^ xml = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastText02);
        XmlNodeList^ textElements = xml->GetElementsByTagName(L"text");
        if (textElements->Length >= 2)
        {
            textElements->Item(0)->AppendChild(xml->CreateTextNode(L"Orologio"));
            int64_t totalSeconds = _timerTotal / TicksPerSecond;
            std::wostringstream duration;
            if (totalSeconds % 60 == 0)
                duration << (totalSeconds / 60) << L" min";
            else
                duration << totalSeconds << L" s";
            std::wstring message = L"Tempo scaduto (";
            message += duration.str();
            message += L")";
            textElements->Item(1)->AppendChild(xml->CreateTextNode(MakeString(message)));
        }

        XmlElement^ toastElement = dynamic_cast<XmlElement^>(xml->GetElementsByTagName(L"toast")->Item(0));
        if (toastElement != nullptr)
        {
            toastElement->SetAttribute(L"launch", L"timer");
            XmlElement^ audio = xml->CreateElement(L"audio");
            audio->SetAttribute(L"src", L"ms-winsoundevent:Notification.Reminder");
            toastElement->AppendChild(audio);
        }

        DateTime due;
        due.UniversalTime = _timerEnd;
        ScheduledToastNotification^ scheduled = ref new ScheduledToastNotification(xml, due);
        scheduled->Id = L"timer";
        ToastNotificationManager::CreateToastNotifier()->AddToSchedule(scheduled);
    }
    catch (...) {}
}

void MainPage::RemoveTimerToast()
{
    try
    {
        auto notifier = ToastNotificationManager::CreateToastNotifier();
        auto scheduled = notifier->GetScheduledToastNotifications();
        for (unsigned int i = 0; i < scheduled->Size; ++i)
        {
            ScheduledToastNotification^ item = scheduled->GetAt(i);
            if (item->Id == L"timer")
                notifier->RemoveFromSchedule(item);
        }
    }
    catch (...) {}
}

void MainPage::Sync_Click(Object^, RoutedEventArgs^)
{
    StartNtpAttempt(false);
}

void MainPage::StartNtpAttempt(bool fallback)
{
    _ntpTryingFallback = fallback;
    ++_ntpRequestId;
    unsigned int requestId = _ntpRequestId;

    CloseNtpSocket();
    _ntpTimeout->Stop();
    _ntpTimeout->Start();
    SetNtpStatus(ref new String(L"verifica dell'ora\u2026"));

    try
    {
        DatagramSocket^ socket = ref new DatagramSocket();
        _ntpSocket = socket;
        socket->MessageReceived += ref new TypedEventHandler<DatagramSocket^, DatagramSocketMessageReceivedEventArgs^>(
            this, &MainPage::NtpMessageReceived);

        String^ hostName = fallback ? L"pool.ntp.org" : L"time.google.com";
        HostName^ host = ref new HostName(hostName);
        create_task(socket->ConnectAsync(host, L"123")).then(
            [this, socket, requestId](task<void> connected)
        {
            try
            {
                connected.get();
                if (requestId != _ntpRequestId) return;

                uint8_t raw[48];
                ClockText::BuildNtpRequest(raw);
                auto packet = ref new Array<uint8_t>(48);
                for (unsigned int i = 0; i < 48; ++i)
                    packet[i] = raw[i];

                DataWriter^ writer = ref new DataWriter(socket->OutputStream);
                writer->WriteBytes(packet);
                _ntpT0 = UnixMillisecondsNow();

                create_task(writer->StoreAsync()).then(
                    [this, socket, requestId, writer](task<unsigned int> sent)
                {
                    try
                    {
                        sent.get();
                    }
                    catch (...)
                    {
                        FailNtpAttempt(requestId);
                    }
                });
            }
            catch (...)
            {
                FailNtpAttempt(requestId);
            }
        });
    }
    catch (...)
    {
        FailNtpAttempt(requestId);
    }
}

void MainPage::NtpTimeout_Tick(Object^, Object^)
{
    FailNtpAttempt(_ntpRequestId);
}

void MainPage::NtpMessageReceived(DatagramSocket^ sender, DatagramSocketMessageReceivedEventArgs^ e)
{
    unsigned int requestId = _ntpRequestId;
    if (sender != _ntpSocket) return;

    try
    {
        // GetDataReader throws when the server port is unreachable (ICMP)
        if (e->GetDataReader()->UnconsumedBufferLength < 48) throw ref new FailureException();
        DataReader^ reader = e->GetDataReader();
        auto data = ref new Array<uint8_t>(48);
        reader->ReadBytes(data);

        int64_t transmitNtpMs = 0;
        if (!ClockText::ParseNtpTransmit(data->Data, data->Length, transmitNtpMs))
            throw ref new FailureException();

        int64_t receiveNtpMs = NtpTimestampMilliseconds(data->Data + 32);
        int64_t t1 = ClockText::UnixMsFromNtpMs(receiveNtpMs);
        int64_t t2 = ClockText::UnixMsFromNtpMs(transmitNtpMs);
        int64_t t3 = UnixMillisecondsNow();
        int64_t offset = ClockText::NtpOffsetMs(_ntpT0, t1, t2, t3);
        // the answer arrives on a socket thread: the page and its timers belong to the UI thread
        Dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
            ref new Windows::UI::Core::DispatchedHandler([this, requestId, offset]() { CompleteNtpAttempt(requestId, offset); }));
    }
    catch (...)
    {
        Dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal,
            ref new Windows::UI::Core::DispatchedHandler([this, requestId]() { FailNtpAttempt(requestId); }));
    }
}

void MainPage::FailNtpAttempt(unsigned int requestId)
{
    if (requestId != _ntpRequestId) return;

    _ntpTimeout->Stop();
    CloseNtpSocket();

    if (!_ntpTryingFallback)
    {
        StartNtpAttempt(true);
        return;
    }

    SetNtpStatus(ref new String(L"ora non verificata (rete)"));
}

void MainPage::CompleteNtpAttempt(unsigned int requestId, int64_t offsetMs)
{
    if (requestId != _ntpRequestId) return;

    _ntpTimeout->Stop();
    CloseNtpSocket();
    _ntpOffsetMs = offsetMs;
    _ntpCheckedAt = WallClockTicks();
    _hasNtpOffset = true;

    auto values = _settings->Values;
    values->Insert(L"ntpOffsetMs", ToObject(_ntpOffsetMs));
    values->Insert(L"ntpCheckedAt", ToObject(_ntpCheckedAt));

    SetNtpStatus(MakeString(CheckedStatus(_ntpOffsetMs, _ntpCheckedAt)));
    UpdateClock(true);
}

void MainPage::CloseNtpSocket()
{
    if (_ntpSocket != nullptr)
    {
        try { delete _ntpSocket; }
        catch (...) {}
        _ntpSocket = nullptr;
    }
}

void MainPage::SetPicked(int hours, int minutes, int seconds)
{
    _pickHours = hours;
    _pickMinutes = minutes;
    _pickSeconds = seconds;
    HoursBox->Content = MakeString(TwoDigits(hours));
    MinutesBox->Content = MakeString(TwoDigits(minutes));
    SecondsBox->Content = MakeString(TwoDigits(seconds));
}

// ore/minuti/secondi: a full-page list of the values, the chosen one goes back on the button
void MainPage::PickTime_Click(Object^ sender, RoutedEventArgs^)
{
    auto button = dynamic_cast<Button^>(sender);
    if (button == nullptr) return;
    std::wstring which = WideString(dynamic_cast<String^>(button->Tag));
    int count = which == L"h" ? 24 : 60;
    int current = which == L"h" ? _pickHours : (which == L"m" ? _pickMinutes : _pickSeconds);

    auto values = ref new Platform::Collections::Vector<String^>();
    for (int i = 0; i < count; ++i) values->Append(MakeString(TwoDigits(i)));
    auto flyout = ref new ListPickerFlyout();
    Windows::UI::Xaml::Controls::Primitives::PickerFlyoutBase::SetTitle(flyout,
        which == L"h" ? L"ORE" : (which == L"m" ? L"MINUTI" : L"SECONDI"));
    flyout->ItemsSource = values;
    flyout->SelectedIndex = current;
    flyout->ItemsPicked += ref new Windows::Foundation::TypedEventHandler<ListPickerFlyout^, ItemsPickedEventArgs^>(
        [this, which](ListPickerFlyout^ picker, ItemsPickedEventArgs^)
    {
        int value = picker->SelectedIndex;
        if (value < 0) return;
        if (which == L"h") SetPicked(value, _pickMinutes, _pickSeconds);
        else if (which == L"m") SetPicked(_pickHours, value, _pickSeconds);
        else SetPicked(_pickHours, _pickMinutes, value);
        UpdateTimer();
    });
    flyout->ShowAt(button);
}

namespace
{
    const wchar_t* const DayLetters[7] = {L"L", L"M", L"M", L"G", L"V", L"S", L"D"};
    const wchar_t* const DayNames[7] = {L"lun", L"mar", L"mer", L"gio", L"ven", L"sab", L"dom"};

    std::wstring DaysText(const AlarmInfo& a)
    {
        if (a.days == 0x7f) return L"ogni giorno";
        if (a.days == 0x1f) return L"lun\u2013ven";
        if (a.days == 0)
        {
            wchar_t date[32];
            swprintf_s(date, L"una volta, %02d/%02d", a.day, a.month);
            return date;
        }
        std::wstring text;
        for (int i = 0; i < 7; ++i)
            if (a.days & (1 << i)) text += (text.empty() ? L"" : L", ") + std::wstring(DayNames[i]);
        return text;
    }

    TextBlock^ Text(const std::wstring& text, double size, double opacity)
    {
        auto block = ref new TextBlock();
        block->Text = ref new String(text.c_str());
        block->FontSize = size;
        block->Opacity = opacity;
        block->TextTrimming = TextTrimming::CharacterEllipsis;
        return block;
    }
}

void MainPage::Tabs_SelectionChanged(Object^, SelectionChangedEventArgs^)
{
    bool alarms = Tabs->SelectedIndex == 3;
    AddAlarmButton->Visibility = alarms ? Windows::UI::Xaml::Visibility::Visible : Windows::UI::Xaml::Visibility::Collapsed;
    if (alarms) RefreshAlarms();
}

void MainPage::RefreshAlarms()
{
    std::wstring error;
    _alarms = Alarms::List(error);
    std::sort(_alarms.begin(), _alarms.end(), [](const AlarmInfo& a, const AlarmInfo& b)
    {
        return a.hour * 60 + a.minute < b.hour * 60 + b.minute;
    });
    AlarmsList->Items->Clear();
    for (size_t i = 0; i < _alarms.size(); ++i)
    {
        const AlarmInfo& a = _alarms[i];
        auto row = ref new Grid();
        row->Margin = Thickness(0, 4, 0, 12);
        row->Tag = static_cast<int>(i);
        auto textColumn = ref new ColumnDefinition();
        textColumn->Width = GridLength(1, GridUnitType::Star);
        row->ColumnDefinitions->Append(textColumn);
        auto switchColumn = ref new ColumnDefinition();
        switchColumn->Width = GridLength::Auto;
        row->ColumnDefinitions->Append(switchColumn);

        auto texts = ref new StackPanel();
        wchar_t time[8];
        swprintf_s(time, L"%02d:%02d", a.hour, a.minute);
        auto big = Text(time, 48, a.enabled ? 1.0 : 0.45);
        big->FontWeight = Windows::UI::Text::FontWeights::Light;
        texts->Children->Append(big);
        std::wstring detail = (a.label.empty() ? L"Sveglia" : a.label) + L" \u00b7 " + DaysText(a);
        texts->Children->Append(Text(detail, 17, a.enabled ? 0.75 : 0.4));
        row->Children->Append(texts);

        auto toggle = ref new ToggleSwitch();
        toggle->IsOn = a.enabled;
        toggle->OnContent = L"";
        toggle->OffContent = L"";
        toggle->MinWidth = 0;
        toggle->VerticalAlignment = Windows::UI::Xaml::VerticalAlignment::Center;
        Grid::SetColumn(toggle, 1);
        AlarmInfo copy = a;
        toggle->Toggled += ref new RoutedEventHandler([this, copy, toggle](Object^, RoutedEventArgs^) mutable
        {
            if (copy.enabled == toggle->IsOn) return;
            copy.enabled = toggle->IsOn;
            if (copy.enabled) copy.year = 0;  // next ring from now
            std::wstring error;
            if (Alarms::Save(copy, error) < 0) ShowMessage(L"Sveglia non modificata: " + error);
            RefreshAlarms();
        });
        row->Children->Append(toggle);

        int slot = a.slot;
        row->Holding += ref new Windows::UI::Xaml::Input::HoldingEventHandler(
            [this, slot, row](Object^, Windows::UI::Xaml::Input::HoldingRoutedEventArgs^ e)
        {
            if (e->HoldingState != Windows::UI::Input::HoldingState::Started) return;
            auto menu = ref new MenuFlyout();
            auto remove = ref new MenuFlyoutItem();
            remove->Text = L"elimina";
            remove->Click += ref new RoutedEventHandler([this, slot](Object^, RoutedEventArgs^) { DeleteAlarm(slot); });
            menu->Items->Append(remove);
            menu->ShowAt(row);
        });
        AlarmsList->Items->Append(row);
    }
    AlarmsEmpty->Text = ref new String((error.empty() ? L"nessuna sveglia: tocca + per crearne una"
                                                      : L"sveglie non disponibili: " + error).c_str());
    AlarmsEmpty->Visibility = _alarms.empty() ? Windows::UI::Xaml::Visibility::Visible
                                              : Windows::UI::Xaml::Visibility::Collapsed;
}

void MainPage::Alarm_Click(Object^, ItemClickEventArgs^ e)
{
    auto row = dynamic_cast<Grid^>(e->ClickedItem);
    if (row == nullptr || row->Tag == nullptr) return;
    int index = safe_cast<int>(row->Tag);
    if (index >= 0 && index < static_cast<int>(_alarms.size())) EditAlarm(_alarms[index]);
}

void MainPage::AddAlarm_Click(Object^, RoutedEventArgs^)
{
    AlarmInfo a = {};
    a.slot = -1;
    a.hour = 7;
    a.minute = 0;
    a.enabled = true;
    EditAlarm(a);
}

void MainPage::EditAlarm(AlarmInfo alarm)
{
    auto dialog = ref new ContentDialog();
    dialog->Title = alarm.slot < 0 ? L"nuova sveglia" : L"modifica sveglia";
    dialog->PrimaryButtonText = L"salva";
    dialog->SecondaryButtonText = L"annulla";

    auto panel = ref new StackPanel();
    auto picker = ref new TimePicker();
    picker->ClockIdentifier = L"24HourClock";
    TimeSpan time;
    time.Duration = (static_cast<int64_t>(alarm.hour) * 60 + alarm.minute) * 60 * 10000000LL;
    picker->Time = time;
    panel->Children->Append(picker);

    auto label = ref new TextBox();
    label->PlaceholderText = L"nome (facoltativo)";
    label->Text = ref new String(alarm.label.c_str());
    label->Margin = Thickness(0, 8, 0, 8);
    panel->Children->Append(label);

    panel->Children->Append(Text(L"ripeti (nessun giorno = una volta)", 15, 0.7));
    auto days = ref new Grid();
    std::vector<Windows::UI::Xaml::Controls::Primitives::ToggleButton^> buttons;
    for (int i = 0; i < 7; ++i)
    {
        auto column = ref new ColumnDefinition();
        column->Width = GridLength(1, GridUnitType::Star);
        days->ColumnDefinitions->Append(column);
        auto button = ref new Windows::UI::Xaml::Controls::Primitives::ToggleButton();
        button->Content = ref new String(DayLetters[i]);
        button->IsChecked = (alarm.days & (1 << i)) != 0;
        button->MinWidth = 0;
        button->Padding = Thickness(0);
        button->Margin = Thickness(i == 0 ? 0 : 2, 0, i == 6 ? 0 : 2, 0);
        button->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
        Grid::SetColumn(button, i);
        days->Children->Append(button);
        buttons.push_back(button);
    }
    panel->Children->Append(days);
    dialog->Content = panel;

    create_task(dialog->ShowAsync()).then([this, alarm, picker, label, buttons](ContentDialogResult result) mutable
    {
        if (result != ContentDialogResult::Primary) return;
        int64_t minutes = picker->Time.Duration / 600000000LL;
        alarm.hour = static_cast<int>(minutes / 60) % 24;
        alarm.minute = static_cast<int>(minutes % 60);
        alarm.label = WideString(label->Text);
        alarm.days = 0;
        for (int i = 0; i < 7; ++i)
            if (buttons[i]->IsChecked != nullptr && buttons[i]->IsChecked->Value) alarm.days |= 1 << i;
        alarm.year = 0;  // next ring from now
        alarm.enabled = true;
        std::wstring error;
        if (Alarms::Save(alarm, error) < 0) ShowMessage(L"Sveglia non salvata: " + error);
        RefreshAlarms();
    }, task_continuation_context::use_current());
}

void MainPage::DeleteAlarm(int slot)
{
    std::wstring error;
    if (!Alarms::Remove(slot, error)) ShowMessage(L"Sveglia non eliminata: " + error);
    RefreshAlarms();
}
