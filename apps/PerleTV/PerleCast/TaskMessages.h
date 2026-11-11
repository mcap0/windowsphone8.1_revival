#ifndef PERLECAST_TASKMESSAGES_H
#define PERLECAST_TASKMESSAGES_H

// Messages between Perle TV and its background audio task (BackgroundMediaPlayer::SendMessageTo*): a ValueSet with
// "cmd" and "data", the fields of data in a fixed order, separated by \x1E.

#include "CastSession.h"
#include "WinRtText.h"

#include <cstdlib>
#include <string>
#include <vector>

#include <windows.foundation.collections.h>

namespace PerleCast {

class Packer
{
public:
    Packer& Put(const std::string& value) { if (!first_) text_ += '\x1E'; text_ += value; first_ = false; return *this; }
    Packer& Put(int64 value) { return Put(std::to_string(value)); }
    Packer& Put(const std::vector<std::string>& values)
    {
        Put(static_cast<int64>(values.size()));
        for (size_t i = 0; i < values.size(); ++i) Put(values[i]);
        return *this;
    }
    Packer& Put(const CastQueueItem& item)
    {
        Put(item.videoUrl).Put(item.title).Put(item.uploader).Put(item.hlsUrl).Put(item.audioLanguage)
            .Put(item.subtitleUrl).Put(item.startSeconds).Put(static_cast<int64>(item.subtitles.size()));
        for (size_t i = 0; i < item.subtitles.size(); ++i) Put(item.subtitles[i].first).Put(item.subtitles[i].second);
        return *this;
    }
    Packer& Put(const DlnaDevice& device)
    {
        return Put(device.udn).Put(device.friendlyName).Put(device.manufacturer).Put(device.modelName)
            .Put(device.location).Put(device.avTransportControlUrl).Put(device.avTransportServiceType)
            .Put(device.renderingControlUrl).Put(device.renderingControlServiceType)
            .Put(device.connectionManagerUrl).Put(device.connectionManagerServiceType);
    }
    Packer& Put(const ViewState& view)
    {
        Put(view.title).Put(view.uploader).Put(view.device).Put(view.status).Put(view.duration).Put(view.position)
            .Put(static_cast<int64>(view.playing ? 1 : 0)).Put(view.queueTitles).Put(static_cast<int64>(view.queueIndex))
            .Put(view.audioNames).Put(static_cast<int64>(view.audioIndex)).Put(view.subtitleLabels)
            .Put(static_cast<int64>(view.subtitleIndex)).Put(static_cast<int64>(view.heights.size()));
        for (size_t i = 0; i < view.heights.size(); ++i) Put(static_cast<int64>(view.heights[i]));
        return Put(static_cast<int64>(view.height));
    }
    const std::string& Text() const { return text_; }

private:
    std::string text_;
    bool first_ = true;
};

class Unpacker
{
public:
    explicit Unpacker(const std::string& text) : text_(text), at_(0) {}
    std::string Str()
    {
        if (at_ > text_.size()) return std::string();
        size_t end = text_.find('\x1E', at_);
        if (end == std::string::npos) end = text_.size();
        std::string value = text_.substr(at_, end - at_);
        at_ = end + 1;
        return value;
    }
    int64 Int() { return _atoi64(Str().c_str()); }
    std::vector<std::string> List()
    {
        std::vector<std::string> values;
        for (int64 n = Int(); n > 0; --n) values.push_back(Str());
        return values;
    }
    CastQueueItem Item()
    {
        CastQueueItem item;
        item.videoUrl = Str(); item.title = Str(); item.uploader = Str(); item.hlsUrl = Str();
        item.audioLanguage = Str(); item.subtitleUrl = Str(); item.startSeconds = Int();
        for (int64 n = Int(); n > 0; --n)
        {
            std::string label = Str();
            item.subtitles.push_back(std::make_pair(label, Str()));
        }
        return item;
    }
    DlnaDevice Device()
    {
        DlnaDevice device;
        device.udn = Str(); device.friendlyName = Str(); device.manufacturer = Str(); device.modelName = Str();
        device.location = Str(); device.avTransportControlUrl = Str(); device.avTransportServiceType = Str();
        device.renderingControlUrl = Str(); device.renderingControlServiceType = Str();
        device.connectionManagerUrl = Str(); device.connectionManagerServiceType = Str();
        return device;
    }
    ViewState View()
    {
        ViewState view;
        view.title = Str(); view.uploader = Str(); view.device = Str(); view.status = Str();
        view.duration = Int(); view.position = Int(); view.playing = Int() != 0;
        view.queueTitles = List(); view.queueIndex = static_cast<size_t>(Int());
        view.audioNames = List(); view.audioIndex = static_cast<size_t>(Int());
        view.subtitleLabels = List(); view.subtitleIndex = static_cast<int>(Int());
        for (int64 n = Int(); n > 0; --n) view.heights.push_back(static_cast<int>(Int()));
        view.height = static_cast<int>(Int());
        return view;
    }

private:
    std::string text_;
    size_t at_;
};

inline Windows::Foundation::Collections::ValueSet^ TaskMessage(const wchar_t* command, const std::string& data = std::string())
{
    auto message = ref new Windows::Foundation::Collections::ValueSet();
    message->Insert(L"cmd", ref new Platform::String(command));
    message->Insert(L"data", FromUtf8(data));
    return message;
}

inline std::wstring TaskCommand(Windows::Foundation::Collections::ValueSet^ message)
{
    if (message == nullptr || !message->HasKey(L"cmd")) return std::wstring();
    Platform::String^ command = dynamic_cast<Platform::String^>(message->Lookup(L"cmd"));
    return command == nullptr ? std::wstring() : std::wstring(command->Data());
}

inline std::string TaskData(Windows::Foundation::Collections::ValueSet^ message)
{
    if (message == nullptr || !message->HasKey(L"data")) return std::string();
    return ToUtf8(dynamic_cast<Platform::String^>(message->Lookup(L"data")));
}

// Perle TV holds this mutex for as long as its process lives (suspended too): the task stops when it is gone.
const wchar_t* const AppAliveMutex = L"PerleTV.alive";

} // namespace PerleCast

#endif
