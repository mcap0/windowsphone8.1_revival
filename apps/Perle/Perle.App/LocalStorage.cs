using System;
using System.Threading.Tasks;
using Windows.Storage;

namespace Perle.App
{
    /// <summary>Perle's files in ApplicationData.LocalFolder.</summary>
    public sealed class LocalStorage : IStorage
    {
        static StorageFolder Folder => ApplicationData.Current.LocalFolder;

        public async Task<string> ReadTextAsync(string name)
        {
            var item = await Folder.TryGetItemAsync(name);
            return item is StorageFile f ? await FileIO.ReadTextAsync(f) : null;
        }

        public async Task WriteTextAsync(string name, string text)
        {
            // write to a temporary file first, so that a crash never leaves a truncated file
            var tmp = await Folder.CreateFileAsync(name + ".tmp", CreationCollisionOption.ReplaceExisting);
            await FileIO.WriteTextAsync(tmp, text);
            await tmp.RenameAsync(name, NameCollisionOption.ReplaceExisting);
        }

        public async Task DeleteAsync(string name)
        {
            var item = await Folder.TryGetItemAsync(name);
            if (item != null) await item.DeleteAsync();
        }
    }
}
