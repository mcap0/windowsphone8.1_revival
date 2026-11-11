using System;

namespace Calendario.Logic
{
    /// <summary>Italian day and month names, independent of the phone's culture settings.</summary>
    public static class ItalianDates
    {
        // Indexed Monday = 0 ... Sunday = 6.
        private static readonly string[] WeekdaysLong = { "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato", "domenica" };
        private static readonly string[] WeekdaysShort = { "Lun", "Mar", "Mer", "Gio", "Ven", "Sab", "Dom" };
        private static readonly string[] MonthsLong = { "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre" };

        public static int MondayIndex(DayOfWeek d)
        {
            return ((int)d + 6) % 7;
        }

        public static string WeekdayLong(DayOfWeek d)
        {
            return WeekdaysLong[MondayIndex(d)];
        }

        public static string WeekdayShort(DayOfWeek d)
        {
            return WeekdaysShort[MondayIndex(d)];
        }

        public static string MonthLong(int month)
        {
            return MonthsLong[month - 1];
        }

        /// <summary>"Oggi", "Domani", "Ieri", otherwise "lunedì 21 ottobre".</summary>
        public static string DayLabel(DateTime date, DateTime today)
        {
            DateTime d = date.Date;
            DateTime t = today.Date;
            if (d == t) return "Oggi";
            if (d == t.AddDays(1)) return "Domani";
            if (d == t.AddDays(-1)) return "Ieri";
            return WeekdayLong(d.DayOfWeek) + " " + d.Day + " " + MonthLong(d.Month);
        }

        /// <summary>"Ottobre 2026".</summary>
        public static string MonthTitle(int year, int month)
        {
            string m = MonthLong(month);
            return char.ToUpperInvariant(m[0]) + m.Substring(1) + " " + year;
        }
    }
}
