#define NOMINMAX
#include <collection.h>
#include <ppltasks.h>
#include "AudioTask.h"

#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <string>

using namespace MusicaAudioTask;
using namespace Platform;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::Data;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Media;
using namespace Windows::Media::Playback;
using namespace Windows::Storage;
using namespace Windows::Storage::FileProperties;
using namespace concurrency;

namespace
{
    String^ CleanText(String^ value, String^ fallback)
    {
        return value == nullptr || value->Length() == 0 ? fallback : value;
    }

    int ClampRepeat(int repeat)
    {
        return std::max(0, std::min(2, repeat));
    }

    // what the player did with each file, in LocalState\\audio.log (read from the PC: ISETool export of Musica)
    void TaskLog(String^ line)
    {
        try
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t stamp[32];
            swprintf_s(stamp, L"%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            std::wstring path = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\audio.log";
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || f == nullptr) return;
            if (_ftelli64(f) > 128 * 1024) { fclose(f); if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || f == nullptr) return; }
            std::wstring w = std::wstring(stamp) + (line == nullptr ? L"" : line->Data()) + L"\r\n";
            int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string u(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &u[0], n, nullptr, nullptr);
            fwrite(u.data(), 1, u.size(), f);
            fclose(f);
        }
        catch (...) {}
    }

    String^ Hex(int v)
    {
        wchar_t b[16];
        swprintf_s(b, L"0x%08X", static_cast<unsigned>(v));
        return ref new String(b);
    }
}

void AudioTask::Run(IBackgroundTaskInstance^ taskInstance)
{
    _deferral = taskInstance->GetDeferral();
    _player = nullptr;
    _controls = nullptr;
    _paths.clear();
    _index = 0;
    _loadToken = 0;
    _repeat = 0;
    _shuffle = false;
    _canceled = false;
    _hasTrack = false;
    _isPlaying = false;
    _positionTicks = 0;
    _durationTicks = 0;
    _title = "Nessun brano";
    _artist = "";
    _albumArtist = "";
    _album = "";

    taskInstance->Canceled +=
        ref new BackgroundTaskCanceledEventHandler(this, &AudioTask::OnCanceled);

    try
    {
        _player = BackgroundMediaPlayer::Current;
        _player->MediaEnded +=
            ref new TypedEventHandler<MediaPlayer^, Object^>(this, &AudioTask::OnMediaEnded);
        _player->MediaOpened += ref new TypedEventHandler<MediaPlayer^, Object^>([](MediaPlayer^ p, Object^)
        {
            TaskLog("opened, duration " + (p->NaturalDuration.Duration / 10000).ToString() + " ms");
        });
        _player->MediaFailed += ref new TypedEventHandler<MediaPlayer^, MediaPlayerFailedEventArgs^>([](MediaPlayer^, MediaPlayerFailedEventArgs^ e)
        {
            TaskLog("FAILED error " + static_cast<int>(e->Error).ToString() + " " + Hex(e->ExtendedErrorCode.Value) + " " + e->ErrorMessage);
        });

        _controls = SystemMediaTransportControls::GetForCurrentView();
        _controls->IsEnabled = true;
        _controls->IsPlayEnabled = true;
        _controls->IsPauseEnabled = true;
        _controls->IsNextEnabled = true;
        _controls->IsPreviousEnabled = true;
        _controls->ButtonPressed +=
            ref new TypedEventHandler<SystemMediaTransportControls^,
                SystemMediaTransportControlsButtonPressedEventArgs^>(
                    this, &AudioTask::OnButtonPressed);

        BackgroundMediaPlayer::MessageReceivedFromForeground +=
            ref new EventHandler<MediaPlayerDataReceivedEventArgs^>(
                this, &AudioTask::OnMessageReceived);

        IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
        if (values->HasKey("shuffle"))
            _shuffle = safe_cast<bool>(values->Lookup("shuffle"));
        if (values->HasKey("repeat"))
            _repeat = ClampRepeat(safe_cast<int>(values->Lookup("repeat")));

        ValueSet^ ready = ref new ValueSet();
        ready->Insert("cmd", "ready");
        BackgroundMediaPlayer::SendMessageToForeground(ready);
    }
    catch (Exception^)
    {
    }
}

void AudioTask::OnMessageReceived(Object^, MediaPlayerDataReceivedEventArgs^ args)
{
    if (_canceled || args == nullptr)
        return;
    HandleMessage(args->Data);
}

