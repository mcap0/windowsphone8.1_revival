#pragma once

namespace TelegramTask
{
    // Time trigger (15 min) and InternetAvailable: one Telegram check per run (TgPoll), toasts for new messages.
    public ref class Watch sealed : Windows::ApplicationModel::Background::IBackgroundTask
    {
    public:
        virtual void Run(Windows::ApplicationModel::Background::IBackgroundTaskInstance^ taskInstance);
    };
}
