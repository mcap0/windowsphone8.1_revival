using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Calendario.Logic;
using Windows.ApplicationModel.Appointments;

namespace Calendario.Data
{
    /// <summary>Scrittura degli appuntamenti nel calendario privato dell'app.</summary>
    public static class CalendarWriter
    {
        public const string CalendarName = "Telefono";

        public static async Task<EventItem> SaveAsync(
            EventItem existing,
            string title,
            DateTime start,
            TimeSpan duration,
            bool allDay,
            string location,
            string details,
            TimeSpan? reminder)
        {
            AppointmentStore store = await AppointmentManager.RequestStoreAsync(AppointmentStoreAccessType.AppCalendarsReadWrite);
            IReadOnlyList<AppointmentCalendar> calendars = await store.FindAppointmentCalendarsAsync();
            AppointmentCalendar calendar = FindCalendar(calendars, existing == null ? null : existing.CalendarId);

            if (calendar == null)
            {
                calendar = await store.CreateAppointmentCalendarAsync(CalendarName);
            }

            Appointment appointment = null;
            if (existing != null && !string.IsNullOrEmpty(existing.LocalId))
            {
                try
                {
                    appointment = await calendar.GetAppointmentAsync(existing.LocalId);
                }
                catch (Exception)
                {
                    appointment = null;
                }
            }

            if (appointment == null)
            {
                appointment = new Appointment();
            }

            appointment.Subject = title;
            appointment.StartTime = new DateTimeOffset(DateTime.SpecifyKind(start, DateTimeKind.Local));
            appointment.Duration = duration;
            appointment.AllDay = allDay;
            appointment.Location = location ?? "";
            appointment.Details = details ?? "";
            appointment.Reminder = reminder;

            await calendar.SaveAppointmentAsync(appointment);
            string localId = appointment.LocalId;

            return new EventItem
            {
                LocalId = localId,
                CalendarId = calendar.LocalId,
                CalendarName = calendar.DisplayName,
                IsEditable = true,
                Title = title,
                Start = start,
                Duration = duration,
                AllDay = allDay,
                Location = location ?? "",
                Details = details ?? "",
                Reminder = reminder
            };
        }

        /// <summary>
        /// Elimina l'evento. Lo cerca in tutti i calendari dell'app; se appartiene al calendario di
        /// un'altra app (es. il "Telefono" dell'Assistente) l'ID non vale qui (0x800710DD) e si usa
        /// la finestra di sistema. Restituisce false se l'utente annulla.
        /// </summary>
        public static async Task<bool> DeleteAsync(EventItem item)
        {
            if (item == null || string.IsNullOrEmpty(item.LocalId))
            {
                throw new InvalidOperationException("L'evento non \u00e8 disponibile.");
            }

            AppointmentStore store = await AppointmentManager.RequestStoreAsync(AppointmentStoreAccessType.AppCalendarsReadWrite);
            IReadOnlyList<AppointmentCalendar> calendars = await store.FindAppointmentCalendarsAsync();
            foreach (AppointmentCalendar calendar in calendars)
            {
                Appointment found = null;
                try { found = await calendar.GetAppointmentAsync(item.LocalId); }
                catch (Exception) { found = null; }
                if (found != null)
                {
                    await calendar.DeleteAppointmentAsync(found.LocalId);
                    return true;
                }
            }

            return await AppointmentManager.ShowRemoveAppointmentAsync(item.LocalId, new Windows.Foundation.Rect(0, 0, 1, 1));
        }

        private static AppointmentCalendar FindCalendar(IReadOnlyList<AppointmentCalendar> calendars, string localId)
        {
            if (calendars == null) return null;

            if (!string.IsNullOrEmpty(localId))
            {
                foreach (AppointmentCalendar calendar in calendars)
                {
                    if (string.Equals(calendar.LocalId, localId, StringComparison.Ordinal))
                    {
                        return calendar;
                    }
                }
            }

            foreach (AppointmentCalendar calendar in calendars)
            {
                if (string.Equals(calendar.DisplayName, CalendarName, StringComparison.OrdinalIgnoreCase))
                {
                    return calendar;
                }
            }

            return null;
        }
    }
}
