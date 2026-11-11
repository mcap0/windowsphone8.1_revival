using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Calendario.Data;
using Calendario.Logic;
using Calendario.Models;
using Windows.UI;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media;
using Windows.UI.Xaml.Shapes;

namespace Calendario
{
    /// <summary>Vista a 4 giorni: colonne con griglia oraria (7 giorni non entrano a 480 px logici).</summary>
    public sealed partial class MainPage
    {
        private const int FourDays = 4;
        private const double HourHeight = 44;
        private const double GutterWidth = 30;

        private static readonly SolidColorBrush LineBrush = new SolidColorBrush(Color.FromArgb(0x33, 0xFF, 0xFF, 0xFF));
        private static readonly SolidColorBrush NowBrush = new SolidColorBrush(Color.FromArgb(0xFF, 0xE8, 0x11, 0x23));
        private static readonly SolidColorBrush TextOnBar = new SolidColorBrush(Colors.White);

        private DateTime fourStart = DateTime.Today;
        private bool fourLoaded;
        private bool fourBusy;
        private int fourVersion;

        private sealed class Segment
        {
            public EventItem Event;
            public DateTime Start;
            public DateTime End;
            public int Lane;
        }

        private async Task LoadFourAsync(bool scrollToMorning)
        {
            if (fourBusy) return;
            fourBusy = true;
            int version = ++fourVersion;
            try
            {
                DateTime from = fourStart.Date;
                List<EventItem> events = await CalendarReader.LoadAsync(from, from.AddDays(FourDays));
                if (version != fourVersion) return;
                BuildFour(from, events);
                fourLoaded = true;
                if (scrollToMorning)
                {
                    FourScroll.UpdateLayout();
                    double hour = from <= DateTime.Today && DateTime.Today < from.AddDays(FourDays)
                        ? Math.Max(0, DateTime.Now.Hour - 1) : 7;
                    FourScroll.ChangeView(null, Math.Min(hour, 16) * HourHeight, null, true);
                }
            }
            catch (Exception ex)
            {
                ShowError(ex);
            }
            finally
            {
                fourBusy = false;
            }
        }

        private void BuildFour(DateTime from, List<EventItem> events)
        {
            DateTime today = DateTime.Today;
            DateTime last = from.AddDays(FourDays - 1);
            FourTitleText.Text = from.Month == last.Month
                ? from.Day + "–" + last.Day + " " + ItalianDates.MonthLong(last.Month)
                : from.Day + " " + ItalianDates.MonthLong(from.Month) + " – " + last.Day + " " + ItalianDates.MonthLong(last.Month);

            ResetGrid(FourHeader);
            ResetGrid(FourAllDay);
            ResetGrid(FourHost);
            FourHost.RowDefinitions.Add(new RowDefinition { Height = new GridLength(24 * HourHeight) });

            // Ore nella colonna di sinistra e linee orizzontali su tutta la larghezza.
            var gutter = new Canvas();
            FourHost.Children.Add(gutter);
            var lines = new Canvas { IsHitTestVisible = false };
            Grid.SetColumn(lines, 1);
            Grid.SetColumnSpan(lines, FourDays);
            FourHost.Children.Add(lines);
            for (int h = 0; h < 24; h++)
            {
                var label = new TextBlock { Text = h.ToString("00"), FontSize = 12, Opacity = 0.6 };
                Canvas.SetTop(label, h * HourHeight - (h == 0 ? 0 : 7));
                gutter.Children.Add(label);
                var line = new Rectangle { Height = 1, Width = 2000, Fill = LineBrush };
                Canvas.SetTop(line, h * HourHeight);
                lines.Children.Add(line);
            }

            for (int i = 0; i < FourDays; i++)
            {
                DateTime day = from.AddDays(i);
                int column = i + 1;

                var head = new TextBlock
                {
                    Text = ItalianDates.WeekdayShort(day.DayOfWeek) + " " + day.Day,
                    FontSize = 16,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    FontWeight = day == today ? Windows.UI.Text.FontWeights.Bold : Windows.UI.Text.FontWeights.Normal
                };
                if (day == today) head.Foreground = DotBrush;
                Grid.SetColumn(head, column);
                FourHeader.Children.Add(head);

                var allDay = new StackPanel { Margin = new Thickness(1, 0, 1, 0) };
                Grid.SetColumn(allDay, column);
                FourAllDay.Children.Add(allDay);

                var segments = new List<Segment>();
                DateTime dayEnd = day.AddDays(1);
                foreach (EventItem e in events)
                {
                    if (e.End <= day && !(e.Duration <= TimeSpan.Zero && e.Start >= day && e.Start < dayEnd)) continue;
                    if (e.Start >= dayEnd) continue;
                    DateTime s = e.Start < day ? day : e.Start;
                    DateTime en = e.End > dayEnd ? dayEnd : e.End;
                    if (e.AllDay || (s == day && en == dayEnd))
                    {
                        allDay.Children.Add(Block(e, e.Title, 12, 20, day));
                        continue;
                    }
                    segments.Add(new Segment { Event = e, Start = s, End = en });
                }

                var dayGrid = new Grid { Background = EmptyBrush, Tag = day };
                dayGrid.Tapped += FourDay_Tapped;
                Grid.SetColumn(dayGrid, column);
                FourHost.Children.Add(dayGrid);

                int lanes = AssignLanes(segments);
                for (int l = 0; l < lanes; l++)
                {
                    dayGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                }
                foreach (Segment seg in segments)
                {
                    double top = (seg.Start - day).TotalHours * HourHeight;
                    double height = Math.Max(20, (seg.End - seg.Start).TotalHours * HourHeight - 1);
                    string text = seg.Event.Title;
                    if (height >= 36) text = TimeFormat.RangeOnDay(seg.Event, day) + "\n" + text;
                    FrameworkElement block = Block(seg.Event, text, 12, height, day);
                    block.VerticalAlignment = VerticalAlignment.Top;
                    block.Margin = new Thickness(1, top, 1, 0);
                    Grid.SetColumn(block, seg.Lane);
                    dayGrid.Children.Add(block);
                }

                if (day == today)
                {
                    var now = new Rectangle
                    {
                        Height = 2,
                        Fill = NowBrush,
                        VerticalAlignment = VerticalAlignment.Top,
                        IsHitTestVisible = false,
                        Margin = new Thickness(0, (DateTime.Now - day).TotalHours * HourHeight, 0, 0)
                    };
                    Grid.SetColumnSpan(now, Math.Max(1, lanes));
                    dayGrid.Children.Add(now);
                }
            }
        }

