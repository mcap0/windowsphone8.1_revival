#pragma once

namespace AvvisiTask
{
    // Time trigger (30 min) and InternetAvailable: logs how old the last KeepAlive tick is and, if KeepAlive looks
    // dead, tries to start it again through BackgroundMediaPlayer (to find out whether WP8.1 allows that here).
    public ref class Watch sealed : Windows::ApplicationModel::Background::IBackgroundTask
    {
    public:
        virtual void Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ taskInstance);
    };
}
