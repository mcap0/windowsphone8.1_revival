using System;
using System.Threading.Tasks;
using Mappe.Core;
using Windows.Graphics.Imaging;
using Windows.Storage;
using Windows.Storage.Streams;
using Windows.UI.Xaml.Controls.Maps;
using Windows.Web.Http;
using System.Runtime.InteropServices.WindowsRuntime;

namespace Mappe
{
    /// <summary>
    /// OSM raster tiles through CustomMapTileDataSource (the 8.1 MapControl has no way to set
    /// request headers on HttpMapTileDataSource, so the User-Agent is set here).
    /// Tiles are fetched only when the map asks for them (viewed tiles only), cached in memory (LRU)
    /// and on disk in LocalFolder/tiles.
    /// </summary>
    public sealed class TileService
    {
        private const int MemoryTiles = 200;
        private readonly LruCache<string, byte[]> _memory = new LruCache<string, byte[]>(MemoryTiles);
        private readonly object _statsLock = new object();
        private Task<StorageFolder> _tilesFolder;

        public const string ModeHttp = "Http";
        public const string ModeCustom = "Custom";

        public readonly string Mode;
        public readonly MapTileDataSource DataSource;

        public int Requested;
        public int Ok;
        public int Failed;
        public int MemoryHits;
        public int DiskHits;
        public int NetworkFetches;
        public string LastError = "-";
        public string LastErrorTile = "-";

        /// <summary>
        /// ModeHttp: HttpMapTileDataSource with the User-Agent in AdditionalRequestHeaders (exists on 8.1).
        /// ModeCustom: CustomMapTileDataSource, downloads here with caches (fallback for diagnosis).
        /// </summary>
        public TileService(string mode)
        {
            Mode = mode == ModeCustom ? ModeCustom : ModeHttp;
            if (Mode == ModeCustom)
            {
                var custom = new CustomMapTileDataSource();
                custom.BitmapRequested += OnBitmapRequested;
                DataSource = custom;
            }
            else
            {
                var http = new HttpMapTileDataSource(TileMath.OsmTemplate);
                try
                {
                    http.AdditionalRequestHeaders["User-Agent"] = GeoUrls.UserAgent;
                }
                catch (Exception ex)
                {
                    // Some builds refuse restricted headers here; keep the source and report it in Diagnostica.
                    LastError = "User-Agent non impostabile: " + Describe(ex);
                }
                DataSource = http;
            }
        }

        public string PathName
        {
            get
            {
                return Mode == ModeCustom
                    ? "Custom (CustomMapTileDataSource, download e cache dell'app)"
                    : "Http (HttpMapTileDataSource, User-Agent in AdditionalRequestHeaders)";
            }
        }

        public void Bump(ref int counter)
        {
            lock (_statsLock) { counter++; }
        }

        public string Stats()
        {
            if (Mode == ModeHttp)
                return "non misurabile: il MapControl scarica i tile direttamente (vedi LoadingStatus)";
            lock (_statsLock)
            {
                return "richieste " + Requested + ", ok " + Ok + ", errori " + Failed +
                       ", da memoria " + MemoryHits + ", da disco " + DiskHits + ", scaricati " + NetworkFetches;
            }
        }

        private async void OnBitmapRequested(CustomMapTileDataSource sender, MapTileBitmapRequestedEventArgs args)
        {
            // The deferral must be taken before the first await.
            MapTileBitmapRequestDeferral deferral = args.Request.GetDeferral();
            int z = args.ZoomLevel, x = args.X, y = args.Y;
            string tile = z + "/" + x + "/" + y;
            lock (_statsLock) { Requested++; }
            try
            {
                if (!TileMath.IsValid(z, x, y)) throw new ArgumentOutOfRangeException("tile " + tile);

                byte[] raw = await GetRawTileAsync(z, x, y);
                byte[] png = await NormalizeToPngAsync(raw);

                var stream = new InMemoryRandomAccessStream();
                await stream.WriteAsync(png.AsBuffer());
                await stream.FlushAsync();
                stream.Seek(0);
                args.Request.PixelData = RandomAccessStreamReference.CreateFromStream(stream);
                lock (_statsLock) { Ok++; }
            }
            catch (Exception ex)
            {
                lock (_statsLock)
                {
                    Failed++;
                    LastError = Describe(ex);
                    LastErrorTile = tile;
                }
            }
            finally
            {
                deferral.Complete();
            }
        }

