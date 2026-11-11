#pragma once

#include <string>

namespace PostaTask
{
    // One Gmail check over IMAP/TLS, blocking (thread pool thread). STATUS INBOX each time; only when UIDNEXT grew
    // the new unread messages' From/Subject, shown as toasts (launch "mail"). Returns a status for the log ("posta: ...").
    std::wstring PollMail();

    // Posta's own account: LocalSettings "posta.account.v1" (JSON written by Services/AccountStore.cs) and the app
    // password in the PasswordVault under resource "Posta.Mail", keyed by the e-mail address.
    bool MailAccount(std::wstring& host, std::wstring& port, std::wstring& user, std::wstring& password);
}
