#pragma once

namespace PostaTask
{
    // Time trigger (15 min) and InternetAvailable: one mail check per run (MailPoll), toasts for new unread mail.
    public ref class Watch sealed : Windows::ApplicationModel::Background::IBackgroundTask
    {
    public:
        virtual void Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ taskInstance);
    };

    // Called by the app at every launch (App.OnLaunched): registers Watch on its two triggers and, the first time,
    // takes the inbox baseline so that the first background check already shows mail that arrives after it.
    public ref class Checks sealed
    {
    public:
        static void Register();
    };
}
