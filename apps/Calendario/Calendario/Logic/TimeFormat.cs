using System;
using System.Globalization;

namespace Calendario.Logic
{
    public static class TimeFormat
    {
        private const string Dash = "–"; // en dash

        public static string Hm(DateTime t)
        {
            return t.ToString("HH:mm", CultureInfo.InvariantCulture);
        }

        /// <summary>
        /// Time range shown in the agenda/day list. For events that do not fit in one day the
        /// dates are included: "12/10 09:00–13/10 10:00".
        /// </summary>
        public static string Range(EventItem e)
        {
            if (e.AllDay)
            {
                int days = (int)Math.Round(e.Duration.TotalDays);
                return days > 1 ? "Tutto il giorno (" + days + " giorni)" : "Tutto il giorno";
            }
            if (e.SpansDays)
            {
                return e.Start.ToString("dd/MM HH:mm", CultureInfo.InvariantCulture) + Dash
                    + e.End.ToString("dd/MM HH:mm", CultureInfo.InvariantCulture);
            }
            if (e.Duration <= TimeSpan.Zero) return Hm(e.Start);
            return Hm(e.Start) + Dash + Hm(e.End);
        }

        /// <summary>Range as seen on one specific day: "fino alle 10:00" / "dalle 09:00" for multi-day events.</summary>
        public static string RangeOnDay(EventItem e, DateTime day)
        {
            if (e.AllDay || !e.SpansDays) return Range(e);
            DateTime d = day.Date;
            if (d <= e.FirstDay) return Hm(e.Start) + Dash + "24:00";
            if (d >= e.LastDay) return "00:00" + Dash + Hm(e.End);
            return "Tutto il giorno";
        }

        public static string ReminderText(TimeSpan? r)
        {
            if (!r.HasValue) return "Nessuno";
            double min = r.Value.TotalMinutes;
            if (min <= 0) return "All'orario dell'evento";
            if (min % 1440 == 0) return (int)(min / 1440) + (min == 1440 ? " giorno prima" : " giorni prima");
            if (min % 60 == 0) return (int)(min / 60) + (min == 60 ? " ora prima" : " ore prima");
            return (int)min + " min prima";
        }
    }
}
