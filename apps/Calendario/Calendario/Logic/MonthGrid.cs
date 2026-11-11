using System;

namespace Calendario.Logic
{
    /// <summary>Monday-first 6x7 month grid. Always 42 cells.</summary>
    public static class MonthGrid
    {
        public const int Cells = 42;

        /// <summary>First cell date (a Monday on or before the 1st of the month).</summary>
        public static DateTime GridStart(int year, int month)
        {
            DateTime first = new DateTime(year, month, 1);
            return first.AddDays(-ItalianDates.MondayIndex(first.DayOfWeek));
        }

        public static DateTime[] Build(int year, int month)
        {
            DateTime start = GridStart(year, month);
            var cells = new DateTime[Cells];
            for (int i = 0; i < Cells; i++) cells[i] = start.AddDays(i);
            return cells;
        }

        public static bool InMonth(DateTime cell, int month)
        {
            return cell.Month == month;
        }

        public static DateTime MonthStart(DateTime anyDayInMonth)
        {
            return new DateTime(anyDayInMonth.Year, anyDayInMonth.Month, 1);
        }

        public static DateTime AddMonths(DateTime monthStart, int delta)
        {
            return new DateTime(monthStart.Year, monthStart.Month, 1).AddMonths(delta);
        }
    }
}
