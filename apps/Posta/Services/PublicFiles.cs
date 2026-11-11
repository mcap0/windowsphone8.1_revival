using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Windows.Storage;

namespace Posta.Services
{
    /// <summary>
    /// Win32 file access under C:\Data\Users\Public (the rooted phone lets apps read and write it; the WP8.1
    /// pickers only show Photos and have no usable save dialog).
    /// </summary>
    public static class PublicFiles
    {
        public const string Root = @"C:\Data\Users\Public";
        public const string DownloadDir = Root + @"\Downloads\Posta";

        public sealed class Entry
        {
            public string Name { get; set; }
            public bool IsDir { get; set; }
            public long Size { get; set; }
        }

        public static List<Entry> List(string dir)
        {
            var result = new List<Entry>();
            WIN32_FIND_DATA fd;
            var h = FindFirstFileExW(dir + @"\*", 1 /* FindExInfoBasic */, out fd, 0, IntPtr.Zero, 0);
            if (h == InvalidHandle)
                return result;
            try
            {
                do
                {
                    if (fd.cFileName == "." || fd.cFileName == ".." || (fd.dwFileAttributes & FileAttributeHidden) != 0)
                        continue;
                    result.Add(new Entry
                    {
                        Name = fd.cFileName,
                        IsDir = (fd.dwFileAttributes & FileAttributeDirectory) != 0,
                        Size = ((long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow
                    });
                } while (FindNextFileW(h, out fd));
            }
            finally
            {
                FindClose(h);
            }
            result.Sort((a, b) => a.IsDir != b.IsDir ? (a.IsDir ? -1 : 1)
                : string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase));
            return result;
        }

        public static byte[] Read(string path)
        {
            var p = new CREATEFILE2_EXTENDED_PARAMETERS { dwSize = (uint)Marshal.SizeOf(typeof(CREATEFILE2_EXTENDED_PARAMETERS)) };
            var h = CreateFile2(path, GenericRead, FileShareRead, OpenExisting, ref p);
            if (h == InvalidHandle)
                throw new InvalidOperationException("Impossibile aprire il file (" + Marshal.GetLastWin32Error() + ").");
            try
            {
                var chunks = new List<byte[]>();
                long total = 0;
                var buffer = new byte[65536];
                uint n;
                while (ReadFile(h, buffer, (uint)buffer.Length, out n, IntPtr.Zero) && n > 0)
                {
                    var chunk = new byte[n];
                    Array.Copy(buffer, chunk, (int)n);
                    chunks.Add(chunk);
                    total += n;
                }
                var data = new byte[(int)total];
                var offset = 0;
                foreach (var chunk in chunks)
                {
                    Array.Copy(chunk, 0, data, offset, chunk.Length);
                    offset += chunk.Length;
                }
                return data;
            }
            finally
            {
                CloseHandle(h);
            }
        }

        /// <summary>Writes into Downloads\Posta with a free name ("a (1).pdf"); returns the full path.</summary>
        public static string SaveDownload(string name, byte[] data)
        {
            CreateDirectoryW(Root + @"\Downloads", IntPtr.Zero);
            CreateDirectoryW(DownloadDir, IntPtr.Zero);
            var clean = SafeName(name);
            var dot = clean.LastIndexOf('.');
            var stem = dot > 0 ? clean.Substring(0, dot) : clean;
            var ext = dot > 0 ? clean.Substring(dot) : "";
            var path = DownloadDir + @"\" + clean;
            for (var n = 1; Exists(path) && n < 1000; n++)
                path = DownloadDir + @"\" + stem + " (" + n + ")" + ext;

            var p = new CREATEFILE2_EXTENDED_PARAMETERS
            {
                dwSize = (uint)Marshal.SizeOf(typeof(CREATEFILE2_EXTENDED_PARAMETERS)),
                dwFileAttributes = FileAttributeNormal
            };
            var h = CreateFile2(path, GenericWrite, 0, CreateNew, ref p);
            if (h == InvalidHandle)
                throw new InvalidOperationException("Salvataggio non riuscito (" + Marshal.GetLastWin32Error() + ").");
            uint written;
            bool ok;
            try
            {
                ok = WriteFile(h, data, (uint)data.Length, out written, IntPtr.Zero) && written == data.Length;
            }
            finally
            {
                CloseHandle(h);
            }
            if (!ok)
            {
                DeleteFileW(path);
                throw new InvalidOperationException("Salvataggio non riuscito.");
            }
            return path;
        }

        /// <summary>Opens the bytes with the app registered for the extension (via a copy in LocalFolder).</summary>
        public static async Task<bool> OpenAsync(string name, byte[] data)
        {
            var folder = await ApplicationData.Current.LocalFolder.CreateFolderAsync("Open", CreationCollisionOption.OpenIfExists);
            var file = await folder.CreateFileAsync(SafeName(name), CreationCollisionOption.ReplaceExisting);
            await FileIO.WriteBytesAsync(file, data);
            return await Windows.System.Launcher.LaunchFileAsync(file);
        }

        public static string SafeName(string name)
        {
            var chars = (name ?? "").ToCharArray();
            for (var i = 0; i < chars.Length; i++)
                if ("\\/:*?\"<>|".IndexOf(chars[i]) >= 0 || chars[i] < 32)
                    chars[i] = '_';
            var clean = new string(chars).Trim();
            return clean.Length == 0 ? "allegato" : clean;
        }

        private static bool Exists(string path)
        {
            WIN32_FILE_ATTRIBUTE_DATA attr;
            return GetFileAttributesExW(path, 0, out attr);
        }

        private static readonly IntPtr InvalidHandle = new IntPtr(-1);
        private const uint FileAttributeHidden = 0x2, FileAttributeDirectory = 0x10, FileAttributeNormal = 0x80;
        private const uint GenericRead = 0x80000000, GenericWrite = 0x40000000, FileShareRead = 1;
        private const uint CreateNew = 1, OpenExisting = 3;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode, Pack = 4)]
        private struct WIN32_FIND_DATA
        {
            public uint dwFileAttributes;
            public long ftCreationTime, ftLastAccessTime, ftLastWriteTime;
            public uint nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string cFileName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 14)] public string cAlternateFileName;
        }

        // FILETIMEs are two DWORDs (4-byte aligned): a C# long would add padding on ARM and shift the name
        [StructLayout(LayoutKind.Sequential, Pack = 4)]
        private struct WIN32_FILE_ATTRIBUTE_DATA
        {
            public uint dwFileAttributes;
            public long ftCreationTime, ftLastAccessTime, ftLastWriteTime;
            public uint nFileSizeHigh, nFileSizeLow;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct CREATEFILE2_EXTENDED_PARAMETERS
        {
            public uint dwSize, dwFileAttributes, dwFileFlags, dwSecurityQosFlags;
            public IntPtr lpSecurityAttributes, hTemplateFile;
        }

        [DllImport("kernelbase.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr FindFirstFileExW(string name, int infoLevel, out WIN32_FIND_DATA data, int searchOp, IntPtr filter, uint flags);
        [DllImport("kernelbase.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool FindNextFileW(IntPtr h, out WIN32_FIND_DATA data);
        [DllImport("kernelbase.dll")]
        private static extern bool FindClose(IntPtr h);
        [DllImport("kernelbase.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateFile2(string name, uint access, uint share, uint disposition, ref CREATEFILE2_EXTENDED_PARAMETERS p);
        [DllImport("kernelbase.dll", SetLastError = true)]
        private static extern bool ReadFile(IntPtr h, byte[] buffer, uint count, out uint read, IntPtr overlapped);
        [DllImport("kernelbase.dll", SetLastError = true)]
        private static extern bool WriteFile(IntPtr h, byte[] buffer, uint count, out uint written, IntPtr overlapped);
        [DllImport("kernelbase.dll")]
        private static extern bool CloseHandle(IntPtr h);
        [DllImport("kernelbase.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateDirectoryW(string path, IntPtr security);
        [DllImport("kernelbase.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool DeleteFileW(string path);
        [DllImport("kernelbase.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool GetFileAttributesExW(string path, int level, out WIN32_FILE_ATTRIBUTE_DATA data);
    }
}
