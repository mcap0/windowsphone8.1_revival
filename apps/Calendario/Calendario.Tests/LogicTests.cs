using System;
using System.Linq;
using Calendario.Logic;
using Xunit;

namespace Calendario.Tests
{
    public class ItalianDatesTests
    {
        [Fact]
        public void DayLabelUsesOggiDomaniIeri()
        {
            DateTime today = new DateTime(2026, 10, 20);
            Assert.Equal("Oggi", ItalianDates.DayLabel(today, today));
            Assert.Equal("Domani", ItalianDates.DayLabel(today.AddDays(1), today));
            Assert.Equal("Ieri", ItalianDates.DayLabel(today.AddDays(-1), today));
        }

        [Fact]
        public void DayLabelOtherDaysAreLongItalian()
        {
            // 21 October 2019 was a Monday.
            Assert.Equal("lunedì 21 ottobre", ItalianDates.DayLabel(new DateTime(2019, 10, 21), new DateTime(2026, 10, 20)));
            Assert.Equal("domenica 1 novembre", ItalianDates.DayLabel(new DateTime(2026, 11, 1), new DateTime(2026, 10, 20)));
        }

        [Fact]
        public void WeekdayIsMondayFirst()
        {
            Assert.Equal(0, ItalianDates.MondayIndex(DayOfWeek.Monday));
            Assert.Equal(6, ItalianDates.MondayIndex(DayOfWeek.Sunday));
            Assert.Equal("Dom", ItalianDates.WeekdayShort(DayOfWeek.Sunday));
            Assert.Equal("mercoledì", ItalianDates.WeekdayLong(DayOfWeek.Wednesday));
        }

        [Fact]
        public void MonthTitleIsCapitalised()
        {
            Assert.Equal("Ottobre 2026", ItalianDates.MonthTitle(2026, 10));
        }
    }

    public class MonthGridTests
    {
        [Fact]
        public void October2026StartsOnMondayTwentyEighthOfSeptember()
        {
            // 1 Oct 2026 is a Thursday.
            DateTime start = MonthGrid.GridStart(2026, 10);
            Assert.Equal(new DateTime(2026, 9, 28), start);
            Assert.Equal(DayOfWeek.Monday, start.DayOfWeek);
        }

        [Fact]
        public void MonthStartingOnMondayHasNoLeadingDays()
        {
            // 1 June 2026 is a Monday.
            Assert.Equal(new DateTime(2026, 6, 1), MonthGrid.GridStart(2026, 6));
        }

        [Fact]
        public void LeapFebruaryIsCoveredCompletely()
        {
            // 1 Feb 2028 is a Tuesday; the grid starts Monday 31 January 2028.
            DateTime[] cells = MonthGrid.Build(2028, 2);
            Assert.Equal(new DateTime(2028, 1, 31), cells[0]);
            Assert.Equal(29, cells.Count(c => c.Month == 2));
        }

        [Theory]
        [InlineData(2024)]
        [InlineData(2025)]
        [InlineData(2026)]
        [InlineData(2027)]
        [InlineData(2028)]
        [InlineData(2100)]
        public void EveryMonthGridIsValid(int year)
        {
            for (int month = 1; month <= 12; month++)
            {
                DateTime[] cells = MonthGrid.Build(year, month);
                Assert.Equal(MonthGrid.Cells, cells.Length);
                Assert.Equal(42, cells.Length);
                Assert.Equal(DayOfWeek.Monday, cells[0].DayOfWeek);
                Assert.True(cells[0] <= new DateTime(year, month, 1));
                Assert.True(new DateTime(year, month, 1) - cells[0] < TimeSpan.FromDays(7));
                for (int i = 1; i < cells.Length; i++)
                {
                    Assert.Equal(TimeSpan.FromDays(1), cells[i] - cells[i - 1]);
                }
                int inMonth = cells.Count(c => c.Month == month && c.Year == year);
                Assert.Equal(DateTime.DaysInMonth(year, month), inMonth);
            }
        }

        [Fact]
        public void AddMonthsCrossesYearBoundary()
        {
            Assert.Equal(new DateTime(2027, 1, 1), MonthGrid.AddMonths(new DateTime(2026, 12, 1), 1));
            Assert.Equal(new DateTime(2026, 12, 1), MonthGrid.AddMonths(new DateTime(2027, 1, 1), -1));
        }
    }

    public class TimeFormatTests
    {
        private static EventItem Timed(DateTime start, TimeSpan dur)
        {
            return new EventItem { Title = "x", Start = start, Duration = dur, AllDay = false };
        }

