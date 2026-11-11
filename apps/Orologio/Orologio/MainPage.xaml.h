#pragma once

#include "MainPage.g.h"
#include "Alarms.h"

namespace Orologio
{
    public ref class MainPage sealed
    {
    public:
        MainPage();

        bool HandleHardwareBack();
        void OnLaunchArguments(Platform::String^ args);

    protected:
        virtual void OnNavigatedTo(Windows::UI::Xaml::Navigation::NavigationEventArgs^ e) override;

    private:
        Windows::Storage::ApplicationDataContainer^ _settings;
        Windows::UI::Xaml::DispatcherTimer^ _clockTimer;
        Windows::UI::Xaml::DispatcherTimer^ _stopwatchTimer;
        Windows::UI::Xaml::DispatcherTimer^ _timerTimer;
        Windows::UI::Xaml::DispatcherTimer^ _ntpTimeout;
        Windows::Networking::Sockets::DatagramSocket^ _ntpSocket;

        bool _restored;
        bool _clockHasSecond;
        bool _hasNtpOffset;
        bool _ntpTryingFallback;
        bool _stopwatchRunning;
        bool _timerExpired;
        bool _timerRinging;

        int64_t _ntpOffsetMs;
        int64_t _ntpCheckedAt;
        int64_t _lastClockSecond;
        int64_t _ntpT0;
        int64_t _stopwatchStart;
        int64_t _stopwatchAccum;
        int64_t _timerEnd;
        int64_t _timerPausedLeft;
        int64_t _timerTotal;
        int64_t _timerExpiredAt;
        int64_t _ringStartedAt;
        int64_t _lastVibrationAt;

        int _timerState;
        unsigned int _ntpRequestId;
        Platform::String^ _lapsSerialized;

        void ClockTimer_Tick(Platform::Object^ sender, Platform::Object^ e);
        void StopwatchTimer_Tick(Platform::Object^ sender, Platform::Object^ e);
        void TimerTimer_Tick(Platform::Object^ sender, Platform::Object^ e);
        void NtpTimeout_Tick(Platform::Object^ sender, Platform::Object^ e);
        void NtpMessageReceived(Windows::Networking::Sockets::DatagramSocket^ sender,
                                Windows::Networking::Sockets::DatagramSocketMessageReceivedEventArgs^ e);

        void Sync_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void StopwatchToggle_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void StopwatchLap_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void StopwatchReset_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void QuickTimer_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void PickTime_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void SetPicked(int hours, int minutes, int seconds);
        int _pickHours, _pickMinutes, _pickSeconds;
        void TimerAction_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void TimerCancel_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);

        void RestoreSettings();
        void RestoreStopwatch();
        void RestoreTimer();
        void LoadNtpState();
        void SaveStopwatch();
        void SaveTimer();
        void SetTimerDuration(int seconds);
        void UpdateClock(bool force);
        void UpdateStopwatch();
        void UpdateStopwatchButtons();
        void UpdateTimer();
        void UpdateTimerButtons();
        void RebuildLapList();
        void StartNtpAttempt(bool fallback);
        void FailNtpAttempt(unsigned int requestId);
        void CompleteNtpAttempt(unsigned int requestId, int64_t offsetMs);
        void CloseNtpSocket();
        void StartTimer();
        void PauseTimer();
        void ResumeTimer();
        void StopTimer();
        void BeginTimerRinging();
        void StopTimerRinging();
        void ScheduleTimerToast();
        void RemoveTimerToast();
        void SetNtpStatus(Platform::String^ text);
        void CreateAlarmFromUri(Platform::String^ text);
        void ShowAlarmList();

        std::vector<AlarmInfo> _alarms;
        void Tabs_SelectionChanged(Platform::Object^ sender, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ e);
        void AddAlarm_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ e);
        void Alarm_Click(Platform::Object^ sender, Windows::UI::Xaml::Controls::ItemClickEventArgs^ e);
        void RefreshAlarms();
        void EditAlarm(AlarmInfo alarm);
        void DeleteAlarm(int slot);
    };
}
