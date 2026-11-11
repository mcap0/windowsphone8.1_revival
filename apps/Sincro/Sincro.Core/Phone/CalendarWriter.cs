using System;
using System.Threading.Tasks;
using Sincro.Logic;
using Windows.ApplicationModel.Appointments;

namespace Sincro.Core
{
    /// <summary>
    /// Writes Google events into one app-owned appointment calendar named "Google".
    /// Other apps may read it (People/Calendar show it), they may not write it.
    /// </summary>
    public static class CalendarWriter
    {
        public const string CalendarName = "Google";

        public static async Task<AppointmentStore> OpenStoreAsync()
        {
            return await AppointmentManager.RequestStoreAsync(AppointmentStoreAccessType.AppCalendarsReadWrite);
        }

        public static async Task<AppointmentCalendar> GetOrCreateCalendarAsync(AppointmentStore store)
        {
            var calendars = await store.FindAppointmentCalendarsAsync();
            foreach (AppointmentCalendar c in calendars)
            {
                if (c.DisplayName == CalendarName) return c;
            }
            AppointmentCalendar created = await store.CreateAppointmentCalendarAsync(CalendarName);
            created.OtherAppReadAccess = AppointmentCalendarOtherAppReadAccess.Full;
            created.OtherAppWriteAccess = AppointmentCalendarOtherAppWriteAccess.None;
            await created.SaveAsync();
            return created;
        }

        /// <summary>Adds the event, or updates the existing local appointment. Returns the local id.</summary>
        public static async Task<string> UpsertAsync(AppointmentCalendar cal, EventModel e, string localId)
        {
            Appointment appt = null;
            if (!string.IsNullOrEmpty(localId))
            {
                try { appt = await cal.GetAppointmentAsync(localId); }
                catch (Exception) { appt = null; } // deleted by the user: recreate
            }
            if (appt == null) appt = new Appointment();

            appt.RoamingId = e.GoogleId;
            appt.Subject = e.Subject ?? "";
            appt.Location = e.Location ?? "";
            appt.Details = e.Details ?? "";
            appt.AllDay = e.AllDay;
            appt.StartTime = e.Start;
            appt.Duration = e.Duration;
            appt.Reminder = e.Reminder;
            await cal.SaveAppointmentAsync(appt);
            return appt.LocalId;
        }

        public static async Task DeleteAsync(AppointmentCalendar cal, string localId)
        {
            try { await cal.DeleteAppointmentAsync(localId); }
            catch (Exception) { } // already gone
        }
    }
}
