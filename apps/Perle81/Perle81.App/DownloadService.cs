/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * "Scarica audio": the AAC audio stream (itag 140) of a video into Music\Perle\<Artista>\<Titolo>.m4a, with the
 * MP4 tags written by the native component. One download at a time, three attempts per item.
 * The queue lives in memory: an app that is closed loses the waiting items.
 */

using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Threading;
using System.Threading.Tasks;
using Perle.Extractor;
using Perle.Progressive;
using Perle81.Native;
using Windows.Networking.BackgroundTransfer;
using Windows.Storage;

namespace Perle81.App
{
    public enum DownloadState { Waiting, Running, Done, Failed }

    public sealed class DownloadItem : INotifyPropertyChanged
    {
        public const int MaxAttempts = 3;

        public string Url { get; set; }
        public string VideoId { get; set; }
        string title, uploader, message;
        double progress;
        DownloadState state;
        int attempts;

        public event PropertyChangedEventHandler PropertyChanged;
        void Raise(string name) { var h = PropertyChanged; if (h != null) h(this, new PropertyChangedEventArgs(name)); }

        public string Title { get { return title; } set { title = value; Raise("Title"); } }
        public string Uploader { get { return uploader; } set { uploader = value; Raise("Uploader"); } }
        public string Message { get { return message; } set { message = value; Raise("Message"); } }
        public double Progress { get { return progress; } set { progress = value; Raise("Progress"); } }
        public DownloadState State { get { return state; } set { state = value; Raise("State"); } }
        public int Attempts { get { return attempts; } set { attempts = value; Raise("Attempts"); } }
    }

    public static class DownloadService
    {
        public static readonly ObservableCollection<DownloadItem> Items = new ObservableCollection<DownloadItem>();
        static readonly SemaphoreSlim gate = new SemaphoreSlim(1);

        public static DownloadItem Enqueue(string url, string title, string uploader)
        {
            var item = new DownloadItem
            {
                Url = url,
                VideoId = StreamUrls.VideoIdFromUrl(url),
                Title = title ?? url,
                Uploader = uploader ?? "",
                State = DownloadState.Waiting,
                Message = "In coda"
            };
            Items.Insert(0, item);
            var ignored = RunAsync(item);
            return item;
        }

        public static void Retry(DownloadItem item)
        {
            if (item.State != DownloadState.Failed) return;
            item.Attempts = 0;
            item.State = DownloadState.Waiting;
            item.Message = "In coda";
            var ignored = RunAsync(item);
        }

        static async Task RunAsync(DownloadItem item)
        {
            await gate.WaitAsync();
            try
            {
                while (true)
                {
                    item.Attempts++;
                    item.State = DownloadState.Running;
                    item.Progress = 0;
                    item.Message = "Analisi del video…";
                    try
                    {
                        string saved = await DownloadOnceAsync(item);
                        item.State = DownloadState.Done;
                        item.Progress = 100;
                        item.Message = "Salvato in Musica\\Perle\\" + saved;
                        return;
                    }
                    catch (Exception ex)
                    {
                        if (item.Attempts >= DownloadItem.MaxAttempts)
                        {
                            item.State = DownloadState.Failed;
                            item.Message = "Errore: " + Describe(ex) + " (tentativi esauriti, usa Riprova)";
                            return;
                        }
                        item.Message = "Errore: " + Describe(ex) + ". Nuovo tentativo…";
                        await Task.Delay(TimeSpan.FromSeconds(10 * item.Attempts));
                    }
                }
            }
            finally { gate.Release(); }
        }

        /// <summary>Message, type and the first frame of the stack: enough to find the failing step from a photo.</summary>
        static string Describe(Exception ex)
        {
            string at = "";
            if (ex.StackTrace != null)
            {
                string first = ex.StackTrace.Split('\n').FirstOrDefault(l => l.Contains("Perle")) ?? ex.StackTrace.Split('\n')[0];
                at = " @ " + first.Trim();
            }
            return ex.GetType().Name + ": " + ex.Message + at;
        }

