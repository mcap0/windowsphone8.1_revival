#ifndef PERLECAST_CASTUI_H
#define PERLECAST_CASTUI_H

#include <collection.h>

namespace PerleCast {

// A video to cast, filled by the app.
public ref class CastVideo sealed
{
public:
    CastVideo();
    property Platform::String^ VideoUrl;       // YouTube page URL: identifies the video and its saved position
    property Platform::String^ Title;
    property Platform::String^ Uploader;
    property Platform::String^ HlsUrl;         // HLS master playlist
    property Platform::String^ AudioLanguage;  // wanted audio language, "" for the default one
    property Platform::String^ SubtitleUrl;    // WebVTT sent to the TV, "" for none
    property double StartSeconds;              // where to start, -1 for the saved position
    // the subtitles that can be chosen on the TV (same order in both lists)
    property Windows::Foundation::Collections::IVector<Platform::String^>^ SubtitleLabels;
    property Windows::Foundation::Collections::IVector<Platform::String^>^ SubtitleUrls;
};

public ref class CastUi sealed
{
public:
    // Not casting: chooses the TV. Casting: plays it now, adds it to the TV queue, or opens the controls.
    static void Show(CastVideo^ video);

    // While casting, a video opened in the app goes to the TV instead of the phone player.
    // Returns true when the video is (now) on the TV.
    static bool PlayOnTvIfCasting(CastVideo^ video);

    // While casting, adds the video at the end of the TV queue.
    static void AddToQueue(CastVideo^ video);

    static property bool IsActive
    {
        bool get();
    }

    static void Reopen();

    // The video on the TV, for the app's mini player (UI thread). The position moves on between the polls of the TV.
    static property Platform::String^ TvTitle { Platform::String^ get(); }
    static property Platform::String^ TvUploader { Platform::String^ get(); }
    static property double TvPositionSeconds { double get(); }
    static property double TvDurationSeconds { double get(); }   // -1 when unknown
    static property bool TvPlaying { bool get(); }
    static void TogglePlayPause();

    static event Windows::Foundation::EventHandler<Platform::Object^>^ ActiveChanged;

internal:
    static void RaiseActiveChanged(bool active);

private:
    CastUi();
};

} // namespace PerleCast

#endif
