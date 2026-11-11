using System;
using Sincro.Core;
using Windows.ApplicationModel.Background;

namespace Sincro.Background
{
    /// <summary>
    /// Timer entry point (TimeTrigger 30 min). Only public type of this WinRT component.
    /// Contacts: incremental. Calendar: only if the last calendar run is older than 2 h (see SyncEngine).
    /// </summary>
    public sealed class SincroTask : IBackgroundTask
    {
        public async void Run(IBackgroundTaskInstance taskInstance)
        {
            BackgroundTaskDeferral deferral = taskInstance.GetDeferral();
            SyncState state = null;
            DateTimeOffset started = DateTimeOffset.UtcNow;
            bool countIncremented = false;
            string result = "error: background task failed";

            try
            {
                state = await SyncStateStore.LoadAsync();
                state.BackgroundRunCount++;
                countIncremented = true;
                state.LastBackgroundStart = started;
                state.LastBackgroundEnd = null;
                state.LastBackgroundResult = "in corso";

                // Persist the start as well, so an interrupted run remains visible.
                try { await SyncStateStore.SaveAsync(state); }
                catch (Exception) { }

                bool contactsOn = AppSettings.ContactsOn;
                bool calendarOn = AppSettings.CalendarOn;

                if (!contactsOn && !calendarOn)
                {
                    result = "skipped: settings off";
                }
                else if (!contactsOn && calendarOn && IsCalendarFresh(state, started))
                {
                    result = "skipped: calendar fresh";
                }
                else
                {
                    await SyncEngine.RunAsync(contactsOn, calendarOn, true);
                    result = "ok";
                }
            }
            catch (Exception ex)
            {
                result = "error: " + ex.GetType().Name + ": " + ex.Message;
            }
            finally
            {
                try
                {
                    if (state == null)
                    {
                        state = await SyncStateStore.LoadAsync();
                    }

                    if (!countIncremented)
                    {
                        state.BackgroundRunCount++;
                    }

                    state.LastBackgroundStart = started;
                    state.LastBackgroundEnd = DateTimeOffset.UtcNow;
                    state.LastBackgroundResult = result;
                    await SyncStateStore.SaveAsync(state);
                }
                catch (Exception)
                {
                    // Never let a persistence failure crash the background host.
                }
                finally
                {
                    deferral.Complete();
                }
            }
        }

        private static bool IsCalendarFresh(SyncState state, DateTimeOffset now)
        {
            return state.LastCalendarSync.HasValue
                && now - state.LastCalendarSync.Value < TimeSpan.FromHours(2);
        }
    }
}