        /// <returns>the path of the file inside Music\Perle</returns>
        static async Task<string> DownloadOnceAsync(DownloadItem item)
        {
            var info = await YoutubeStream.GetInfoAsync(item.Url);
            var audio = info.AdaptiveFormats.FirstOrDefault(f => f.Itag == 140) ?? YoutubeStream.BestAudio(info);
            if (audio == null || audio.Url == null) throw new ExtractionException("nessun flusso audio AAC per questo video");

            string channel = info.UploaderName ?? "";
            string name = info.Name ?? item.VideoId;
            item.Title = name;
            item.Uploader = channel;
            string artist = TitleTools.ArtistOf(channel, name);
            string track = TitleTools.TrackOf(channel, name);

            string year = null;
            try { year = await VideoDate.GetYearAsync(item.VideoId); } catch (Exception) { }
            byte[] cover = null;
            try { cover = await Innertube.Http.GetByteArrayAsync(Thumbs.MediumOf(item.VideoId)); } catch (Exception) { }

            var local = ApplicationData.Current.LocalFolder;
            var dir = await local.CreateFolderAsync("dl", CreationCollisionOption.OpenIfExists);
            var raw = await dir.CreateFileAsync(item.VideoId + ".m4a", CreationCollisionOption.ReplaceExisting);

            item.Message = "Download…";
            await DownloadAsync(audio.Url, raw, item);

            var tagged = await dir.CreateFileAsync(item.VideoId + ".tagged.m4a", CreationCollisionOption.ReplaceExisting);
            item.Message = "Tag MP4…";
            var tags = new Mp4TagSet { Title = track ?? name, Artist = artist ?? channel, Album = "Perle", Year = year ?? "", Cover = cover ?? new byte[0] };
            string rawPath = raw.Path, taggedPath = tagged.Path;
            string error = await Task.Run(() => Mp4Tagger.Write(rawPath, taggedPath, tags));
            if (!string.IsNullOrEmpty(error)) throw new IOException("tag: " + error);

            var music = KnownFolders.MusicLibrary;
            var perle = await music.CreateFolderAsync("Perle", CreationCollisionOption.OpenIfExists);
            var artistFolder = await perle.CreateFolderAsync(TitleTools.SafeName(artist), CreationCollisionOption.OpenIfExists);
            string fileName = TitleTools.SafeName(track) + ".m4a";
            await tagged.MoveAsync(artistFolder, fileName, NameCollisionOption.ReplaceExisting);
            await raw.DeleteAsync();
            return TitleTools.SafeName(artist) + "\\" + fileName;
        }

        /// <summary>The background downloader first (it keeps going with the app closed); HTTP with Range if it refuses.</summary>
        static async Task DownloadAsync(string url, StorageFile file, DownloadItem item)
        {
            try
            {
                var downloader = new BackgroundDownloader();
                var download = downloader.CreateDownload(new Uri(url), file);
                await download.StartAsync().AsTask(new Progress<DownloadOperation>(d =>
                {
                    var p = d.Progress;
                    if (p.TotalBytesToReceive > 0) item.Progress = 100.0 * p.BytesReceived / p.TotalBytesToReceive;
                }));
                return;
            }
            catch (Exception)
            {
                // refused or not available for this URL: the app downloads it itself
            }
            await DownloadWithRangeAsync(url, file, item);
        }

        static async Task DownloadWithRangeAsync(string url, StorageFile file, DownloadItem item)
        {
            long have = (long)(await file.GetBasicPropertiesAsync()).Size;
            using (var req = new HttpRequestMessage(HttpMethod.Get, new Uri(url)))
            {
                req.Headers.TryAddWithoutValidation("User-Agent", Innertube.BrowserUserAgent);
                if (have > 0) req.Headers.Range = new RangeHeaderValue(have, null);
                using (var resp = await Innertube.Http.SendAsync(req, HttpCompletionOption.ResponseHeadersRead))
                {
                    if (resp.StatusCode == HttpStatusCode.RequestedRangeNotSatisfiable) { have = 0; }
                    else resp.EnsureSuccessStatusCode();
                    bool partial = resp.StatusCode == HttpStatusCode.PartialContent;
                    if (!partial) have = 0;
                    long length = resp.Content.Headers.ContentLength ?? -1;
                    long total = length < 0 ? -1 : length + have;

                    using (var output = await file.OpenStreamForWriteAsync())
                    using (var input = await resp.Content.ReadAsStreamAsync())
                    {
                        output.SetLength(have);
                        output.Seek(have, SeekOrigin.Begin);
                        var buffer = new byte[64 * 1024];
                        long done = have;
                        int n;
                        while ((n = await input.ReadAsync(buffer, 0, buffer.Length)) > 0)
                        {
                            await output.WriteAsync(buffer, 0, n);
                            done += n;
                            if (total > 0) item.Progress = 100.0 * done / total;
                        }
                    }
                }
            }
        }
    }
}
