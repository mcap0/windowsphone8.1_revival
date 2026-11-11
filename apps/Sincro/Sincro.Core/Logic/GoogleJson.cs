using System;
using System.Collections.Generic;
using System.Globalization;
using Newtonsoft.Json.Linq;

namespace Sincro.Logic
{
    /// <summary>Maps People API JSON (people.connections.list) to ContactModel.</summary>
    public static class ContactMapper
    {
        public static PhoneKind PhoneKindFor(string googleType)
        {
            switch ((googleType ?? "").ToLowerInvariant())
            {
                case "mobile": return PhoneKind.Mobile;
                case "work": return PhoneKind.Work;
                default: return PhoneKind.Telephone; // home, other, main, pager, ...
            }
        }

        public static EmailKind EmailKindFor(string googleType)
        {
            switch ((googleType ?? "").ToLowerInvariant())
            {
                case "work": return EmailKind.Work;
                case "home":
                case "other":
                case "": return EmailKind.Personal;
                default: return EmailKind.Other;
            }
        }

        public static ContactModel Map(JObject person)
        {
            var c = new ContactModel { ResourceName = (string)person["resourceName"] };
            if (c.ResourceName == null) return null;

            if ((bool?)person["metadata"]?["deleted"] == true)
            {
                c.Deleted = true;
                return c;
            }

            // Primary name first, if Google marks one; otherwise the first entry.
            JToken name = FirstPrimaryOrFirst(person["names"]);
            if (name != null)
            {
                c.GivenName = (string)name["givenName"];
                c.FamilyName = (string)name["familyName"];
                c.DisplayName = (string)name["displayName"];
            }
            if (string.IsNullOrWhiteSpace(c.DisplayName))
            {
                c.DisplayName = (c.GivenName + " " + c.FamilyName).Trim();
            }

            var org = FirstPrimaryOrFirst(person["organizations"]);
            if (org != null)
            {
                c.Company = (string)org["name"];
                c.JobTitle = (string)org["title"];
            }

            // Phones: one value per phone property; extra numbers go to Telephone when free.
            var phones = person["phoneNumbers"] as JArray;
            if (phones != null)
            {
                var used = new HashSet<PhoneKind>();
                var overflow = new List<string>();
                foreach (JToken p in phones)
                {
                    string value = (string)p["value"] ?? (string)p["canonicalForm"];
                    if (string.IsNullOrWhiteSpace(value)) continue;
                    PhoneKind kind = PhoneKindFor((string)p["type"]);
                    if (used.Add(kind)) c.Phones.Add(new KeyValuePair<PhoneKind, string>(kind, value));
                    else overflow.Add(value);
                }
                // Telephone is the fallback slot for everything that did not fit.
                if (!used.Contains(PhoneKind.Telephone) && overflow.Count > 0)
                {
                    c.Phones.Add(new KeyValuePair<PhoneKind, string>(PhoneKind.Telephone, overflow[0]));
                }
            }

            var emails = person["emailAddresses"] as JArray;
            if (emails != null)
            {
                var used = new HashSet<EmailKind>();
                foreach (JToken e in emails)
                {
                    string value = (string)e["value"];
                    if (string.IsNullOrWhiteSpace(value)) continue;
                    EmailKind kind = EmailKindFor((string)e["type"]);
                    if (used.Add(kind)) c.Emails.Add(new KeyValuePair<EmailKind, string>(kind, value));
                }
            }
            return c;
        }

        private static JToken FirstPrimaryOrFirst(JToken array)
        {
            var arr = array as JArray;
            if (arr == null || arr.Count == 0) return null;
            foreach (JToken t in arr)
            {
                if ((bool?)t["metadata"]?["primary"] == true) return t;
            }
            return arr[0];
        }
    }

    /// <summary>Maps Calendar API event JSON to EventModel.</summary>
    public static class CalendarMapper
    {
        public const int DefaultReminderMinutes = 15;

        /// <summary>Returns null when the event has no usable start.</summary>
        public static EventModel Map(JObject ev)
        {
            var e = new EventModel { GoogleId = (string)ev["id"] };
            if (string.IsNullOrEmpty(e.GoogleId)) return null;
            e.Cancelled = string.Equals((string)ev["status"], "cancelled", StringComparison.OrdinalIgnoreCase);
            if (e.Cancelled) return e;

            e.Subject = (string)ev["summary"] ?? "(senza titolo)";
            e.Location = (string)ev["location"];
            e.Details = (string)ev["description"];
            e.Reminder = ReminderFor(ev["reminders"] as JObject);

            JToken start = ev["start"];
            JToken end = ev["end"];
            if (start == null) return null;

            string startDate = (string)start["date"];
            if (!string.IsNullOrEmpty(startDate))
            {
                // All-day: "end.date" is exclusive. Start at local midnight of the day.
                DateTime d = DateTime.ParseExact(startDate, "yyyy-MM-dd", CultureInfo.InvariantCulture);
                DateTime endD = d.AddDays(1);
                string endDate = end == null ? null : (string)end["date"];
                if (!string.IsNullOrEmpty(endDate))
                {
                    endD = DateTime.ParseExact(endDate, "yyyy-MM-dd", CultureInfo.InvariantCulture);
                }
                if (endD <= d) endD = d.AddDays(1);
                e.AllDay = true;
                e.Start = new DateTimeOffset(d, TimeZoneInfo.Local.GetUtcOffset(d));
                e.Duration = endD - d;
                return e;
            }

            string startDt = (string)start["dateTime"];
            if (string.IsNullOrEmpty(startDt)) return null;
            // DateTimeOffset.Parse keeps the event's own UTC offset (e.g. +02:00 or -05:00).
            e.Start = DateTimeOffset.Parse(startDt, CultureInfo.InvariantCulture, DateTimeStyles.None);
            string endDt = end == null ? null : (string)end["dateTime"];
            if (!string.IsNullOrEmpty(endDt))
            {
                DateTimeOffset endTime = DateTimeOffset.Parse(endDt, CultureInfo.InvariantCulture, DateTimeStyles.None);
                e.Duration = endTime > e.Start ? endTime - e.Start : TimeSpan.Zero;
            }
            return e;
        }

        /// <summary>useDefault=true means Google's default (we use 15 min); overrides take the first entry.</summary>
        public static TimeSpan? ReminderFor(JObject reminders)
        {
            if (reminders == null) return TimeSpan.FromMinutes(DefaultReminderMinutes);
            if ((bool?)reminders["useDefault"] == true) return TimeSpan.FromMinutes(DefaultReminderMinutes);
            var overrides = reminders["overrides"] as JArray;
            if (overrides != null && overrides.Count > 0)
            {
                int? minutes = (int?)overrides[0]["minutes"];
                if (minutes.HasValue) return TimeSpan.FromMinutes(minutes.Value);
            }
            return null;
        }
    }

    /// <summary>Recognises the "sync token no longer valid" answer (full resync needed).</summary>
    public static class GoogleErrors
    {
        public static bool IsExpiredSyncToken(int httpStatus, string body)
        {
            return httpStatus == 410 || (body != null && body.IndexOf("EXPIRED_SYNC_TOKEN", StringComparison.Ordinal) >= 0);
        }
    }
}
