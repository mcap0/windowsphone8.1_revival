using System;
using System.Collections.Generic;
using System.Linq;

namespace Sincro.Logic
{
    public sealed class ContactPlan
    {
        public List<ContactModel> Upserts = new List<ContactModel>();
        public List<string> Deletes = new List<string>();        // resource names to remove from the phone
        public HashSet<string> NewKnown = new HashSet<string>(); // resource names that exist on the phone afterwards
    }

    public static class ContactDiff
    {
        /// <param name="changed">People returned by this pass (all people on a full sync; changed ones plus deletions otherwise).</param>
        /// <param name="fullSync">True when the pass lists every contact; contacts not listed are then removed.</param>
        /// <param name="known">Resource names currently stored on the phone by this app.</param>
        public static ContactPlan Plan(IEnumerable<ContactModel> changed, bool fullSync, IEnumerable<string> known)
        {
            var plan = new ContactPlan();
            var knownSet = new HashSet<string>(known ?? Enumerable.Empty<string>());
            var seen = new HashSet<string>();

            foreach (ContactModel c in changed)
            {
                if (c == null || string.IsNullOrEmpty(c.ResourceName)) continue;
                if (c.Deleted)
                {
                    plan.Deletes.Add(c.ResourceName);
                    knownSet.Remove(c.ResourceName);
                    continue;
                }
                if (seen.Add(c.ResourceName)) plan.Upserts.Add(c);
                knownSet.Add(c.ResourceName);
            }

            if (fullSync)
            {
                // Anything we had but Google no longer lists is gone.
                foreach (string name in knownSet.ToList())
                {
                    if (!seen.Contains(name))
                    {
                        plan.Deletes.Add(name);
                        knownSet.Remove(name);
                    }
                }
            }

            plan.Deletes = plan.Deletes.Distinct().ToList();
            plan.NewKnown = knownSet;
            return plan;
        }
    }

    public sealed class CalendarPlan
    {
        public List<EventModel> Adds = new List<EventModel>();
        public List<EventModel> Updates = new List<EventModel>();
        public List<string> Deletes = new List<string>(); // Google ids to remove from the phone
    }

    public static class CalendarDiff
    {
        /// <summary>
        /// Full window listing against the stored map (Google id -> fingerprint of what was written).
        /// Events missing from the listing, or cancelled, are deletes.
        /// </summary>
        public static CalendarPlan Plan(IEnumerable<EventModel> events, IDictionary<string, string> stored)
        {
            var plan = new CalendarPlan();
            var listed = new HashSet<string>();

            foreach (EventModel e in events)
            {
                if (e == null || string.IsNullOrEmpty(e.GoogleId)) continue;
                if (e.Cancelled)
                {
                    if (stored.ContainsKey(e.GoogleId)) plan.Deletes.Add(e.GoogleId);
                    continue;
                }
                if (!listed.Add(e.GoogleId)) continue; // duplicate across pages
                string fp;
                if (!stored.TryGetValue(e.GoogleId, out fp)) plan.Adds.Add(e);
                else if (fp != e.Fingerprint) plan.Updates.Add(e);
            }

            foreach (string id in stored.Keys)
            {
                if (!listed.Contains(id) && !plan.Deletes.Contains(id)) plan.Deletes.Add(id);
            }
            return plan;
        }
    }
}
