/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * IStorage on the app's local folder (StorageFile / ApplicationData.LocalFolder), Windows Phone 8.1 only.
 * TryGetItemAsync is Windows 10 only, so missing files are detected through GetFileAsync.
 */

using System;
using System.IO;
using System.Threading.Tasks;
using Windows.Storage;

namespace Perle
{
    public sealed class WinRtStorage : IStorage
    {
        static StorageFolder Folder => ApplicationData.Current.LocalFolder;

        static async Task<StorageFile> FindAsync(string name)
        {
            try { return await Folder.GetFileAsync(name); }
            catch (FileNotFoundException) { return null; }
        }

        public async Task<string> ReadTextAsync(string name)
        {
            var file = await FindAsync(name);
            return file == null ? null : await FileIO.ReadTextAsync(file);
        }

        public async Task WriteTextAsync(string name, string text)
        {
            var file = await Folder.CreateFileAsync(name, CreationCollisionOption.ReplaceExisting);
            await FileIO.WriteTextAsync(file, text);
        }

        public async Task DeleteAsync(string name)
        {
            var file = await FindAsync(name);
            if (file != null) await file.DeleteAsync();
        }
    }
}
