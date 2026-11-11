using System;
using System.Collections.Generic;
using System.Linq;
using Newtonsoft.Json.Linq;
using Sincro.Logic;
using Xunit;

namespace Sincro.Tests
{
    public class DiffTests
    {
        private static void Same(string[] expected, IEnumerable<string> actual)
        {
            var list = actual.ToList();
            Assert.True(expected.SequenceEqual(list), "expected [" + string.Join(",", expected) + "] but was [" + string.Join(",", list) + "]");
        }

        private static ContactModel Person(string name)
        {
            return new ContactModel { ResourceName = name, DisplayName = name };
        }

        private static ContactModel Deleted(string name)
        {
            return new ContactModel { ResourceName = name, Deleted = true };
        }

        [Fact]
        public void Full_sync_adds_updates_and_removes_contacts_missing_from_listing()
        {
            var listed = new List<ContactModel> { Person("people/a"), Person("people/b") };
            ContactPlan plan = ContactDiff.Plan(listed, true, new[] { "people/a", "people/old" });

            Same(new[] { "people/a", "people/b" }, plan.Upserts.Select(c => c.ResourceName).ToArray());
            Same(new[] { "people/old" }, plan.Deletes.ToArray());
            Assert.Equal(new HashSet<string> { "people/a", "people/b" }, plan.NewKnown);
        }

        [Fact]
        public void Incremental_sync_only_touches_listed_people_and_deletions()
        {
            var changed = new List<ContactModel> { Person("people/b"), Deleted("people/c") };
            ContactPlan plan = ContactDiff.Plan(changed, false, new[] { "people/a", "people/c" });

            Same(new[] { "people/b" }, plan.Upserts.Select(c => c.ResourceName).ToArray());
            Same(new[] { "people/c" }, plan.Deletes.ToArray());
            Assert.Equal(new HashSet<string> { "people/a", "people/b" }, plan.NewKnown);
        }

        [Fact]
        public void Incremental_sync_keeps_unlisted_known_contacts()
        {
            ContactPlan plan = ContactDiff.Plan(new List<ContactModel>(), false, new[] { "people/a" });
            Assert.Empty(plan.Deletes);
            Assert.Equal(new HashSet<string> { "people/a" }, plan.NewKnown);
        }

        [Fact]
        public void Duplicate_people_in_one_pass_are_written_once()
        {
            ContactPlan plan = ContactDiff.Plan(new List<ContactModel> { Person("people/a"), Person("people/a") }, false, new string[0]);
            Assert.Equal(1, plan.Upserts.Count);
        }

        private static EventModel Ev(string id, string subject, bool cancelled = false)
        {
            return new EventModel
            {
                GoogleId = id,
                Subject = subject,
                Cancelled = cancelled,
                Start = new DateTimeOffset(2026, 10, 20, 9, 0, 0, TimeSpan.Zero),
                Duration = TimeSpan.FromHours(1),
            };
        }

        [Fact]
        public void Calendar_plan_adds_new_updates_changed_and_deletes_missing()
        {
            var stored = new Dictionary<string, string>
            {
                { "keep", Ev("keep", "Same").Fingerprint },
                { "edit", Ev("edit", "Old title").Fingerprint },
                { "gone", Ev("gone", "Removed upstream").Fingerprint },
            };
            var events = new List<EventModel>
            {
                Ev("keep", "Same"),
                Ev("edit", "New title"),
                Ev("fresh", "Brand new"),
            };

            CalendarPlan plan = CalendarDiff.Plan(events, stored);

            Same(new[] { "fresh" }, plan.Adds.Select(e => e.GoogleId).ToArray());
            Same(new[] { "edit" }, plan.Updates.Select(e => e.GoogleId).ToArray());
            Same(new[] { "gone" }, plan.Deletes.ToArray());
        }

        [Fact]
        public void Cancelled_events_delete_known_ids_and_are_never_added()
        {
            var stored = new Dictionary<string, string> { { "x", "fp" } };
            var events = new List<EventModel> { Ev("x", "t", cancelled: true), Ev("y", "t", cancelled: true) };

            CalendarPlan plan = CalendarDiff.Plan(events, stored);

            Same(new[] { "x" }, plan.Deletes.ToArray());
            Assert.Empty(plan.Adds);
            Assert.Empty(plan.Updates);
        }

        [Fact]
        public void Unchanged_events_produce_no_work()
        {
            var stored = new Dictionary<string, string> { { "a", Ev("a", "T").Fingerprint } };
            CalendarPlan plan = CalendarDiff.Plan(new List<EventModel> { Ev("a", "T") }, stored);
            Assert.Empty(plan.Adds);
            Assert.Empty(plan.Updates);
            Assert.Empty(plan.Deletes);
        }
    }

    public class RemoteIdTests
    {
        [Fact]
        public void For_prefixes_resource_name_with_app_tag()
        {
            string id = RemoteIds.For("people/c1");
            Assert.StartsWith(RemoteIds.Prefix, id);
            Assert.EndsWith("people/c1", id);
        }

        [Fact]
        public void Round_trip_returns_resource_name()
        {
            Assert.Equal("people/c99", RemoteIds.ResourceNameOf(RemoteIds.For("people/c99")));
        }

        [Fact]
        public void Foreign_remote_ids_are_not_ours()
        {
            Assert.False(RemoteIds.IsOwn("people/c1"));
            Assert.False(RemoteIds.IsOwn("sincro:other-guid:people/c1"));
            Assert.False(RemoteIds.IsOwn(null));
            Assert.Null(RemoteIds.ResourceNameOf("people/c1"));
        }

        [Fact]
        public void For_rejects_empty_resource_name()
        {
            Assert.Throws<ArgumentException>(() => RemoteIds.For(""));
        }
    }

    public class GoogleErrorTests
    {
        [Fact]
        public void Gone_or_expired_sync_token_triggers_full_resync()
        {
            Assert.True(GoogleErrors.IsExpiredSyncToken(410, ""));
            Assert.True(GoogleErrors.IsExpiredSyncToken(400, "{\"error\":{\"status\":\"FAILED_PRECONDITION\",\"message\":\"EXPIRED_SYNC_TOKEN\"}}"));
        }

        [Fact]
        public void Other_errors_are_not_expired_tokens()
        {
            Assert.False(GoogleErrors.IsExpiredSyncToken(401, "{\"error\":\"invalid_grant\"}"));
            Assert.False(GoogleErrors.IsExpiredSyncToken(500, null));
        }
    }
}
