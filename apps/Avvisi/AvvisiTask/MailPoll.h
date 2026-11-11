#pragma once

#include <string>

namespace AvvisiTask
{
    // One Gmail check over IMAP/TLS, blocking (thread pool thread). STATUS INBOX each time; only when UIDNEXT grew
    // the new unread messages' From/Subject, shown as toasts (launch "mail"). Returns a status for the log ("posta: ...").
    std::wstring PollMail();

    // Account: address in LocalSettings "mailUser", app password in the PasswordVault under resource
    // "Avvisi.Posta" (the app's MainPage writes both).
    bool MailAccount(std::wstring& user, std::wstring& password);
}
