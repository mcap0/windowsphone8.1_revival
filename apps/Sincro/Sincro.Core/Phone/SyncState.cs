using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Windows.Storage;

namespace Sincro.Core
{
    public sealed class CalendarEntry
    {
        public string LocalId;
        public string Fingerprint;
    }

    /// <summary>Everything the sync remembers between runs, kept as JSON in the app's local folder.</summary>
    public sealed class SyncState
    {
        public string ContactsSyncToken;
        public List<string> KnownContacts = new List<string>();
        public Dictionary<string, CalendarEntry> CalendarEvents = new Dictionary<string, CalendarEntry>();
        public DateTimeOffset? LastContactsSync;
        public DateTimeOffset? LastCalendarSync;
        public int ContactsCount;
        public int CalendarCount;
        public DateTimeOffset? LastBackgroundStart;
        public DateTimeOffset? LastBackgroundEnd;
        public string LastBackgroundResult;
        public int BackgroundRunCount;
        public List<string> Log = new List<string>();

        public void AddLog(string text)
        {
            Log.Add(DateTime.Now.ToString("dd/MM HH:mm") + "  " + text);
            while (Log.Count > 30) Log.RemoveAt(0);
        }
    }

    public static class SyncStateStore
    {
        private const string FileName = "sincro-state.json";

        public static async Task<SyncState> LoadAsync()
        {
            try
            {
                StorageFile file = await ApplicationData.Current.LocalFolder.GetFileAsync(FileName);
                string text = await FileIO.ReadTextAsync(file);
                return JsonConvert.DeserializeObject<SyncState>(text) ?? new SyncState();
            }
            catch (Exception)
            {
                return new SyncState();
            }
        }

        public static async Task SaveAsync(SyncState state)
        {
            StorageFile file = await ApplicationData.Current.LocalFolder.CreateFileAsync(FileName, CreationCollisionOption.ReplaceExisting);
            await FileIO.WriteTextAsync(file, JsonConvert.SerializeObject(state));
        }
    }

    /// <summary>On/off switches for the two areas (stored in LocalSettings, read by UI and background task).</summary>
    public static class AppSettings
    {
        public static bool ContactsOn
        {
            get { return Read("contacts_on"); }
            set { Write("contacts_on", value); }
        }

        public static bool CalendarOn
        {
            get { return Read("calendar_on"); }
            set { Write("calendar_on", value); }
        }

        private static bool Read(string key)
        {
            try
            {
                object v;
                if (ApplicationData.Current.LocalSettings.Values.TryGetValue(key, out v) && v is bool) return (bool)v;
            }
            catch (Exception) { }
            return true;
        }

        private static void Write(string key, bool value)
        {
            try { ApplicationData.Current.LocalSettings.Values[key] = value; }
            catch (Exception) { }
        }
    }
}
