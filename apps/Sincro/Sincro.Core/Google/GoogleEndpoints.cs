using System;
using System.Globalization;
using System.Text;
using Sincro.Logic;

namespace Sincro.Core
{
    /// <summary>URL builders for the two Google APIs used by v1.</summary>
    public static class GoogleEndpoints
    {
        public const string PeopleFields = "names,phoneNumbers,emailAddresses,organizations,photos,birthdays";

        /// <summary>Contacts list. With syncToken it returns only changes (deletions have metadata.deleted).</summary>
        public static string Connections(string syncToken, string pageToken)
        {
            var sb = new StringBuilder("https://people.googleapis.com/v1/people/me/connections");
            sb.Append("?personFields=").Append(Uri.EscapeDataString(PeopleFields));
            sb.Append("&pageSize=1000&requestSyncToken=true");
            if (!string.IsNullOrEmpty(syncToken)) sb.Append("&syncToken=").Append(Uri.EscapeDataString(syncToken));
            if (!string.IsNullOrEmpty(pageToken)) sb.Append("&pageToken=").Append(Uri.EscapeDataString(pageToken));
            return sb.ToString();
        }

        /// <summary>
        /// Calendar events in a window (now-30 d .. now+180 d). Full listing every run:
        /// Google forbids timeMin/timeMax together with syncToken, so v1 skips syncToken for calendar.
        /// showDeleted=true returns cancelled instances, which the diff turns into deletes.
        /// </summary>
        public static string Events(DateTime utcNow, string pageToken)
        {
            string min = utcNow.AddDays(-30).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);
            string max = utcNow.AddDays(180).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture);
            var sb = new StringBuilder("https://www.googleapis.com/calendar/v3/calendars/primary/events");
            sb.Append("?singleEvents=true&showDeleted=true&maxResults=2500");
            sb.Append("&timeMin=").Append(Uri.EscapeDataString(min));
            sb.Append("&timeMax=").Append(Uri.EscapeDataString(max));
            if (!string.IsNullOrEmpty(pageToken)) sb.Append("&pageToken=").Append(Uri.EscapeDataString(pageToken));
            return sb.ToString();
        }
    }
}