        private async Task<byte[]> GetRawTileAsync(int z, int x, int y)
        {
            string key = TileMath.Key(z, x, y);
            byte[] bytes;
            if (_memory.TryGet(key, out bytes))
            {
                Bump(ref MemoryHits);
                return bytes;
            }

            bytes = await ReadDiskAsync(key);
            if (bytes != null)
            {
                Bump(ref DiskHits);
                _memory.Put(key, bytes);
                return bytes;
            }

            bytes = await DownloadAsync(TileMath.Build(TileMath.OsmTemplate, z, x, y));
            Bump(ref NetworkFetches);
            _memory.Put(key, bytes);
            try
            {
                await WriteDiskAsync(key, bytes);
            }
            catch (Exception ex)
            {
                lock (_statsLock) { LastError = "cache disco: " + Describe(ex); }
            }
            return bytes;
        }

        private static async Task<byte[]> DownloadAsync(string url)
        {
            var client = new HttpClient();
            try
            {
                var request = new HttpRequestMessage(HttpMethod.Get, new Uri(url));
                request.Headers.TryAppendWithoutValidation("User-Agent", GeoUrls.UserAgent);
                HttpResponseMessage response = await client.SendRequestAsync(request);
                if (!response.IsSuccessStatusCode)
                    throw new InvalidOperationException("HTTP " + (int)response.StatusCode + " " + response.ReasonPhrase);
                IBuffer buffer = await response.Content.ReadAsBufferAsync();
                return buffer.ToArray();
            }
            finally
            {
                client.Dispose();
            }
        }

        private Task<StorageFolder> TilesFolder()
        {
            if (_tilesFolder == null)
                _tilesFolder = ApplicationData.Current.LocalFolder.CreateFolderAsync("tiles", CreationCollisionOption.OpenIfExists).AsTask();
            return _tilesFolder;
        }

        private async Task<byte[]> ReadDiskAsync(string key)
        {
            StorageFolder folder = await TilesFolder();
            StorageFile file;
            try
            {
                file = await folder.GetFileAsync(key + ".png");
            }
            catch (Exception)
            {
                return null; // not cached yet
            }
            IBuffer buffer = await FileIO.ReadBufferAsync(file);
            return buffer.ToArray();
        }

        private async Task WriteDiskAsync(string key, byte[] bytes)
        {
            StorageFolder folder = await TilesFolder();
            StorageFile file = await folder.CreateFileAsync(key + ".png", CreationCollisionOption.ReplaceExisting);
            await FileIO.WriteBytesAsync(file, bytes);
        }

        /// <summary>Decodes the downloaded image to pixels and re-encodes it as PNG for the map control.</summary>
        private static async Task<byte[]> NormalizeToPngAsync(byte[] raw)
        {
            using (var input = new InMemoryRandomAccessStream())
            {
                await input.WriteAsync(raw.AsBuffer());
                await input.FlushAsync();
                input.Seek(0);

                BitmapDecoder decoder = await BitmapDecoder.CreateAsync(input);
                PixelDataProvider data = await decoder.GetPixelDataAsync(
                    BitmapPixelFormat.Bgra8,
                    BitmapAlphaMode.Premultiplied,
                    new BitmapTransform(),
                    ExifOrientationMode.IgnoreExifOrientation,
                    ColorManagementMode.DoNotColorManage);
                byte[] pixels = data.DetachPixelData();

                using (var output = new InMemoryRandomAccessStream())
                {
                    BitmapEncoder encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, output);
                    encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied,
                        decoder.PixelWidth, decoder.PixelHeight, decoder.DpiX, decoder.DpiY, pixels);
                    await encoder.FlushAsync();
                    return await ReadAllAsync(output);
                }
            }
        }

        private static async Task<byte[]> ReadAllAsync(IRandomAccessStream stream)
        {
            stream.Seek(0);
            uint size = (uint)stream.Size;
            using (var reader = new DataReader(stream.GetInputStreamAt(0)))
            {
                await reader.LoadAsync(size);
                var bytes = new byte[size];
                reader.ReadBytes(bytes);
                return bytes;
            }
        }

        public static string Describe(Exception ex)
        {
            string msg = ex.GetType().Name + ": " + ex.Message;
            return msg.Length > 300 ? msg.Substring(0, 300) : msg;
        }
    }
}
