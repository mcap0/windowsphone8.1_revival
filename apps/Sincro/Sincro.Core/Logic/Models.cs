using System;
using System.Collections.Generic;

// Pure logic: no WinRT, no network. Linked into Sincro.Tests (net8) as well as Sincro.Core (WP8.1).
namespace Sincro.Logic
{
    public enum PhoneKind { Mobile, Work, Telephone }

    public enum EmailKind { Personal, Work, Other }

    /// <summary>One Google person, already mapped to the phone's contact fields.</summary>
    public sealed class ContactModel
    {
        public string ResourceName;   // people/c123...
        public bool Deleted;          // metadata.deleted in incremental sync
        public string GivenName;
        public string FamilyName;
        public string DisplayName;
        public string Company;
        public string JobTitle;
        public List<KeyValuePair<PhoneKind, string>> Phones = new List<KeyValuePair<PhoneKind, string>>();
        public List<KeyValuePair<EmailKind, string>> Emails = new List<KeyValuePair<EmailKind, string>>();
    }

    /// <summary>One Google Calendar event mapped to appointment fields.</summary>
    public sealed class EventModel
    {
        public string GoogleId;
        public bool Cancelled;
        public string Subject;
        public string Location;
        public string Details;
        public DateTimeOffset Start;
        public TimeSpan Duration;
        public bool AllDay;
        public TimeSpan? Reminder;

        /// <summary>Everything that is written to the phone; a change in it means "update".</summary>
        public string Fingerprint
        {
            get
            {
                return string.Join("\u001f", new string[] {
                    Subject ?? "", Location ?? "", Details ?? "",
                    Start.ToString("o"), Duration.ToString(), AllDay.ToString(),
                    Reminder.HasValue ? Reminder.Value.ToString() : "-" });
            }
        }
    }
}
