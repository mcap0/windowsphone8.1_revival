#ifndef PERLECAST_TASKLINK_H
#define PERLECAST_TASKLINK_H

#include <functional>
#include <string>

#include <windows.foundation.collections.h>

namespace PerleCast {

// The app's side of the background task (CastTask). Call on the UI thread; handlers run on the UI thread.
namespace TaskLink {

// Sends a message, starting the task first when needed (messages wait until it says "ready").
void Send(Windows::Foundation::Collections::ValueSet^ message);
// Stops the task when neither casting nor background audio need it.
void ReleaseIfIdle();
// Receives every message of the task: command, data.
void AddHandler(std::function<void(const std::wstring&, const std::string&)> handler);

} // namespace TaskLink

// "audio in sottofondo": the audio of a video played by the background task, also with Perle TV in the background.
public ref class BackgroundAudio sealed
{
public:
    // source: a local file path or a stream URL; the position is saved as "pos:<videoUrl>" (shared with the player).
    static void Play(Platform::String^ source, Platform::String^ videoUrl, Platform::String^ title,
                     Platform::String^ uploader, double startSeconds);
    // Stops it; StatusChanged then says "stopped" (position saved).
    static void Stop();
    // Perle TV is being closed (Back on the main page): the task stops everything.
    static void AppClosing();
    static property bool IsActive { bool get(); }
    static property Platform::String^ VideoUrl { Platform::String^ get(); }
    // "playing", "ended", "stopped", "error: <text>" (UI thread)
    static event Windows::Foundation::EventHandler<Platform::String^>^ StatusChanged;

internal:
    static void OnTaskMessage(const std::wstring& command, const std::string& data);

private:
    BackgroundAudio();
};

} // namespace PerleCast

#endif
