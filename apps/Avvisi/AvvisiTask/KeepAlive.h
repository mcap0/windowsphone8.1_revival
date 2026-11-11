#pragma once

namespace AvvisiTask
{
    // Background audio task: plays one second of silence in a loop so that WP8.1 keeps this process alive with the
    // screen off, and every 2 minutes runs the checks: Telegram (TgPoll) and Gmail (MailPoll), with toasts for new messages.
    public ref class KeepAlive sealed : Windows::ApplicationModel::Background::IBackgroundTask
    {
    public:
        virtual void Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ taskInstance);

    private:
        void Tick();
        void Resume(const wchar_t* why);
        void OnCanceled(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ sender,
                        Windows::ApplicationModel::Background::BackgroundTaskCancellationReason reason);
        void OnStateChanged(Windows::Media::Playback::MediaPlayer^ sender, Platform::Object^ args);
        void OnFailed(Windows::Media::Playback::MediaPlayer^ sender,
                      Windows::Media::Playback::MediaPlayerFailedEventArgs^ args);
        void OnMessage(Platform::Object^ sender, Windows::Media::Playback::MediaPlayerDataReceivedEventArgs^ args);
        void OnButton(Windows::Media::SystemMediaTransportControls^ sender,
                      Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs^ args);
        void Finish(const wchar_t* why);

        Windows::ApplicationModel::Background::BackgroundTaskDeferral^ deferral_;
        Windows::System::Threading::ThreadPoolTimer^ timer_;
        Windows::Media::Playback::MediaPlayer^ player_;
        Windows::Media::SystemMediaTransportControls^ controls_;
        unsigned int ticks_;
        volatile long busy_;
        long long started_;
        int batteryStart_;
        bool userPaused_;
        bool finished_;
    };
}
