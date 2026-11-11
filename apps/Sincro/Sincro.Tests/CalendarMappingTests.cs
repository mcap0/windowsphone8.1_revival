using System;
using Newtonsoft.Json.Linq;
using Sincro.Logic;
using Xunit;

namespace Sincro.Tests
{
    // Event shapes follow the Calendar API reference (events.list), trimmed.
    public class CalendarMappingTests
    {
        private static EventModel Map(string json)
        {
            return CalendarMapper.Map(GoogleJsonReader.Parse(json));
        }

        [Fact]
        public void Timed_event_keeps_its_own_offset_and_duration()
        {
            EventModel e = Map(@"{
              ""id"": ""abc123"", ""status"": ""confirmed"", ""summary"": ""Dentista"",
              ""location"": ""Via Roma 1"", ""description"": ""Portare la tessera"",
              ""start"": { ""dateTime"": ""2026-10-20T09:00:00+02:00"", ""timeZone"": ""Europe/Rome"" },
              ""end"": { ""dateTime"": ""2026-10-20T09:45:00+02:00"", ""timeZone"": ""Europe/Rome"" },
              ""reminders"": { ""useDefault"": true }
            }");
            Assert.False(e.Cancelled);
            Assert.False(e.AllDay);
            Assert.Equal("abc123", e.GoogleId);
            Assert.Equal("Dentista", e.Subject);
            Assert.Equal("Via Roma 1", e.Location);
            Assert.Equal("Portare la tessera", e.Details);
            Assert.Equal(TimeSpan.FromHours(2), e.Start.Offset);
            Assert.Equal(new DateTimeOffset(2026, 10, 20, 7, 0, 0, TimeSpan.Zero), e.Start.ToUniversalTime());
            Assert.Equal(TimeSpan.FromMinutes(45), e.Duration);
            Assert.Equal(TimeSpan.FromMinutes(15), e.Reminder);
        }

        [Fact]
        public void Timed_event_with_negative_offset_across_timezones()
        {
            EventModel e = Map(@"{
              ""id"": ""ny1"", ""summary"": ""Call"",
              ""start"": { ""dateTime"": ""2026-03-10T23:00:00-05:00"" },
              ""end"": { ""dateTime"": ""2026-03-11T00:30:00-05:00"" }
            }");
            Assert.Equal(TimeSpan.FromHours(-5), e.Start.Offset);
            Assert.Equal(new DateTimeOffset(2026, 3, 11, 4, 0, 0, TimeSpan.Zero), e.Start.ToUniversalTime());
            Assert.Equal(TimeSpan.FromMinutes(90), e.Duration);
        }

        [Fact]
        public void All_day_event_uses_exclusive_end_date()
        {
            EventModel e = Map(@"{
              ""id"": ""hol1"", ""summary"": ""Ferie"",
              ""start"": { ""date"": ""2026-10-20"" },
              ""end"": { ""date"": ""2026-10-22"" }
            }");
            Assert.True(e.AllDay);
            Assert.Equal(TimeSpan.FromHours(48), e.Duration);
            Assert.Equal(new DateTime(2026, 10, 20), e.Start.DateTime.Date);
            Assert.Equal(0, e.Start.DateTime.Hour);
        }

        [Fact]
        public void All_day_event_without_end_lasts_one_day()
        {
            EventModel e = Map(@"{ ""id"": ""d1"", ""summary"": ""X"", ""start"": { ""date"": ""2026-12-25"" } }");
            Assert.True(e.AllDay);
            Assert.Equal(TimeSpan.FromHours(24), e.Duration);
        }

        [Fact]
        public void Cancelled_event_is_flagged_and_needs_no_start()
        {
            EventModel e = Map(@"{ ""id"": ""gone"", ""status"": ""cancelled"" }");
            Assert.True(e.Cancelled);
            Assert.Equal("gone", e.GoogleId);
        }

        [Fact]
        public void Event_without_summary_gets_placeholder_title()
        {
            EventModel e = Map(@"{ ""id"": ""n1"", ""start"": { ""dateTime"": ""2026-10-20T09:00:00+02:00"" }, ""end"": { ""dateTime"": ""2026-10-20T10:00:00+02:00"" } }");
            Assert.Equal("(senza titolo)", e.Subject);
        }

        [Fact]
        public void Event_without_id_is_ignored()
        {
            Assert.Null(Map(@"{ ""summary"": ""x"", ""start"": { ""date"": ""2026-10-20"" } }"));
        }

        [Fact]
        public void Reminder_defaults_and_overrides()
        {
            Assert.Equal(TimeSpan.FromMinutes(15), CalendarMapper.ReminderFor(null));
            Assert.Equal(TimeSpan.FromMinutes(15), CalendarMapper.ReminderFor(GoogleJsonReader.Parse(@"{ ""useDefault"": true }")));
            Assert.Equal(TimeSpan.FromMinutes(30), CalendarMapper.ReminderFor(GoogleJsonReader.Parse(@"{ ""useDefault"": false, ""overrides"": [ { ""method"": ""popup"", ""minutes"": 30 }, { ""method"": ""email"", ""minutes"": 60 } ] }")));
            Assert.Null(CalendarMapper.ReminderFor(GoogleJsonReader.Parse(@"{ ""useDefault"": false }")));
        }

        [Fact]
        public void Fingerprint_changes_when_visible_content_changes()
        {
            EventModel a = Map(@"{ ""id"": ""f"", ""summary"": ""A"", ""start"": { ""date"": ""2026-10-20"" } }");
            EventModel b = Map(@"{ ""id"": ""f"", ""summary"": ""B"", ""start"": { ""date"": ""2026-10-20"" } }");
            EventModel same = Map(@"{ ""id"": ""f"", ""summary"": ""A"", ""start"": { ""date"": ""2026-10-20"" } }");
            Assert.NotEqual(a.Fingerprint, b.Fingerprint);
            Assert.Equal(a.Fingerprint, same.Fingerprint);
        }
    }
}