void AudioTask::HandleMessage(ValueSet^ data)
{
    if (data == nullptr || !data->HasKey("cmd"))
        return;

    try
    {
        String^ command = safe_cast<String^>(data->Lookup("cmd"));

        if (command == "ping")
        {
            ValueSet^ ready = ref new ValueSet();
            ready->Insert("cmd", "ready");
            BackgroundMediaPlayer::SendMessageToForeground(ready);
        }
        else if (command == "queue")
        {
            _paths.clear();
            String^ serialized = data->HasKey("paths")
                ? safe_cast<String^>(data->Lookup("paths")) : "";
            if (serialized == nullptr)
                serialized = "";

            std::wstring all(serialized->Data());
            size_t start = 0;
            while (start < all.size())
            {
                size_t end = all.find(L'\n', start);
                if (end == std::wstring::npos)
                    end = all.size();
                if (end > start)
                    _paths.push_back(ref new String(all.substr(start, end - start).c_str()));
                start = end + 1;
            }

            int requestedIndex = data->HasKey("index")
                ? safe_cast<int>(data->Lookup("index")) : 0;
            _index = requestedIndex < 0 ? 0U : static_cast<unsigned int>(requestedIndex);
            if (_paths.empty() || _index >= _paths.size())
                _index = 0;

            if (data->HasKey("shuffle"))
                _shuffle = safe_cast<bool>(data->Lookup("shuffle"));
            if (data->HasKey("repeat"))
                _repeat = ClampRepeat(safe_cast<int>(data->Lookup("repeat")));
            _positionTicks = data->HasKey("positionTicks")
                ? std::max(0LL, safe_cast<long long>(data->Lookup("positionTicks"))) : 0;

            if (_paths.empty())
            {
                ++_loadToken;
                _hasTrack = false;
                _isPlaying = false;
                _title = "Nessun brano";
                _artist = "";
                _album = "";
                if (_player != nullptr)
                    _player->Pause();
                if (_controls != nullptr)
                    _controls->PlaybackStatus = MediaPlaybackStatus::Stopped;
            }
            SavePlaybackState();
            SendState();
        }
        else if (command == "play")
        {
            StartCurrent();
        }
        else if (command == "stop")
        {
            ++_loadToken;
            _hasTrack = false;
            _isPlaying = false;
            _paths.clear();
            _index = 0;
            _positionTicks = 0;
            _durationTicks = 0;
            _title = "Nessun brano";
            _artist = "";
            _album = "";
            if (_player != nullptr)
                _player->Pause();
            if (_controls != nullptr)
                _controls->PlaybackStatus = MediaPlaybackStatus::Stopped;
            SavePlaybackState();
            SendState();
        }
        else if (command == "state")
        {
            SendState();
        }
        else if (command == "toggle")
        {
            if (_player == nullptr || !_hasTrack)
            {
                SendState();
                return;
            }

            if (_player->CurrentState == MediaPlayerState::Playing)
            {
                _player->Pause();
                _isPlaying = false;
                if (_controls != nullptr)
                    _controls->PlaybackStatus = MediaPlaybackStatus::Paused;
            }
            else
            {
                _player->Play();
                _isPlaying = true;
                if (_controls != nullptr)
                    _controls->PlaybackStatus = MediaPlaybackStatus::Playing;
            }
            SavePlaybackState();
            SendState();
        }
        else if (command == "next")
        {
            Advance(false);
        }
        else if (command == "previous")
        {
            Advance(true);
        }
        else if (command == "seek")
        {
            if (data->HasKey("positionTicks") && _player != nullptr && _hasTrack)
            {
                _positionTicks = std::max(0LL,
                    safe_cast<long long>(data->Lookup("positionTicks")));
                if (_durationTicks > 0)
                    _positionTicks = std::min(_positionTicks, _durationTicks);
                TimeSpan position;
                position.Duration = _positionTicks;
                _player->Position = position;
                SavePlaybackState();
                SendState();
            }
        }
        else if (command == "repeat")
        {
            if (data->HasKey("repeat"))
                _repeat = ClampRepeat(safe_cast<int>(data->Lookup("repeat")));
            SavePlaybackState();
        }
    }
    catch (Exception^)
    {
        SendState();
    }
}