        [Fact]
        public void TimedRangeUsesEnDash()
        {
            var e = Timed(new DateTime(2026, 10, 20, 9, 30, 0), TimeSpan.FromHours(1));
            Assert.Equal("09:30–10:30", TimeFormat.Range(e));
        }

        [Fact]
        public void ZeroDurationShowsStartOnly()
        {
            var e = Timed(new DateTime(2026, 10, 20, 9, 30, 0), TimeSpan.Zero);
            Assert.Equal("09:30", TimeFormat.Range(e));
        }

        [Fact]
        public void EventEndingAtMidnightStaysOnItsDay()
        {
            var e = Timed(new DateTime(2026, 10, 20, 23, 0, 0), TimeSpan.FromHours(1));
            Assert.Equal(new DateTime(2026, 10, 20), e.LastDay);
            Assert.False(e.SpansDays);
            Assert.Equal("23:00–00:00", TimeFormat.Range(e));
        }

        [Fact]
        public void MultiDayTimedEventShowsDates()
        {
            var e = Timed(new DateTime(2026, 10, 12, 9, 0, 0), TimeSpan.FromHours(25));
            Assert.True(e.SpansDays);
            Assert.Equal("12/10 09:00–13/10 10:00", TimeFormat.Range(e));
        }

        [Fact]
        public void MultiDayTimedEventOnMiddleDayIsAllDayLabel()
        {
            var e = Timed(new DateTime(2026, 10, 12, 9, 0, 0), TimeSpan.FromHours(60));
            Assert.Equal("Tutto il giorno", TimeFormat.RangeOnDay(e, new DateTime(2026, 10, 13)));
            Assert.Equal("09:00–24:00", TimeFormat.RangeOnDay(e, new DateTime(2026, 10, 12)));
            Assert.Equal("00:00–21:00", TimeFormat.RangeOnDay(e, new DateTime(2026, 10, 14)));
        }

        [Fact]
        public void AllDayLabels()
        {
            var one = new EventItem { AllDay = true, Start = new DateTime(2026, 10, 20), Duration = TimeSpan.FromDays(1) };
            var three = new EventItem { AllDay = true, Start = new DateTime(2026, 10, 20), Duration = TimeSpan.FromDays(3) };
            Assert.Equal("Tutto il giorno", TimeFormat.Range(one));
            Assert.Equal("Tutto il giorno (3 giorni)", TimeFormat.Range(three));
        }

        [Fact]
        public void ReminderTexts()
        {
            Assert.Equal("Nessuno", TimeFormat.ReminderText(null));
            Assert.Equal("All'orario dell'evento", TimeFormat.ReminderText(TimeSpan.Zero));
            Assert.Equal("15 min prima", TimeFormat.ReminderText(TimeSpan.FromMinutes(15)));
            Assert.Equal("1 ora prima", TimeFormat.ReminderText(TimeSpan.FromHours(1)));
            Assert.Equal("2 ore prima", TimeFormat.ReminderText(TimeSpan.FromHours(2)));
            Assert.Equal("1 giorno prima", TimeFormat.ReminderText(TimeSpan.FromDays(1)));
            Assert.Equal("2 giorni prima", TimeFormat.ReminderText(TimeSpan.FromDays(2)));
        }
    }

    public class AgendaBuilderTests
    {
        private static EventItem Ev(string id, DateTime start, TimeSpan dur, bool allDay = false, string title = null)
        {
            return new EventItem { LocalId = id, Title = title ?? id, Start = start, Duration = dur, AllDay = allDay };
        }

        [Fact]
        public void AllDayEventsSortBeforeTimedInsideADay()
        {
            var day = new DateTime(2026, 10, 20);
            var events = new[]
            {
                Ev("timed8", day.AddHours(8), TimeSpan.FromHours(1)),
                Ev("allday", day, TimeSpan.FromDays(1), true),
                Ev("timed7", day.AddHours(7), TimeSpan.FromHours(1)),
            };
            var groups = AgendaBuilder.Group(events, day, day);
            Assert.Equal(1, groups.Count);
            string[] expected = { "allday", "timed7", "timed8" };
            string[] actual = groups[0].Events.Select(e => e.LocalId).ToArray();
            Assert.Equal(expected, actual);
        }

        [Fact]
        public void MultiDayEventAppearsOnEveryDayInRange()
        {
            var events = new[] { Ev("trip", new DateTime(2026, 10, 19), TimeSpan.FromDays(3), true) };
            var groups = AgendaBuilder.Group(events, new DateTime(2026, 10, 18), new DateTime(2026, 10, 25));
            Assert.Equal(new[] { new DateTime(2026, 10, 19), new DateTime(2026, 10, 20), new DateTime(2026, 10, 21) },
                groups.Select(g => g.Date).ToArray());
        }

