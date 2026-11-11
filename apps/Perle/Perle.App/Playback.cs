using System;
using System.Threading.Tasks;
using Perle.Extractor;
using Windows.Media.Core;
using Windows.Media.Streaming.Adaptive;
using Windows.Storage.Streams;

namespace Perle.App
{
    /// <summary>Turns a <see cref="StreamInfo"/> into something MediaPlayer can play on the Lumia.</summary>
    public static class Playback
    {
        public const int MaxHeight = 720;

        static Windows.Web.Http.HttpClient CreateClient()
        {
            var client = new Windows.Web.Http.HttpClient();
            client.DefaultRequestHeaders.UserAgent.TryParseAdd(Innertube.VisionOsUserAgent);
            return client;
        }

        public static async Task<MediaSource> CreateSourceAsync(StreamInfo info)
        {
            if (info.HlsUrl != null)
            {
                var http = CreateClient();
                string master = await http.GetStringAsync(new Uri(info.HlsUrl));
                string filtered = HlsFilter.Filter(master, MaxHeight);
                var stream = new InMemoryRandomAccessStream();
                using (var writer = new DataWriter(stream.GetOutputStreamAt(0)))
                {
                    writer.WriteString(filtered);
                    await writer.StoreAsync();
                    await writer.FlushAsync();
                    writer.DetachStream();
                }
                stream.Seek(0);
                var result = await AdaptiveMediaSource.CreateFromStreamAsync(stream, new Uri(info.HlsUrl), "application/vnd.apple.mpegurl", http);
                if (result.Status == AdaptiveMediaSourceCreationStatus.Success)
                {
                    result.MediaSource.InitialBitrate = result.MediaSource.AvailableBitrates.Count > 0
                        ? result.MediaSource.AvailableBitrates[result.MediaSource.AvailableBitrates.Count / 2]
                        : result.MediaSource.InitialBitrate;
                    return MediaSource.CreateFromAdaptiveMediaSource(result.MediaSource);
                }
                System.Diagnostics.Debug.WriteLine("HLS failed: " + result.Status + " " + result.ExtendedError);
            }

            // muxed progressive stream, when YouTube still provides one
            foreach (var f in info.Formats)
                if (f.IsH264 && f.Height <= MaxHeight)
                    return MediaSource.CreateFromUri(new Uri(f.Url));

            throw new ExtractionException("Nessun flusso riproducibile su questo telefono");
        }
    }
}