void AudioTask::StartCurrent()
{
    if (_canceled || _player == nullptr || _paths.empty() || _index >= _paths.size())
    {
        _hasTrack = false;
        _isPlaying = false;
        _title = "Nessun brano";
        _artist = "";
        _album = "";
        if (_controls != nullptr)
            _controls->PlaybackStatus = MediaPlaybackStatus::Stopped;
        SendState();
        return;
    }

    const unsigned int token = ++_loadToken;
    String^ path = _paths[_index];
    _hasTrack = true;
    _isPlaying = false;
    _positionTicks = std::max(0LL, _positionTicks);
    _durationTicks = 0;
    _title = "Caricamento";
    _artist = "";
    _album = "";

    try
    {
        if (_controls != nullptr)
            _controls->PlaybackStatus = MediaPlaybackStatus::Changing;

        TaskLog("open " + path);
        create_task(StorageFile::GetFileFromPathAsync(path)).then(
            [this, token](task<StorageFile^> operation)
            {
                StorageFile^ file = nullptr;
                try { file = operation.get(); }
                catch (Exception^ e)
                {
                    TaskLog("file not opened: " + Hex(e->HResult) + " " + e->Message);
                    if (!_canceled && token == _loadToken)
                    {
                        _hasTrack = false;
                        _isPlaying = false;
                        if (_controls != nullptr)
                            _controls->PlaybackStatus = MediaPlaybackStatus::Stopped;
                        SendState();
                    }
                    return;
                }

                if (_canceled || token != _loadToken || file == nullptr)
                    return;
                try
                {
                    _player->SetFileSource(file);
                    TimeSpan position;
                    position.Duration = _positionTicks;
                    if (_positionTicks > 0)
                        _player->Position = position;
                    _player->Play();
                    _isPlaying = true;
                    if (_controls != nullptr)
                        _controls->PlaybackStatus = MediaPlaybackStatus::Playing;
                    SavePlaybackState();
                    SendState();
                    UpdateMetadata(file, token);
                }
                catch (Exception^ e)
                {
                    TaskLog("SetFileSource: " + Hex(e->HResult) + " " + e->Message);
                    _hasTrack = false;
                    _isPlaying = false;
                    if (_controls != nullptr)
                        _controls->PlaybackStatus = MediaPlaybackStatus::Stopped;
                    SendState();
                }
            });
    }
    catch (Exception^)
    {
        _hasTrack = false;
        _isPlaying = false;
        if (_controls != nullptr)
            _controls->PlaybackStatus = MediaPlaybackStatus::Stopped;
        SendState();
    }
}

void AudioTask::Advance(bool previous)
{
    if (_paths.empty())
    {
        _hasTrack = false;
        _isPlaying = false;
        SendState();
        return;
    }

    if (previous)
    {
        if (_index > 0)
            --_index;
        else if (_repeat == 1)
            _index = static_cast<unsigned int>(_paths.size() - 1);
        else
        {
            _positionTicks = 0;
            StartCurrent();
            return;
        }
    }
    else
    {
        if (_paths.size() == 1)
        {
            if (_repeat == 1)
                _index = 0;
            else if (_repeat == 0)
            {
                _isPlaying = false;
                _hasTrack = false;
                if (_player != nullptr)
                    _player->Pause();
                if (_controls != nullptr)
                    _controls->PlaybackStatus = MediaPlaybackStatus::Stopped;
                SavePlaybackState();
                SendState();
                return;
            }
        }
        else if (_shuffle)
        {
            const unsigned int count = static_cast<unsigned int>(_paths.size());
            const unsigned int offset = static_cast<unsigned int>(GetTickCount64()) % (count - 1);
            _index = offset >= _index ? offset + 1 : offset;
        }
        else if (_index + 1 < _paths.size())
            ++_index;
        else if (_repeat == 1)
            _index = 0;
        else
        {
            _isPlaying = false;
            _hasTrack = false;
            if (_player != nullptr)
                _player->Pause();
            if (_controls != nullptr)
                _controls->PlaybackStatus = MediaPlaybackStatus::Stopped;
            SavePlaybackState();
            SendState();
            return;
        }
    }

    _positionTicks = 0;
    StartCurrent();
}

void AudioTask::OnMediaEnded(MediaPlayer^, Object^)
{
    if (_canceled)
        return;
    if (_repeat == 2 && !_paths.empty())
    {
        _positionTicks = 0;
        StartCurrent();
        return;
    }
    Advance(false);
}

