using System;
using System.Collections.Generic;
using System.Linq;

namespace Calendario.Logic
{
    public sealed class DayGroup
    {
        public DateTime Date;
        public List<EventItem> Events = new List<EventItem>();
    }

    public static class AgendaBuilder
    {
        /// <summary>
        /// Google/Sincro store all-day events as local midnight; other calendars may store UTC midnight.
        /// Returns the local calendar date the all-day event starts on.
        /// </summary>
        public static DateTime AllDayDate(DateTime localStart, DateTime utcStart)
        {
            if (localStart.TimeOfDay == TimeSpan.Zero) return localStart.Date;
            if (utcStart.TimeOfDay == TimeSpan.Zero) return utcStart.Date;
            return localStart.Date;
        }

        /// <summary>All-day durations are whole days, at least one.</summary>
        public static TimeSpan AllDayDuration(TimeSpan duration)
        {
            int days = (int)Math.Round(duration.TotalDays);
            return TimeSpan.FromDays(Math.Max(1, days));
        }

        /// <summary>Sort inside a day: all-day first, then start time, then title.</summary>
        public static int Compare(EventItem a, EventItem b)
        {
            if (a.AllDay != b.AllDay) return a.AllDay ? -1 : 1;
            int c = a.Start.CompareTo(b.Start);
            if (c != 0) return c;
            return string.Compare(a.Title ?? "", b.Title ?? "", StringComparison.CurrentCultureIgnoreCase);
        }

        /// <summary>
        /// Groups events by every local day in [from, to] that they touch. Days without events are skipped.
        /// </summary>
        public static List<DayGroup> Group(IEnumerable<EventItem> events, DateTime from, DateTime to)
        {
            DateTime f = from.Date;
            DateTime t = to.Date;
            var byDay = new SortedDictionary<DateTime, DayGroup>();
            foreach (EventItem e in events)
            {
                DateTime first = e.FirstDay < f ? f : e.FirstDay;
                DateTime last = e.LastDay > t ? t : e.LastDay;
                for (DateTime d = first; d <= last; d = d.AddDays(1))
                {
                    DayGroup g;
                    if (!byDay.TryGetValue(d, out g))
                    {
                        g = new DayGroup { Date = d };
                        byDay[d] = g;
                    }
                    g.Events.Add(e);
                }
            }
            var result = new List<DayGroup>(byDay.Values);
            foreach (DayGroup g in result) g.Events.Sort(Compare);
            return result;
        }

        /// <summary>Set of local dates (inside [from, to]) that have at least one event.</summary>
        public static HashSet<DateTime> DaysWithEvents(IEnumerable<EventItem> events, DateTime from, DateTime to)
        {
            return new HashSet<DateTime>(Group(events, from, to).Select(g => g.Date));
        }

        /// <summary>
        /// Event shown as "next" on the live tile: first timed event starting today at or after now,
        /// otherwise the first all-day event of today. Null when nothing is left today.
        /// </summary>
        public static EventItem NextToday(IEnumerable<EventItem> events, DateTime now)
        {
            DateTime today = now.Date;
            var todays = events.Where(e => e.FirstDay <= today && e.LastDay >= today).ToList();
            EventItem next = todays
                .Where(e => !e.AllDay && e.Start >= now)
                .OrderBy(e => e.Start).FirstOrDefault();
            if (next != null) return next;
            return todays.Where(e => e.AllDay).OrderBy(e => e.Start).FirstOrDefault();
        }
    }
}
