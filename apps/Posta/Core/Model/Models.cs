using System;
using System.Collections.Generic;

namespace Posta.Core.Model
{
    /// <summary>Account data that is safe to keep in plain settings (the password is NOT here).</summary>
    public sealed class AccountSettings
    {
        public string Email { get; set; } = "";
        public string ImapHost { get; set; } = "imap.gmail.com";
        public int ImapPort { get; set; } = 993;
        public string SmtpHost { get; set; } = "smtp.gmail.com";
        public int SmtpPort { get; set; } = 465;
        /// <summary>Login name; when empty the e-mail address is used.</summary>
        public string Username { get; set; } = "";

        public string EffectiveUsername
        {
            get { return string.IsNullOrWhiteSpace(Username) ? (Email ?? "").Trim() : Username.Trim(); }
        }

        public static AccountSettings GmailDefaults(string email)
        {
            return new AccountSettings { Email = (email ?? "").Trim() };
        }
    }

    /// <summary>One row of the inbox list (envelope + flags only).</summary>
    public sealed class MailSummary
    {
        public uint Uid { get; set; }
        public string From { get; set; } = "";
        public string To { get; set; } = "";
        public string Subject { get; set; } = "";
        public DateTimeOffset Date { get; set; }
        public bool IsSeen { get; set; }
        public string MessageId { get; set; } = "";
    }

    /// <summary>A page of the inbox plus the total number of messages on the server.</summary>
    public sealed class InboxPage
    {
        public int Total { get; set; }
        public List<MailSummary> Messages { get; set; } = new List<MailSummary>();
    }

    /// <summary>Full message as needed by the message and reply screens.</summary>
    public sealed class MailContent
    {
        public uint Uid { get; set; }
        public string From { get; set; } = "";
        public string To { get; set; } = "";
        public string Cc { get; set; } = "";
        public string Subject { get; set; } = "";
        public DateTimeOffset Date { get; set; }
        public string MessageId { get; set; } = "";
        public string References { get; set; } = "";
        public string TextBody { get; set; } = "";
        public string HtmlBody { get; set; } = "";
    }
}