void AudioTask::OnButtonPressed(SystemMediaTransportControls^,
                                SystemMediaTransportControlsButtonPressedEventArgs^ args)
{
    if (args == nullptr || _canceled)
        return;
    switch (args->Button)
    {
    case SystemMediaTransportControlsButton::Play:
        if (_player != nullptr && _hasTrack)
        {
            _player->Play();
            _isPlaying = true;
            if (_controls != nullptr)
                _controls->PlaybackStatus = MediaPlaybackStatus::Playing;
            SendState();
        }
        break;
    case SystemMediaTransportControlsButton::Pause:
        if (_player != nullptr)
        {
            _player->Pause();
            _isPlaying = false;
            if (_controls != nullptr)
                _controls->PlaybackStatus = MediaPlaybackStatus::Paused;
            SavePlaybackState();
            SendState();
        }
        break;
    case SystemMediaTransportControlsButton::Next:
        Advance(false);
        break;
    case SystemMediaTransportControlsButton::Previous:
        Advance(true);
        break;
    default:
        break;
    }
}

void AudioTask::UpdateMetadata(StorageFile^ file, unsigned int loadToken)
{
    if (file == nullptr || _controls == nullptr)
        return;
    try
    {
        create_task(file->Properties->GetMusicPropertiesAsync()).then(
            [this, file, loadToken](MusicProperties^ properties)
            {
                if (_canceled || loadToken != _loadToken || properties == nullptr)
                    return;
                _title = CleanText(properties->Title, file->DisplayName);
                _artist = CleanText(properties->Artist, "Sconosciuto");
                _albumArtist = CleanText(properties->AlbumArtist, _artist);
                _album = CleanText(properties->Album, "Sconosciuto");
                try
                {
                    MusicDisplayProperties^ display = _controls->DisplayUpdater->MusicProperties;
                    display->Title = _title;
                    display->Artist = _artist;
                    display->AlbumArtist = _albumArtist;
                    _controls->DisplayUpdater->Type = MediaPlaybackType::Music;
                    _controls->DisplayUpdater->Update();
                    TimeSpan duration = _player->NaturalDuration;
                    _durationTicks = duration.Duration;
                    if (_durationTicks > 0 && _positionTicks > _durationTicks)
                        _positionTicks = _durationTicks;
                }
                catch (Exception^) {}
                SendState();
            });
    }
    catch (Exception^) {}
}

void AudioTask::SendState()
{
    if (_canceled)
        return;
    try
    {
        if (_player != nullptr)
        {
            TimeSpan position = _player->Position;
            if (position.Duration >= 0)
                _positionTicks = position.Duration;
            TimeSpan duration = _player->NaturalDuration;
            if (duration.Duration > 0)
                _durationTicks = duration.Duration;
            _isPlaying = _player->CurrentState == MediaPlayerState::Playing;
        }

        ValueSet^ message = ref new ValueSet();
        message->Insert("title", _title == nullptr ? "Nessun brano" : _title);
        message->Insert("artist", _artist == nullptr ? "" : _artist);
        message->Insert("album", _album == nullptr ? "" : _album);
        message->Insert("path", _paths.empty() || _index >= _paths.size() ? "" : _paths[_index]);
        message->Insert("playing", _isPlaying);
        message->Insert("positionTicks", _positionTicks);
        message->Insert("durationTicks", _durationTicks);
        message->Insert("index", _index);
        BackgroundMediaPlayer::SendMessageToForeground(message);
    }
    catch (Exception^) {}
}

void AudioTask::SavePlaybackState()
{
    try
    {
        if (_player != nullptr && _hasTrack)
        {
            TimeSpan position = _player->Position;
            if (position.Duration >= 0)
                _positionTicks = position.Duration;
        }
        String^ paths = "";
        for (unsigned int i = 0; i < _paths.size(); ++i)
        {
            if (i != 0)
                paths += "\n";
            paths += _paths[i];
        }
        IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
        values->Insert("queue", paths);
        values->Insert("queueIndex", static_cast<int>(_index));
        values->Insert("positionTicks", _positionTicks);
        values->Insert("shuffle", _shuffle);
        values->Insert("repeat", _repeat);
    }
    catch (Exception^) {}
}

void AudioTask::OnCanceled(IBackgroundTaskInstance^,
                           BackgroundTaskCancellationReason)
{
    if (_canceled)
        return;
    _canceled = true;
    try { SavePlaybackState(); } catch (Exception^) {}
    if (_player != nullptr)
    {
        try
        {
            if (_player->CurrentState == MediaPlayerState::Playing)
                _player->Pause();
        }
        catch (Exception^) {}
    }
    if (_deferral != nullptr)
    {
        _deferral->Complete();
        _deferral = nullptr;
    }
}
