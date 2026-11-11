#pragma once

#include <collection.h>
#include <vector>

namespace MusicaAudioTask
{
    public ref class AudioTask sealed : Windows::ApplicationModel::Background::IBackgroundTask
    {
    public:
        virtual void Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ taskInstance);

    private:
        void OnMessageReceived(Platform::Object^ sender,
                               Windows::Media::Playback::MediaPlayerDataReceivedEventArgs^ args);
        void OnMediaEnded(Windows::Media::Playback::MediaPlayer^ sender, Platform::Object^ args);
        void OnButtonPressed(Windows::Media::SystemMediaTransportControls^ sender,
                             Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs^ args);
        void OnCanceled(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ sender,
                        Windows::ApplicationModel::Background::BackgroundTaskCancellationReason reason);
        void HandleMessage(Windows::Foundation::Collections::ValueSet^ data);
        void StartCurrent();
        void Advance(bool previous);
        void SendState();
        void SavePlaybackState();
        void UpdateMetadata(Windows::Storage::StorageFile^ file, unsigned int loadToken);

        Windows::ApplicationModel::Background::BackgroundTaskDeferral^ _deferral;
        Windows::Media::Playback::MediaPlayer^ _player;
        Windows::Media::SystemMediaTransportControls^ _controls;
        std::vector<Platform::String^> _paths;
        unsigned int _index;
        unsigned int _loadToken;
        int _repeat;
        bool _shuffle;
        bool _canceled;
        bool _hasTrack;
        bool _isPlaying;
        long long _positionTicks;
        long long _durationTicks;
        Platform::String^ _title;
        Platform::String^ _artist;
        Platform::String^ _albumArtist;
        Platform::String^ _album;
    };
}
