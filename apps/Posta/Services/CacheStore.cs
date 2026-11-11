using System;
using System.Threading.Tasks;
using Posta.Core.Cache;
using Windows.Storage;

namespace Posta.Services
{
    /// <summary>Reads and writes the inbox cache file in the app's LocalFolder. Errors are swallowed: cache is optional.</summary>
    public static class CacheStore
    {
        private const string FileName = "inbox-cache.json";

        public static async Task<InboxCacheDocument> LoadAsync()
        {
            try
            {
                var file = await ApplicationData.Current.LocalFolder.GetFileAsync(FileName);
                var text = await FileIO.ReadTextAsync(file);
                return InboxCache.Deserialize(text);
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static async Task SaveAsync(InboxCacheDocument doc)
        {
            try
            {
                var file = await ApplicationData.Current.LocalFolder.CreateFileAsync(FileName, CreationCollisionOption.ReplaceExisting);
                await FileIO.WriteTextAsync(file, InboxCache.Serialize(doc));
            }
            catch (Exception)
            {
                // a failed cache write must never break the inbox
            }
        }

        public static async Task ClearAsync()
        {
            try
            {
                var file = await ApplicationData.Current.LocalFolder.GetFileAsync(FileName);
                await file.DeleteAsync();
            }
            catch (Exception)
            {
                // nothing cached
            }
        }
    }
}
