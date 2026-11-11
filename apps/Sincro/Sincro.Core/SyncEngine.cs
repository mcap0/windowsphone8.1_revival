using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using Sincro.Logic;
using Windows.ApplicationModel.Appointments;
using Windows.Phone.PersonalInformation;

namespace Sincro.Core
{
    public sealed class SyncReport
    {
        public readonly List<string> Errors = new List<string>();
        public string ContactsText = "";
        public string CalendarText = "";
    }

    /// <summary>One Google -> phone pass. Used by the UI ("Sincronizza ora") and by the background task.</summary>
    public static class SyncEngine
    {
        // Only guards this process; the background task runs in its own process.
        private static readonly SemaphoreSlim Gate = new SemaphoreSlim(1, 1);

        public const int CalendarEveryHoursInBackground = 2;

        /// <param name="scheduled">true from the background task: contacts incremental, calendar at most every 2 h.</param>
        public static async Task<SyncReport> RunAsync(bool contacts, bool calendar, bool scheduled)
        {
            var report = new SyncReport();
            await Gate.WaitAsync();
            try
            {
                SyncState state = await SyncStateStore.LoadAsync();
                GoogleCredentials creds = Vault.Load();
                if (creds == null)
                {
                    report.Errors.Add("Accesso Google non impostato.");
                    return report;
                }
                var api = new GoogleApi(creds);

                if (contacts)
                {
                    try { await SyncContactsAsync(api, state, report); }
                    catch (Exception ex) { report.Errors.Add("Contatti: " + ex.Message); }
                }

                bool calendarDue = !scheduled || state.LastCalendarSync == null
                    || DateTimeOffset.UtcNow - state.LastCalendarSync.Value >= TimeSpan.FromHours(CalendarEveryHoursInBackground);
                if (calendar && calendarDue)
                {
                    try { await SyncCalendarAsync(api, state, report); }
                    catch (Exception ex) { report.Errors.Add("Calendario: " + ex.Message); }
                }

                foreach (string err in report.Errors) state.AddLog(err);
                await SyncStateStore.SaveAsync(state);
                return report;
            }
            finally
            {
                Gate.Release();
            }
        }

        /// <summary>Removes everything this app wrote to the phone (not the Google account, not the credentials).</summary>
        public static async Task RemoveSyncedDataAsync()
        {
            await Gate.WaitAsync();
            try
            {
                SyncState state = await SyncStateStore.LoadAsync();

                try
                {
                    ContactStore cs = await ContactWriter.OpenStoreAsync();
                    foreach (string name in state.KnownContacts.ToList())
                    {
                        await ContactWriter.DeleteAsync(cs, name);
                    }
                }
                catch (Exception ex) { state.AddLog("Contatti: " + ex.Message); }

                try
                {
                    // Deletes our appointments one by one; the empty "Google" calendar is left in place.
                    AppointmentStore store = await CalendarWriter.OpenStoreAsync();
                    AppointmentCalendar cal = await CalendarWriter.GetOrCreateCalendarAsync(store);
                    foreach (CalendarEntry entry in state.CalendarEvents.Values)
                    {
                        await CalendarWriter.DeleteAsync(cal, entry.LocalId);
                    }
                }
                catch (Exception ex) { state.AddLog("Calendario: " + ex.Message); }

                state.KnownContacts = new List<string>();
                state.ContactsSyncToken = null;
                state.ContactsCount = 0;
                state.CalendarEvents = new Dictionary<string, CalendarEntry>();
                state.CalendarCount = 0;
                state.LastContactsSync = null;
                state.LastCalendarSync = null;
                state.AddLog("Dati sincronizzati rimossi");
                await SyncStateStore.SaveAsync(state);
            }
            finally
            {
                Gate.Release();
            }
        }

        private sealed class PeoplePass
        {
            public readonly List<ContactModel> People = new List<ContactModel>();
            public string SyncToken;
        }

