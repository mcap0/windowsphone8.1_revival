using System;

namespace Calendario.Logic
{
    /// <summary>One appointment as shown by the app. Pure data: no WinRT types.</summary>
    public sealed class EventItem
    {
        public string LocalId;
        public string CalendarId;
        public string Title;
        public string Location;
        public string Details;
        /// <summary>Local start. For all-day events: local midnight of the first day.</summary>
        public DateTime Start;
        /// <summary>For all-day events a whole number of days (exclusive end).</summary>
        public TimeSpan Duration;
        public bool AllDay;
        public TimeSpan? Reminder;
        public string CalendarName;
        public bool IsEditable;
        /// <summary>ARGB calendar colour when the store provides one.</summary>
        public uint? CalendarArgb;

        public DateTime End
        {
            get { return Start + Duration; }
        }

        /// <summary>First local date the event touches.</summary>
        public DateTime FirstDay
        {
            get { return Start.Date; }
        }

        /// <summary>Last local date (inclusive) the event touches.</summary>
        public DateTime LastDay
        {
            get
            {
                if (Duration <= TimeSpan.Zero) return Start.Date;
                return (End - TimeSpan.FromTicks(1)).Date;
            }
        }

        public bool SpansDays
        {
            get { return LastDay > FirstDay; }
        }
    }
}
