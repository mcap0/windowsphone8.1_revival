using System;
using System.Collections.Generic;
using Calendario.Logic;
using Windows.UI;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Media;

namespace Calendario.Models
{
    /// <summary>One line of a ListView: either a day header or an event. Bound by RowTemplate in the XAML.</summary>
    public sealed class AgendaRow
    {
        private static readonly SolidColorBrush DefaultBar = new SolidColorBrush(Color.FromArgb(0xFF, 0x00, 0x63, 0xB1));

        public string Key;
        public DateTime Date;
        public bool IsHeader;
        public EventItem Event;

        public string HeaderText { get; set; }
        public string TimeText { get; set; }
        public string Title { get; set; }
        public string Subtitle { get; set; }
        public Brush BarBrush { get; set; }

        public Visibility HeaderVisibility
        {
            get { return IsHeader ? Visibility.Visible : Visibility.Collapsed; }
        }

        public Visibility EventVisibility
        {
            get { return IsHeader ? Visibility.Collapsed : Visibility.Visible; }
        }

        public static string DateKey(DateTime d)
        {
            return d.ToString("yyyy-MM-dd");
        }

        public static AgendaRow Header(DateTime date, string text)
        {
            return new AgendaRow { Key = "d" + DateKey(date), Date = date.Date, IsHeader = true, HeaderText = text };
        }

        public static AgendaRow EventRow(EventItem e, DateTime day)
        {
            string sub = e.Location;
            if (!string.IsNullOrEmpty(e.CalendarName))
            {
                sub = string.IsNullOrEmpty(sub) ? e.CalendarName : sub + "  ·  " + e.CalendarName;
            }
            return new AgendaRow
            {
                Key = "e" + DateKey(day) + "|" + e.LocalId + "|" + e.Start.Ticks,
                Date = day.Date,
                IsHeader = false,
                Event = e,
                TimeText = TimeFormat.RangeOnDay(e, day),
                Title = e.Title,
                Subtitle = sub,
                BarBrush = BarFor(e.CalendarArgb)
            };
        }

        /// <summary>Flattens day groups into rows, with a header per day when requested.</summary>
        public static List<AgendaRow> Build(IEnumerable<DayGroup> groups, DateTime today, bool withHeaders)
        {
            var rows = new List<AgendaRow>();
            foreach (DayGroup g in groups)
            {
                if (withHeaders) rows.Add(Header(g.Date, ItalianDates.DayLabel(g.Date, today)));
                foreach (EventItem e in g.Events) rows.Add(EventRow(e, g.Date));
            }
            return rows;
        }

        public static Brush BarFor(uint? argb)
        {
            if (!argb.HasValue) return DefaultBar;
            uint v = argb.Value;
            return new SolidColorBrush(Color.FromArgb((byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8), (byte)v));
        }
    }
}
