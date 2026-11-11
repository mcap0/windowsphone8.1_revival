#pragma once

#include <string>

namespace AvvisiTask
{
    // One Telegram check, blocking (call it from a thread pool thread). Reads the Telegram app's session
    // (its LocalFolder\tg.state, never written here), connects with its own MTProto session, asks updates.getState
    // and, when that changed, the top dialogs; new incoming messages in chats that are not muted become toasts
    // (launch "chat=<key>", consumed by the native Telegram app). Returns a short status for the log ("telegram: ...").
    std::wstring PollTelegram();

    // Path of the Telegram app's LocalFolder, from this package's own one (same publisher).
    std::wstring TelegramFolder();
}