        private static void ResetGrid(Grid grid)
        {
            grid.Children.Clear();
            grid.RowDefinitions.Clear();
            grid.ColumnDefinitions.Clear();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(GutterWidth) });
            for (int i = 0; i < FourDays; i++)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            }
        }

        /// <summary>Corsie per eventi sovrapposti: ognuno nella prima corsia libera. Restituisce il numero di corsie.</summary>
        private static int AssignLanes(List<Segment> segments)
        {
            segments.Sort((a, b) => a.Start != b.Start ? a.Start.CompareTo(b.Start) : b.End.CompareTo(a.End));
            var laneEnds = new List<DateTime>();
            foreach (Segment seg in segments)
            {
                int lane = laneEnds.FindIndex(end => end <= seg.Start);
                if (lane < 0)
                {
                    lane = laneEnds.Count;
                    laneEnds.Add(seg.End);
                }
                else
                {
                    laneEnds[lane] = seg.End;
                }
                seg.Lane = lane;
            }
            return Math.Max(1, laneEnds.Count);
        }

        private FrameworkElement Block(EventItem e, string text, double fontSize, double height, DateTime day)
        {
            var border = new Border
            {
                Background = AgendaRow.BarFor(e.CalendarArgb),
                Height = height,
                Margin = new Thickness(0, 0, 0, 1),
                Padding = new Thickness(3, 1, 2, 1),
                Child = new TextBlock
                {
                    Text = text,
                    FontSize = fontSize,
                    Foreground = TextOnBar,
                    TextWrapping = TextWrapping.Wrap,
                    TextTrimming = TextTrimming.CharacterEllipsis
                },
                Tag = e
            };
            border.Tapped += FourEvent_Tapped;
            return border;
        }

        private void FourEvent_Tapped(object sender, TappedRoutedEventArgs e)
        {
            e.Handled = true;
            var ev = (sender as FrameworkElement).Tag as EventItem;
            if (ev == null) return;
            App.SelectedEvent = ev;
            Frame.Navigate(typeof(DetailPage));
        }

        private void FourDay_Tapped(object sender, TappedRoutedEventArgs e)
        {
            var grid = sender as FrameworkElement;
            if (grid == null || !(grid.Tag is DateTime)) return;
            Frame.Navigate(typeof(DayPage), (DateTime)grid.Tag);
        }

        private void PrevFour_Click(object sender, RoutedEventArgs e)
        {
            fourStart = fourStart.AddDays(-FourDays);
            LoadFourAsync(false);
        }

        private void NextFour_Click(object sender, RoutedEventArgs e)
        {
            fourStart = fourStart.AddDays(FourDays);
            LoadFourAsync(false);
        }
    }
}
