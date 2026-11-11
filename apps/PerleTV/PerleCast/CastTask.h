#ifndef PERLECAST_CASTTASK_H
#define PERLECAST_CASTTASK_H

#include <collection.h>
#include <mutex>
#include <string>

namespace PerleCast {

class CastSession;
enum CastTaskMode { ModeIdle, ModeCast, ModeAudio };

// Background audio task: the only part of a WP8.1 app that keeps running when the app is in the background or the
// screen is locked. Two jobs, one at a time:
// - casting: hosts the CastSession (stream server + TV) and plays silence so that the process stays alive;
// - audio: plays the audio of a video ("audio in sottofondo" in the player).
// Both stop when Perle TV is closed (its AppAliveMutex disappears); Start / screen lock leave them running.
public ref class CastTask sealed : Windows::ApplicationModel::Background::IBackgroundTask
{
public:
    virtual void Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ taskInstance);

private:
    void OnCanceled(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ sender,
                    Windows::ApplicationModel::Background::BackgroundTaskCancellationReason reason);
    void OnMessage(Platform::Object^ sender, Windows::Media::Playback::MediaPlayerDataReceivedEventArgs^ args);
    void OnButton(Windows::Media::SystemMediaTransportControls^ sender,
                  Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs^ args);
    void OnMediaOpened(Windows::Media::Playback::MediaPlayer^ sender, Platform::Object^ args);
    void OnMediaEnded(Windows::Media::Playback::MediaPlayer^ sender, Platform::Object^ args);
    void OnMediaFailed(Windows::Media::Playback::MediaPlayer^ sender,
                       Windows::Media::Playback::MediaPlayerFailedEventArgs^ args);
    void OnStateChanged(Windows::Media::Playback::MediaPlayer^ sender, Platform::Object^ args);
    void OnTick(Windows::System::Threading::ThreadPoolTimer^ timer);

    void HandleCast(const std::wstring& command, const std::string& data);
    void StartAudio(const std::string& data);
    void StopAudio(const char* status);
    void PlaySilence();
    void ShowOnLockScreen(const std::string& title, const std::string& artist);
    void SendState();
    void Finish(const std::string& reason);
    long long AudioSeconds();

    Windows::ApplicationModel::Background::BackgroundTaskDeferral^ deferral_;
    Windows::Media::Playback::MediaPlayer^ player_;
    Windows::Media::SystemMediaTransportControls^ controls_;
    Windows::System::Threading::ThreadPoolTimer^ timer_;
    CastSession* session_;
    std::recursive_mutex lock_;
    CastTaskMode mode_;
    bool finished_;
    bool appSeen_;     // the app's mutex was seen once: only then its absence means "closed"
    int appMissing_;   // ticks in a row without it
    int ticks_;
    std::string tvTitle_;
    // audio mode
    std::string audioVideoUrl_;
    long long audioStart_;  // seconds, applied when the source is open
};

} // namespace PerleCast

#endif
