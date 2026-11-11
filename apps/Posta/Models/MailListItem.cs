using System;
using Posta.Core.Model;
using Windows.UI.Text;

namespace Posta.Models
{
    /// <summary>View model for one row of the inbox list.</summary>
    public sealed class MailListItem
    {
        public uint Uid { get; private set; }
        public string From { get; private set; }
        public string Subject { get; private set; }
        public string DateText { get; private set; }
        public bool IsUnread { get; private set; }
        public FontWeight Weight { get { return IsUnread ? FontWeights.Bold : FontWeights.Normal; } }

        public static MailListItem Create(MailSummary m)
        {
            var local = m.Date == DateTimeOffset.MinValue ? m.Date : m.Date.ToLocalTime();
            return new MailListItem
            {
                Uid = m.Uid,
                From = string.IsNullOrWhiteSpace(m.From) ? "(mittente sconosciuto)" : m.From,
                Subject = string.IsNullOrWhiteSpace(m.Subject) ? "(nessun oggetto)" : m.Subject,
                DateText = m.Date == DateTimeOffset.MinValue ? "" : local.ToString("dd/MM/yyyy HH:mm"),
                IsUnread = !m.IsSeen,
            };
        }

        public void MarkSeen(bool seen)
        {
            IsUnread = !seen;
        }
    }
}
