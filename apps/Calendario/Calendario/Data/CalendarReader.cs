using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Calendario.Logic;
using Windows.ApplicationModel.Appointments;

namespace Calendario.Data
{
    /// <summary>
    /// Lettura di tutti gli appuntamenti presenti sul telefono, compresi quelli Google sincronizzati.
    /// </summary>
    public static class CalendarReader
    {
        /// <summary>Carica gli eventi che si sovrappongono all'intervallo [fromLocal, toLocal).</summary>
        public static async Task<List<EventItem>> LoadAsync(DateTime fromLocal, DateTime toLocal)
        {
            AppointmentStore store = await AppointmentManager.RequestStoreAsync(AppointmentStoreAccessType.AllCalendarsReadOnly);

            var calendars = new Dictionary<string, AppointmentCalendar>();
            IReadOnlyList<AppointmentCalendar> list = await store.FindAppointmentCalendarsAsync();
            foreach (AppointmentCalendar calendar in list)
            {
                calendars[calendar.LocalId] = calendar;
            }

            var options = new FindAppointmentsOptions();
            options.MaxCount = 2000;
            options.FetchProperties.Add(AppointmentProperties.Subject);
            options.FetchProperties.Add(AppointmentProperties.Location);
            options.FetchProperties.Add(AppointmentProperties.Details);
            options.FetchProperties.Add(AppointmentProperties.StartTime);
            options.FetchProperties.Add(AppointmentProperties.Duration);
            options.FetchProperties.Add(AppointmentProperties.AllDay);
            options.FetchProperties.Add(AppointmentProperties.Reminder);

            DateTimeOffset start = new DateTimeOffset(DateTime.SpecifyKind(fromLocal, DateTimeKind.Local));
            TimeSpan span = toLocal - fromLocal;
            IReadOnlyList<Appointment> appointments = await store.FindAppointmentsAsync(start, span, options);

            var result = new List<EventItem>(appointments.Count);
            foreach (Appointment appointment in appointments)
            {
                var item = new EventItem();
                item.LocalId = appointment.LocalId;
                item.Title = string.IsNullOrWhiteSpace(appointment.Subject) ? "(senza titolo)" : appointment.Subject;
                item.Location = appointment.Location ?? "";
                item.Details = appointment.Details ?? "";
                item.Reminder = appointment.Reminder;
                item.AllDay = appointment.AllDay;

                if (appointment.AllDay)
                {
                    item.Start = AgendaBuilder.AllDayDate(appointment.StartTime.LocalDateTime, appointment.StartTime.UtcDateTime);
                    item.Duration = AgendaBuilder.AllDayDuration(appointment.Duration);
                }
                else
                {
                    item.Start = appointment.StartTime.LocalDateTime;
                    item.Duration = appointment.Duration;
                }

                AppointmentCalendar calendar;
                if (calendars.TryGetValue(appointment.CalendarId, out calendar))
                {
                    item.CalendarId = calendar.LocalId;
                    item.CalendarName = calendar.DisplayName;
                    item.IsEditable = string.Equals(calendar.DisplayName, CalendarWriter.CalendarName, StringComparison.OrdinalIgnoreCase);

                    Windows.UI.Color? color = calendar.DisplayColor;
                    if (color.HasValue)
                    {
                        Windows.UI.Color value = color.Value;
                        item.CalendarArgb = ((uint)value.A << 24) | ((uint)value.R << 16) | ((uint)value.G << 8) | value.B;
                    }
                }

                result.Add(item);
            }

            result.Sort(AgendaBuilder.Compare);
            return result;
        }
    }
}