        [Fact]
        public void EventsAreClippedToRange()
        {
            var events = new[] { Ev("long", new DateTime(2026, 10, 1), TimeSpan.FromDays(60), true) };
            var groups = AgendaBuilder.Group(events, new DateTime(2026, 10, 20), new DateTime(2026, 10, 21));
            Assert.Equal(2, groups.Count);
            Assert.Equal(new DateTime(2026, 10, 20), groups[0].Date);
        }

        [Fact]
        public void DaysWithoutEventsAreSkipped()
        {
            var events = new[] { Ev("a", new DateTime(2026, 10, 20, 10, 0, 0), TimeSpan.FromHours(1)) };
            var groups = AgendaBuilder.Group(events, new DateTime(2026, 10, 19), new DateTime(2026, 10, 22));
            Assert.Equal(1, groups.Count);
            Assert.Equal(new DateTime(2026, 10, 20), groups[0].Date);
        }

        [Fact]
        public void AllDayDateUsesLocalMidnightWhenPresent()
        {
            // Sincro writes local midnight: local 20 Oct 00:00 (UTC+2 = 19 Oct 22:00 UTC).
            DateTime local = new DateTime(2026, 10, 20, 0, 0, 0);
            DateTime utc = new DateTime(2026, 10, 19, 22, 0, 0);
            Assert.Equal(new DateTime(2026, 10, 20), AgendaBuilder.AllDayDate(local, utc));
        }

        [Fact]
        public void AllDayDateFallsBackToUtcMidnight()
        {
            // Other calendars store UTC midnight: UTC-5 user sees 19 Oct 19:00 local, UTC 20 Oct 00:00.
            DateTime local = new DateTime(2026, 10, 19, 19, 0, 0);
            DateTime utc = new DateTime(2026, 10, 20, 0, 0, 0);
            Assert.Equal(new DateTime(2026, 10, 20), AgendaBuilder.AllDayDate(local, utc));
        }

        [Fact]
        public void AllDayDurationIsWholeDaysAtLeastOne()
        {
            Assert.Equal(TimeSpan.FromDays(1), AgendaBuilder.AllDayDuration(TimeSpan.Zero));
            Assert.Equal(TimeSpan.FromDays(1), AgendaBuilder.AllDayDuration(TimeSpan.FromHours(24)));
            Assert.Equal(TimeSpan.FromDays(3), AgendaBuilder.AllDayDuration(TimeSpan.FromHours(71)));
        }

        [Fact]
        public void NextTodayPicksFirstUpcomingTimedEvent()
        {
            DateTime now = new DateTime(2026, 10, 20, 10, 0, 0);
            var events = new[]
            {
                Ev("past", new DateTime(2026, 10, 20, 8, 0, 0), TimeSpan.FromHours(1)),
                Ev("later", new DateTime(2026, 10, 20, 15, 0, 0), TimeSpan.FromHours(1)),
                Ev("soon", new DateTime(2026, 10, 20, 11, 0, 0), TimeSpan.FromHours(1)),
            };
            Assert.Equal("soon", AgendaBuilder.NextToday(events, now).LocalId);
        }

        [Fact]
        public void NextTodayFallsBackToAllDayThenNull()
        {
            DateTime now = new DateTime(2026, 10, 20, 23, 0, 0);
            var allDay = new[] { Ev("holiday", new DateTime(2026, 10, 20), TimeSpan.FromDays(1), true) };
            Assert.Equal("holiday", AgendaBuilder.NextToday(allDay, now).LocalId);
            var tomorrow = new[] { Ev("tomorrow", new DateTime(2026, 10, 21, 9, 0, 0), TimeSpan.FromHours(1)) };
            Assert.Null(AgendaBuilder.NextToday(tomorrow, now));
        }

        [Fact]
        public void DaysWithEventsForMonthDots()
        {
            var events = new[]
            {
                Ev("a", new DateTime(2026, 10, 3, 9, 0, 0), TimeSpan.FromHours(1)),
                Ev("b", new DateTime(2026, 10, 3, 12, 0, 0), TimeSpan.FromHours(1)),
            };
            var dots = AgendaBuilder.DaysWithEvents(events, new DateTime(2026, 9, 28), new DateTime(2026, 10, 11));
            Assert.Equal(1, dots.Count);
            Assert.True(dots.Contains(new DateTime(2026, 10, 3)));
        }
    }
}