        private static async Task SyncContactsAsync(GoogleApi api, SyncState state, SyncReport report)
        {
            PeoplePass pass;
            bool full = state.ContactsSyncToken == null;
            try
            {
                pass = await FetchPeopleAsync(api, full ? null : state.ContactsSyncToken);
            }
            catch (GoogleApiException ex) when (!full && GoogleErrors.IsExpiredSyncToken(ex.Status, ex.Body))
            {
                // Google dropped our token: list everything again and rebuild.
                full = true;
                state.ContactsSyncToken = null;
                pass = await FetchPeopleAsync(api, null);
            }

            ContactPlan plan = ContactDiff.Plan(pass.People, full, state.KnownContacts);
            ContactStore store = await ContactWriter.OpenStoreAsync();

            foreach (string name in plan.Deletes)
            {
                try { await ContactWriter.DeleteAsync(store, name); }
                catch (Exception ex) { report.Errors.Add("Contatto " + name + ": " + ex.Message); }
            }
            int written = 0;
            foreach (ContactModel m in plan.Upserts)
            {
                try { await ContactWriter.UpsertAsync(store, m); written++; }
                catch (Exception ex) { report.Errors.Add("Contatto " + m.DisplayName + ": " + ex.Message); }
            }

            state.KnownContacts = plan.NewKnown.ToList();
            state.ContactsCount = plan.NewKnown.Count;
            state.ContactsSyncToken = pass.SyncToken;
            state.LastContactsSync = DateTimeOffset.Now;
            report.ContactsText = (full ? "Completa: " : "Modifiche: ") + written + " scritti, " + plan.Deletes.Count + " rimossi";
            state.AddLog("Contatti " + report.ContactsText);
        }

        private static async Task<PeoplePass> FetchPeopleAsync(GoogleApi api, string syncToken)
        {
            var pass = new PeoplePass();
            string page = null;
            do
            {
                JObject json = await api.GetJsonAsync(GoogleEndpoints.Connections(syncToken, page));
                var list = json["connections"] as JArray;
                if (list != null)
                {
                    foreach (JToken t in list)
                    {
                        ContactModel m = ContactMapper.Map(t as JObject);
                        if (m != null) pass.People.Add(m);
                    }
                }
                page = (string)json["nextPageToken"];
                if (page == null) pass.SyncToken = (string)json["nextSyncToken"];
            }
            while (page != null);
            return pass;
        }

        private static async Task SyncCalendarAsync(GoogleApi api, SyncState state, SyncReport report)
        {
            // Full window listing every time (see GoogleEndpoints.Events).
            List<EventModel> events = await FetchEventsAsync(api);

            var stored = state.CalendarEvents;
            var fingerprints = stored.ToDictionary(kv => kv.Key, kv => kv.Value.Fingerprint);
            CalendarPlan plan = CalendarDiff.Plan(events, fingerprints);

            AppointmentStore store = await CalendarWriter.OpenStoreAsync();
            AppointmentCalendar cal = await CalendarWriter.GetOrCreateCalendarAsync(store);

            foreach (string id in plan.Deletes)
            {
                CalendarEntry entry;
                if (stored.TryGetValue(id, out entry))
                {
                    await CalendarWriter.DeleteAsync(cal, entry.LocalId);
                    stored.Remove(id);
                }
            }
            foreach (EventModel e in plan.Adds)
            {
                try
                {
                    string localId = await CalendarWriter.UpsertAsync(cal, e, null);
                    stored[e.GoogleId] = new CalendarEntry { LocalId = localId, Fingerprint = e.Fingerprint };
                }
                catch (Exception ex) { report.Errors.Add("Evento " + e.Subject + ": " + ex.Message); }
            }
            foreach (EventModel e in plan.Updates)
            {
                try
                {
                    string old = stored[e.GoogleId].LocalId;
                    string localId = await CalendarWriter.UpsertAsync(cal, e, old);
                    stored[e.GoogleId] = new CalendarEntry { LocalId = localId, Fingerprint = e.Fingerprint };
                }
                catch (Exception ex) { report.Errors.Add("Evento " + e.Subject + ": " + ex.Message); }
            }

            state.CalendarCount = stored.Count;
            state.LastCalendarSync = DateTimeOffset.Now;
            report.CalendarText = plan.Adds.Count + " nuovi, " + plan.Updates.Count + " modificati, " + plan.Deletes.Count + " rimossi";
            state.AddLog("Calendario " + report.CalendarText);
        }

        private static async Task<List<EventModel>> FetchEventsAsync(GoogleApi api)
        {
            var events = new List<EventModel>();
            string page = null;
            DateTime now = DateTime.UtcNow;
            do
            {
                JObject json = await api.GetJsonAsync(GoogleEndpoints.Events(now, page));
                var items = json["items"] as JArray;
                if (items != null)
                {
                    foreach (JToken t in items)
                    {
                        EventModel e = CalendarMapper.Map(t as JObject);
                        if (e != null) events.Add(e);
                    }
                }
                page = (string)json["nextPageToken"];
            }
            while (page != null);
            return events;
        }
    }
}
