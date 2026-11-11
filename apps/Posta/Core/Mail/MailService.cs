using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using MailKit;
using MailKit.Net.Imap;
using MailKit.Security;
using MimeKit;
using Posta.Core.Model;

namespace Posta.Core.Mail
{
    public static class MailFolderKeys
    {
        public const string Inbox = "inbox";
        public const string Sent = "sent";
        public const string Drafts = "drafts";
        public const string Trash = "trash";
        public const string Spam = "spam";
        public const string All = "all";
    }

    public sealed class OutgoingAttachment
    {
        public string Name { get; set; }
        public byte[] Data { get; set; }
        public string Label { get { return Name + " (" + (Data.Length >= 1024 * 1024 ? (Data.Length / (1024.0 * 1024.0)).ToString("0.0") + " MB" : ((Data.Length + 1023) / 1024) + " KB") + ")"; } }
    }

    public sealed class MailAttachmentInfo
    {
        public string Name { get; set; }
        public long Size { get; set; }
    }

    public sealed class MailMessageDetails
    {
        public MailContent Content { get; set; }
        public List<MailAttachmentInfo> Attachments { get; set; }

        public MailMessageDetails()
        {
            Attachments = new List<MailAttachmentInfo>();
        }
    }

    /// <summary>
    /// IMAP/SMTP operations for one account. Each public call opens its own connection
    /// and closes it again. Nothing here touches the UI.
    /// </summary>
    public sealed class MailService
    {
        public const int PageSize = 50;
        private const string GmailTrashFallback = "[Gmail]/Trash";

        private readonly AccountSettings _settings;
        private readonly string _password;

        public MailService(AccountSettings settings, string password)
        {
            if (settings == null)
                throw new ArgumentNullException("settings");

            _settings = settings;
            _password = password ?? "";
        }

        public async Task<string> TestConnectionAsync(CancellationToken ct)
        {
            using (var imap = await ConnectImapAsync(ct))
            {
                var inbox = imap.Inbox;
                await inbox.OpenAsync(FolderAccess.ReadOnly, ct);
                var count = inbox.Count;
                await imap.DisconnectAsync(true, ct);
                await CheckSmtpAsync(ct);
                return "IMAP e SMTP raggiungibili. Messaggi in arrivo: " + count + ".";
            }
        }

        public Task<InboxPage> FetchInboxPageAsync(int page, CancellationToken ct)
        {
            return FetchFolderPageAsync(MailFolderKeys.Inbox, page, ct);
        }

        public async Task<InboxPage> FetchFolderPageAsync(string folderKey, int page, CancellationToken ct)
        {
            if (page < 0)
                throw new ArgumentOutOfRangeException("page");

            using (var imap = await ConnectImapAsync(ct))
            {
                var folder = GetFolder(imap, folderKey);
                await folder.OpenAsync(FolderAccess.ReadOnly, ct);
                var total = folder.Count;
                var result = new InboxPage { Total = total };

                var high = (long)total - (long)page * PageSize - 1;
                var low = Math.Max(0L, (long)total - ((long)page + 1) * PageSize);
                if (total > 0 && high >= 0 && high >= low)
                {
                    const MessageSummaryItems items = MessageSummaryItems.Envelope |
                                                      MessageSummaryItems.Flags |
                                                      MessageSummaryItems.UniqueId |
                                                      MessageSummaryItems.InternalDate;
                    var fetched = await folder.FetchAsync((int)low, (int)high, items, ct);
                    foreach (var summary in fetched.OrderByDescending(item => item.Index))
                        result.Messages.Add(ToSummary(summary));
                }

                await imap.DisconnectAsync(true, ct);
                return result;
            }
        }

        public async Task<MailContent> FetchMessageAsync(uint uid, CancellationToken ct)
        {
            return await FetchMessageAsync(MailFolderKeys.Inbox, uid, ct);
        }

        public async Task<MailContent> FetchMessageAsync(string folderKey, uint uid, CancellationToken ct)
        {
            var details = await FetchMessageDetailsAsync(folderKey, uid, ct);
            return details.Content;
        }

        public async Task<MailMessageDetails> FetchMessageDetailsAsync(
            string folderKey, uint uid, CancellationToken ct)
        {
            using (var imap = await ConnectImapAsync(ct))
            {
                var folder = GetFolder(imap, folderKey);
                await folder.OpenAsync(FolderAccess.ReadWrite, ct);
                var id = new UniqueId(uid);
                var message = await folder.GetMessageAsync(id, ct);
                await folder.AddFlagsAsync(new List<UniqueId> { id }, MessageFlags.Seen, true, ct);
                await imap.DisconnectAsync(true, ct);

                var details = new MailMessageDetails
                {
                    Content = ToContent(message, uid)
                };

                foreach (var attachment in message.Attachments)
                {
                    details.Attachments.Add(new MailAttachmentInfo
                    {
                        Name = GetAttachmentName(attachment),
                        Size = GetAttachmentSize(attachment)
                    });
                }

                return details;
            }
        }

