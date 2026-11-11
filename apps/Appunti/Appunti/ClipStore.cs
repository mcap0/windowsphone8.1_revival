using System;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using Windows.Data.Json;
using Windows.Storage;

namespace Appunti
{
    public sealed class ClipItem
    {
        public ClipItem(string text)
        {
            Text = text;
        }

        public string Text { get; private set; }
    }

    // Local history: the last 20 texts, newest first, kept in LocalFolder/history.json.
    public static class ClipStore
    {
        public const int MaxItems = 20;
        private const string FileName = "history.json";

        public static readonly ObservableCollection<ClipItem> Items = new ObservableCollection<ClipItem>();
        private static bool _loaded;

        public static async Task LoadAsync()
        {
            if (_loaded) return;
            _loaded = true;
            try
            {
                StorageFile file = await ApplicationData.Current.LocalFolder.GetFileAsync(FileName);
                string json = await FileIO.ReadTextAsync(file);
                JsonArray array = JsonArray.Parse(json);
                foreach (IJsonValue value in array)
                {
                    string text = value.GetString();
                    if (!string.IsNullOrEmpty(text)) Items.Add(new ClipItem(text));
                }
            }
            catch (Exception)
            {
                // No history yet, or unreadable file: start empty.
            }
        }

        // Adds a text at the top unless it equals the newest entry; caps the list at MaxItems.
        public static async Task AddAsync(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return;
            if (Items.Count > 0 && Items[0].Text == text) return;

            Items.Insert(0, new ClipItem(text));
            while (Items.Count > MaxItems) Items.RemoveAt(Items.Count - 1);
            await SaveAsync();
        }

        private static async Task SaveAsync()
        {
            var array = new JsonArray();
            foreach (ClipItem item in Items)
            {
                array.Add(JsonValue.CreateStringValue(item.Text));
            }
            StorageFile file = await ApplicationData.Current.LocalFolder.CreateFileAsync(FileName, CreationCollisionOption.ReplaceExisting);
            await FileIO.WriteTextAsync(file, array.Stringify());
        }
    }
}