        public async Task<byte[]> FetchAttachmentAsync(
            string folderKey, uint uid, int attachmentIndex, CancellationToken ct)
        {
            if (attachmentIndex < 0)
                throw new ArgumentOutOfRangeException("attachmentIndex");

            using (var imap = await ConnectImapAsync(ct))
            {
                var folder = GetFolder(imap, folderKey);
                await folder.OpenAsync(FolderAccess.ReadOnly, ct);
                var message = await folder.GetMessageAsync(new UniqueId(uid), ct);
                var index = 0;

                foreach (var attachment in message.Attachments)
                {
                    if (index++ != attachmentIndex)
                        continue;

                    using (var output = new MemoryStream())
                    {
                        var part = attachment as MimePart;
                        if (part != null)
                        {
                            part.Content.DecodeTo(output);
                        }
                        else
                        {
                            var messagePart = attachment as MessagePart;
                            if (messagePart == null)
                                throw new InvalidOperationException("Formato allegato non supportato.");
                            messagePart.Message.WriteTo(output);
                        }

                        await imap.DisconnectAsync(true, ct);
                        return output.ToArray();
                    }
                }

                throw new InvalidOperationException("Allegato non trovato.");
            }
        }

        public Task SetSeenAsync(uint uid, bool seen, CancellationToken ct)
        {
            return SetSeenAsync(MailFolderKeys.Inbox, uid, seen, ct);
        }

        public async Task SetSeenAsync(string folderKey, uint uid, bool seen, CancellationToken ct)
        {
            using (var imap = await ConnectImapAsync(ct))
            {
                var folder = GetFolder(imap, folderKey);
                await folder.OpenAsync(FolderAccess.ReadWrite, ct);
                var ids = new List<UniqueId> { new UniqueId(uid) };
                if (seen)
                    await folder.AddFlagsAsync(ids, MessageFlags.Seen, true, ct);
                else
                    await folder.RemoveFlagsAsync(ids, MessageFlags.Seen, true, ct);
                await imap.DisconnectAsync(true, ct);
            }
        }

        public Task DeleteAsync(uint uid, CancellationToken ct)
        {
            return DeleteAsync(MailFolderKeys.Inbox, uid, ct);
        }

        public async Task DeleteAsync(string folderKey, uint uid, CancellationToken ct)
        {
            using (var imap = await ConnectImapAsync(ct))
            {
                var folder = GetFolder(imap, folderKey);
                await folder.OpenAsync(FolderAccess.ReadWrite, ct);
                var trash = FindTrash(imap);
                var ids = new List<UniqueId> { new UniqueId(uid) };

                if (string.Equals(folder.FullName, trash.FullName, StringComparison.OrdinalIgnoreCase))
                    await folder.AddFlagsAsync(ids, MessageFlags.Deleted, true, ct);
                else
                    await folder.MoveToAsync(ids, trash, ct);

                await imap.DisconnectAsync(true, ct);
            }
        }

        public Task SendAsync(
            string to, string subject, string body, string inReplyTo,
            string references, CancellationToken ct)
        {
            return SendAsync(to, "", subject, body, inReplyTo, references, ct);
        }

        public Task SendAsync(
            string to, string cc, string subject, string body, string inReplyTo,
            string references, CancellationToken ct)
        {
            return SendAsync(to, cc, subject, body, inReplyTo, references, null, ct);
        }

        public async Task SendAsync(
            string to, string cc, string subject, string body, string inReplyTo,
            string references, IList<OutgoingAttachment> attachments, CancellationToken ct)
        {
            var message = new MimeMessage();
            message.From.Add(new MailboxAddress(_settings.Email, _settings.Email));
            message.To.AddRange(InternetAddressList.Parse(to ?? ""));

            if (!string.IsNullOrWhiteSpace(cc))
                message.Cc.AddRange(InternetAddressList.Parse(cc));

            message.Subject = subject ?? "";
            if (!string.IsNullOrWhiteSpace(inReplyTo))
                message.InReplyTo = inReplyTo.Trim();

            if (!string.IsNullOrWhiteSpace(references))
            {
                foreach (var id in references.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries))
                    message.References.Add(id);
            }

            var builder = new BodyBuilder { TextBody = body ?? "" };
            if (attachments != null)
            {
                foreach (var attachment in attachments)
                    builder.Attachments.Add(attachment.Name, attachment.Data);
            }
            message.Body = builder.ToMessageBody();

            using (var smtp = new MailKit.Net.Smtp.SmtpClient())
            {
                await smtp.ConnectAsync(_settings.SmtpHost, _settings.SmtpPort,
                    SecureSocketOptions.SslOnConnect, ct);
                await smtp.AuthenticateAsync(_settings.EffectiveUsername, _password, ct);
                await smtp.SendAsync(message, ct);
                await smtp.DisconnectAsync(true, ct);
            }
        }

        public static string Describe(Exception ex)
        {
            if (ex is OperationCanceledException)
                return "Operazione annullata.";
            if (ex is AuthenticationException)
                return "Accesso rifiutato: controlla indirizzo e password per le app.";
            if (ex is SslHandshakeException)
                return "Connessione sicura (SSL/TLS) non riuscita: controlla host e porta.";
            if (ex is ParseException)
                return "Indirizzo destinatario non valido.";
            if (ex is FolderNotFoundException)
                return "Cartella non trovata sul server.";
            if (ex is ServiceNotConnectedException)
                return "Connessione al server interrotta.";
            return "Errore: " + ex.Message;
        }

        private async Task<ImapClient> ConnectImapAsync(CancellationToken ct)
        {
            var imap = new ImapClient();
            try
            {
                await imap.ConnectAsync(_settings.ImapHost, _settings.ImapPort,
                    SecureSocketOptions.SslOnConnect, ct);
                await imap.AuthenticateAsync(_settings.EffectiveUsername, _password, ct);
                return imap;
            }
            catch
            {
                imap.Dispose();
                throw;
            }
        }

        private async Task CheckSmtpAsync(CancellationToken ct)
        {
            using (var smtp = new MailKit.Net.Smtp.SmtpClient())
            {
                await smtp.ConnectAsync(_settings.SmtpHost, _settings.SmtpPort,
                    SecureSocketOptions.SslOnConnect, ct);
                await smtp.AuthenticateAsync(_settings.EffectiveUsername, _password, ct);
                await smtp.DisconnectAsync(true, ct);
            }
        }

        private static IMailFolder GetFolder(ImapClient imap, string folderKey)
        {
            var key = (folderKey ?? MailFolderKeys.Inbox).ToLowerInvariant();
            switch (key)
            {
                case MailFolderKeys.Inbox:
                    return imap.Inbox;
                case MailFolderKeys.Sent:
                    return GetSpecialOrPath(imap, SpecialFolder.Sent, "Sent Mail");
                case MailFolderKeys.Drafts:
                    return GetSpecialOrPath(imap, SpecialFolder.Drafts, "Drafts");
                case MailFolderKeys.Trash:
                    return FindTrash(imap);
                case MailFolderKeys.Spam:
                    return GetSpecialOrPath(imap, SpecialFolder.Junk, "Spam");
                case MailFolderKeys.All:
                    return GetSpecialOrPath(imap, SpecialFolder.All, "All Mail");
                default:
                    throw new ArgumentException("Cartella non valida.", "folderKey");
            }
        }

        private static IMailFolder GetSpecialOrPath(
            ImapClient imap, SpecialFolder specialFolder, string gmailFallback)
        {
            try
            {
                return imap.GetFolder(specialFolder);
            }
            catch (FolderNotFoundException)
            {
                return imap.GetFolder("[Gmail]/" + gmailFallback);
            }
        }

        private static IMailFolder FindTrash(ImapClient imap)
        {
            try
            {
                return imap.GetFolder(SpecialFolder.Trash);
            }
            catch (FolderNotFoundException)
            {
                return imap.GetFolder(GmailTrashFallback);
            }
        }

        private static MailContent ToContent(MimeMessage message, uint uid)
        {
            return new MailContent
            {
                Uid = uid,
                From = message.From != null ? message.From.ToString() : "",
                To = message.To != null ? message.To.ToString() : "",
                Cc = message.Cc != null ? message.Cc.ToString() : "",
                Subject = message.Subject ?? "",
                Date = message.Date,
                MessageId = message.MessageId ?? "",
                References = string.Join(" ", message.References ?? new MessageIdList()),
                TextBody = message.TextBody ?? "",
                HtmlBody = message.HtmlBody ?? ""
            };
        }

        private static string GetAttachmentName(MimeEntity entity)
        {
            var part = entity as MimePart;
            if (part != null && !string.IsNullOrWhiteSpace(part.FileName))
                return part.FileName;

            var dispositionName = entity.ContentDisposition == null
                ? null
                : entity.ContentDisposition.FileName;
            if (!string.IsNullOrWhiteSpace(dispositionName))
                return dispositionName;

            var contentTypeName = entity.ContentType == null ? null : entity.ContentType.Name;
            return string.IsNullOrWhiteSpace(contentTypeName) ? "Allegato" : contentTypeName;
        }

        private static long GetAttachmentSize(MimeEntity entity)
        {
            var part = entity as MimePart;
            if (part == null || part.Content == null)
                return 0;

            var stream = part.Content.Stream;
            if (stream == null || !stream.CanSeek)
                return 0;

            try
            {
                return stream.Length;
            }
            catch (Exception)
            {
                return 0;
            }
        }

        private static MailSummary ToSummary(IMessageSummary summary)
        {
            var envelope = summary.Envelope;
            return new MailSummary
            {
                Uid = summary.UniqueId.Id,
                From = envelope != null && envelope.From != null ? envelope.From.ToString() : "",
                To = envelope != null && envelope.To != null ? envelope.To.ToString() : "",
                Subject = envelope != null ? (envelope.Subject ?? "") : "",
                Date = summary.InternalDate.HasValue
                    ? summary.InternalDate.Value
                    : DateTimeOffset.MinValue,
                IsSeen = summary.Flags.HasValue &&
                         (summary.Flags.Value & MessageFlags.Seen) != 0,
                MessageId = envelope != null ? (envelope.MessageId ?? "") : ""
            };
        }
    }
}
