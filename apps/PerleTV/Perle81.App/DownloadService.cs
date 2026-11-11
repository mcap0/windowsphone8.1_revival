/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Persistent audio download queue for Perle TV. BackgroundTransfer owns the network transfer while the app is
 * suspended or terminated; the app reattaches on launch to tag and move a completed file into Music\\Perle.
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Perle.Extractor;
using Perle.Progressive;
using Perle81.Native;
using Windows.Data.Xml.Dom;
using Windows.Networking.BackgroundTransfer;
using Windows.Storage;
using Windows.UI.Notifications;

namespace Perle81.App
{
    public enum DownloadState { Waiting, Running, Done, Failed, Cancelled }

    public sealed class DownloadItem : INotifyPropertyChanged
    {
        public const int MaxAttempts = 3;

        public string Url { get; set; }
        public string VideoId { get; set; }
        public string OperationId { get; set; }
        public string ArtistName { get; set; }
        public string TrackName { get; set; }
        public string Year { get; set; }
        public string SavedRelativePath { get; set; }

        string title, uploader, message;
        double progress;
        DownloadState state;
        int attempts;
        bool deleting;

        internal DownloadOperation TransferOperation;
        internal CancellationTokenSource CancelSource;
        internal Task WorkTask;
        internal bool CancelRequested;

        public event PropertyChangedEventHandler PropertyChanged;
        void Raise(string name) { var h = PropertyChanged; if (h != null) h(this, new PropertyChangedEventArgs(name)); }

        public string Title { get { return title; } set { title = value; Raise("Title"); } }
        public string Uploader { get { return uploader; } set { uploader = value; Raise("Uploader"); } }
        public string Message { get { return message; } set { message = value; Raise("Message"); } }
        public double Progress { get { return progress; } set { progress = value; Raise("Progress"); } }
        public DownloadState State
        {
            get { return state; }
            set { state = value; Raise("State"); Raise("CanRetry"); Raise("DeleteLabel"); }
        }
        public int Attempts { get { return attempts; } set { attempts = value; Raise("Attempts"); } }
        public bool CanRetry { get { return State == DownloadState.Failed; } }
        public bool CanDelete { get { return !deleting; } }
        internal bool Deleting
        {
            get { return deleting; }
            set { deleting = value; Raise("CanDelete"); }
        }
        public string DeleteLabel
        {
            get
            {
                if (State == DownloadState.Running) return "Blocca e elimina";
                if (State == DownloadState.Done) return "Elimina audio";
                return "Rimuovi download";
            }
        }
    }

    public static class DownloadService
    {
        public sealed class PersistedDownload
        {
            public string Url { get; set; }
            public string VideoId { get; set; }
            public string OperationId { get; set; }
            public string Title { get; set; }
            public string Uploader { get; set; }
            public string ArtistName { get; set; }
            public string TrackName { get; set; }
            public string Year { get; set; }
            public string SavedRelativePath { get; set; }
            public string Message { get; set; }
            public double Progress { get; set; }
            public DownloadState State { get; set; }
            public int Attempts { get; set; }
        }

        const string QueueFile = "downloads81.json";
        public static readonly ObservableCollection<DownloadItem> Items = new ObservableCollection<DownloadItem>();
        static readonly SemaphoreSlim transferGate = new SemaphoreSlim(1);
        static readonly SemaphoreSlim saveGate = new SemaphoreSlim(1);
        static readonly object initLock = new object();
        static Task initializeTask;

        // Diagnostics for the background-transfer restore path: share/inbox/perletv-dl.txt on the PC (tools/inbox).
        static void Log(string line)
        {
            try
            {
                var client = new Windows.Web.Http.HttpClient();
                var body = new Windows.Web.Http.HttpStringContent(DateTime.Now.ToString("HH:mm:ss ") + line);
                var ignored = client.PostAsync(new Uri("http://192.168.1.148:8765/perletv-dl"), body);
            }
            catch (Exception) { }
        }

        // A raw .m4a left by a transfer that finished while Perle TV was not running: complete when its top-level
        // MP4 boxes cover the whole file and one of them is moov.
        static async Task<bool> RawLooksCompleteAsync(StorageFile raw)
        {
            try
            {
                using (var stream = (await raw.OpenReadAsync()).AsStreamForRead())
                {
                    long length = stream.Length, pos = 0;
                    bool moov = false;
                    var header = new byte[16];
                    while (pos + 8 <= length)
                    {
                        stream.Seek(pos, SeekOrigin.Begin);
                        if (stream.Read(header, 0, 16) < 8) return false;
                        long size = ((long)header[0] << 24) | ((long)header[1] << 16) | ((long)header[2] << 8) | header[3];
                        string type = System.Text.Encoding.UTF8.GetString(header, 4, 4);
                        if (size == 1)
                        {
                            size = 0;
                            for (int i = 8; i < 16; i++) size = (size << 8) | header[i];
                        }
                        if (size < 8) return false;
                        if (type == "moov") moov = true;
                        pos += size;
                    }
                    return moov && pos == length && length > 0;
                }
            }
            catch (Exception) { return false; }
        }

        static async Task FinishRawAsync(DownloadItem item, StorageFile raw)
        {
            var cancellation = new CancellationTokenSource();
            item.CancelSource = cancellation;
            try
            {
                await FinalizeDownloadedAsync(item, raw, cancellation.Token);
                item.State = DownloadState.Done;
                item.Progress = 100;
                item.Message = "Salvato in Musica\\Perle\\" + item.SavedRelativePath;
                await SaveQueueAsync();
                ShowToast("Download completato", item.Title + " salvato in Musica\\Perle");
            }
            catch (Exception ex)
            {
                Log("finish raw " + item.VideoId + " failed: " + Describe(ex));
                item.State = DownloadState.Failed;
                item.Message = "Errore: " + Describe(ex) + " (usa Riprova)";
                await SaveQueueAsync();
            }
            finally
            {
                if (item.CancelSource == cancellation) item.CancelSource = null;
                cancellation.Dispose();
            }
        }

        public static Task InitializeAsync()
        {
            lock (initLock)
            {
                if (initializeTask == null) initializeTask = RestoreAsync();
                return initializeTask;
            }
        }

        public static DownloadItem Enqueue(string url, string title, string uploader)
        {
            string videoId = StreamUrls.VideoIdFromUrl(url);
            var existing = Items.FirstOrDefault(x => x.VideoId == videoId &&
                (x.State == DownloadState.Waiting || x.State == DownloadState.Running));
            if (existing != null) return existing;

            var item = new DownloadItem
            {
                Url = url,
                VideoId = videoId,
                Title = title ?? url,
                Uploader = uploader ?? "",
                State = DownloadState.Waiting,
                Message = "In coda"
            };
            Items.Insert(0, item);
            item.WorkTask = StartNewAsync(item);
            return item;
        }

        static async Task StartNewAsync(DownloadItem item)
        {
            await InitializeAsync();
            if (item.CancelRequested || item.TransferOperation != null) return;
            await SaveQueueAsync();
            var work = RunAsync(item);
            item.WorkTask = work;
            await work;
        }

        public static void Retry(DownloadItem item)
        {
            if (item == null || item.State != DownloadState.Failed) return;
            item.Attempts = 0;
            item.OperationId = null;
            item.CancelRequested = false;
            item.State = DownloadState.Waiting;
            item.Message = "In coda";
            item.WorkTask = StartNewAsync(item);
        }

        public static async Task DeleteAsync(DownloadItem item)
        {
            if (item == null || item.Deleting) return;
            item.Deleting = true;
            try { await DeleteCoreAsync(item); }
            finally { item.Deleting = false; }
        }

        static async Task DeleteCoreAsync(DownloadItem item)
        {
            item.CancelRequested = true;
            item.State = DownloadState.Cancelled;
            item.Message = "Annullamento…";
            await SaveQueueAsync();
            if (item.CancelSource != null)
            {
                try { item.CancelSource.Cancel(); } catch (ObjectDisposedException) { }
            }
            Task work = item.WorkTask;
            if (work != null)
            {
                try { await work; } catch (OperationCanceledException) { }
            }

            await DeletePartialFilesAsync(item);
            await DeleteSavedFileAsync(item);
            Items.Remove(item);
            await SaveQueueAsync();
        }

        static async Task RestoreAsync()
        {
            var alreadyPresent = new HashSet<DownloadItem>(Items);
            try
            {
                StorageFile queue = await ApplicationData.Current.LocalFolder.GetFileAsync(QueueFile);
                string json = await FileIO.ReadTextAsync(queue);
                var records = JsonConvert.DeserializeObject<List<PersistedDownload>>(json) ?? new List<PersistedDownload>();
                foreach (var record in records)
                {
                    if (string.IsNullOrEmpty(record.Url) || string.IsNullOrEmpty(record.VideoId)) continue;
                    Items.Add(new DownloadItem
                    {
                        Url = record.Url,
                        VideoId = record.VideoId,
                        OperationId = record.OperationId,
                        Title = record.Title,
                        Uploader = record.Uploader,
                        ArtistName = record.ArtistName,
                        TrackName = record.TrackName,
                        Year = record.Year,
                        SavedRelativePath = record.SavedRelativePath,
                        Message = record.Message,
                        Progress = record.Progress,
                        State = record.State,
                        Attempts = record.Attempts
                    });
                }
            }
            catch (FileNotFoundException) { }
            catch (Exception) { /* a damaged queue must not stop Perle TV from launching */ }

            IReadOnlyList<DownloadOperation> current;
            try { current = await BackgroundDownloader.GetCurrentDownloadsAsync(); }
            catch (Exception ex) { Log("GetCurrentDownloadsAsync: " + Describe(ex)); current = new List<DownloadOperation>(); }
            foreach (var op in current)
            {
                try
                {
                    Log("op " + op.Guid + " " + op.Progress.Status + " " + op.Progress.BytesReceived + "/" +
                        op.Progress.TotalBytesToReceive + " " + op.ResultFile.Name);
                }
                catch (Exception) { }
            }
            foreach (var it in Items) Log("item " + it.VideoId + " " + it.State + " op=" + it.OperationId + " " + it.Progress.ToString("0"));

            var pending = new List<DownloadItem>();
            var attachedIds = new HashSet<Guid>();
            foreach (var item in Items.ToArray())
            {
                // Enqueue can race the first launch restore; its own StartNewAsync will start it once restore finishes.
                if (alreadyPresent.Contains(item)) continue;
                DownloadOperation operation = null;
                Guid id;
                if (!string.IsNullOrEmpty(item.OperationId) && Guid.TryParse(item.OperationId, out id))
                    operation = current.FirstOrDefault(x => x.Guid == id);

                if (operation != null)
                {
                    attachedIds.Add(operation.Guid);
                    item.State = DownloadState.Running;
                    item.Message = "Ripresa download…";
                    item.TransferOperation = operation;
                    item.WorkTask = AttachAndFinishAsync(item, operation);
                }
                else if (item.State == DownloadState.Running || item.State == DownloadState.Waiting)
                {
                    if (!string.IsNullOrEmpty(item.OperationId))
                    {
                        // The transfer is gone from the system queue: either it finished while we were not running
                        // (keep the file) or it failed (start again).
                        StorageFile raw = null;
                        try
                        {
                            var dl = await ApplicationData.Current.LocalFolder.CreateFolderAsync("dl", CreationCollisionOption.OpenIfExists);
                            raw = await dl.GetFileAsync(item.VideoId + ".m4a");
                        }
                        catch (Exception) { }
                        bool complete = raw != null && await RawLooksCompleteAsync(raw);
                        Log("item " + item.VideoId + " op gone, raw " + (raw == null ? "missing" : complete ? "complete" : "partial"));
                        if (complete)
                        {
                            item.OperationId = null;
                            item.State = DownloadState.Running;
                            item.Progress = 100;
                            item.Message = "Completamento…";
                            item.WorkTask = FinishRawAsync(item, raw);
                            continue;
                        }
                    }
                    item.OperationId = null;
                    item.State = DownloadState.Waiting;
                    item.Message = "Ripresa download in coda";
                    pending.Add(item);
                }
            }

            // Older builds kept the queue only in memory. Recover their transfers from the raw-file name when possible.
            foreach (var operation in current)
            {
                if (attachedIds.Contains(operation.Guid)) continue;
                string videoId = Path.GetFileNameWithoutExtension(operation.ResultFile.Path);
                if (LooksLikeVideoId(videoId))
                {
                    var item = Items.FirstOrDefault(x => x.VideoId == videoId);
                    if (item == null)
                    {
                        item = new DownloadItem
                        {
                            Url = "https://www.youtube.com/watch?v=" + videoId,
                            VideoId = videoId,
                            Title = videoId,
                            Uploader = "",
                            State = DownloadState.Running,
                            Message = "Ripresa download…"
                        };
                        Items.Insert(0, item);
                    }
                    pending.Remove(item);
                    item.OperationId = operation.Guid.ToString();
                    item.State = DownloadState.Running;
                    item.Message = "Ripresa download…";
                    item.TransferOperation = operation;
                    item.WorkTask = AttachAndFinishAsync(item, operation);
                    attachedIds.Add(operation.Guid);
                    continue;
                }

                // Anything else cannot be tied to a Perle item, so it is an orphan from an older build.
                await StopTransferAsync(operation);
                try { await operation.ResultFile.DeleteAsync(); } catch (Exception) { }
            }

            await SaveQueueAsync();
            foreach (var item in pending) item.WorkTask = RunAsync(item);
        }

        static async Task SaveQueueAsync()
        {
            await saveGate.WaitAsync();
            try
            {
                var records = Items.Select(item => new PersistedDownload
                {
                    Url = item.Url,
                    VideoId = item.VideoId,
                    OperationId = item.OperationId,
                    Title = item.Title,
                    Uploader = item.Uploader,
                    ArtistName = item.ArtistName,
                    TrackName = item.TrackName,
                    Year = item.Year,
                    SavedRelativePath = item.SavedRelativePath,
                    Message = item.Message,
                    Progress = item.Progress,
                    State = item.State,
                    Attempts = item.Attempts
                }).ToList();
                StorageFile file = await ApplicationData.Current.LocalFolder.CreateFileAsync(
                    QueueFile, CreationCollisionOption.ReplaceExisting);
                await FileIO.WriteTextAsync(file, JsonConvert.SerializeObject(records));
            }
            catch (Exception) { /* downloads continue if local queue persistence fails */ }
            finally { saveGate.Release(); }
        }

        static async Task RunAsync(DownloadItem item)
        {
            var cancellation = new CancellationTokenSource();
            item.CancelSource = cancellation;
            bool entered = false;
            try
            {
                await transferGate.WaitAsync(cancellation.Token);
                entered = true;
                while (!item.CancelRequested && item.Attempts < DownloadItem.MaxAttempts)
                {
                    cancellation.Token.ThrowIfCancellationRequested();
                    item.Attempts++;
                    item.State = DownloadState.Running;
                    item.Progress = 0;
                    item.Message = "Analisi del video…";
                    await SaveQueueAsync();
                    try
                    {
                        await DownloadOnceAsync(item, cancellation.Token);
                        cancellation.Token.ThrowIfCancellationRequested();
                        item.State = DownloadState.Done;
                        item.Progress = 100;
                        item.Message = "Salvato in Musica\\Perle\\" + item.SavedRelativePath;
                        await SaveQueueAsync();
                        ShowToast("Download completato", item.Title + " salvato in Musica\\Perle");
                        return;
                    }
                    catch (OperationCanceledException)
                    {
                        if (!item.CancelRequested) throw;
                        await StopTransferAsync(item.TransferOperation);
                        item.TransferOperation = null;
                        item.OperationId = null;
                        item.State = DownloadState.Cancelled;
                        item.Message = "Download annullato";
                        await SaveQueueAsync();
                        return;
                    }
                    catch (Exception ex)
                    {
                        await StopTransferAsync(item.TransferOperation);
                        item.TransferOperation = null;
                        item.OperationId = null;
                        if (item.CancelRequested)
                        {
                            item.State = DownloadState.Cancelled;
                            item.Message = "Download annullato";
                            await SaveQueueAsync();
                            return;
                        }
                        if (item.Attempts >= DownloadItem.MaxAttempts)
                        {
                            item.State = DownloadState.Failed;
                            item.Message = "Errore: " + Describe(ex) + " (tentativi esauriti, usa Riprova)";
                            await SaveQueueAsync();
                            ShowToast("Download non riuscito", item.Title + ": " + Describe(ex));
                            return;
                        }
                        item.Message = "Errore: " + Describe(ex) + ". Nuovo tentativo…";
                        await SaveQueueAsync();
                        await Task.Delay(TimeSpan.FromSeconds(10 * item.Attempts), cancellation.Token);
                    }
                }
                if (item.CancelRequested)
                {
                    item.State = DownloadState.Cancelled;
                    item.Message = "Download annullato";
                    await SaveQueueAsync();
                }
            }
            catch (OperationCanceledException)
            {
                await StopTransferAsync(item.TransferOperation);
                item.OperationId = null;
                item.State = DownloadState.Cancelled;
                item.Message = "Download annullato";
                await SaveQueueAsync();
            }
            finally
            {
                if (entered) transferGate.Release();
                if (item.CancelSource == cancellation) item.CancelSource = null;
                item.TransferOperation = null;
                cancellation.Dispose();
            }
        }

        static async Task AttachAndFinishAsync(DownloadItem item, DownloadOperation operation)
        {
            var cancellation = new CancellationTokenSource();
            item.CancelSource = cancellation;
            bool entered = false;
            try
            {
                await transferGate.WaitAsync(cancellation.Token);
                entered = true;
                item.TransferOperation = operation;
                item.OperationId = operation.Guid.ToString();
                var progress = new Progress<DownloadOperation>(d => UpdateProgress(item, d));
                var attached = operation.AttachAsync();
                await attached.AsTask(cancellation.Token, progress);
                cancellation.Token.ThrowIfCancellationRequested();
                item.OperationId = null;
                item.TransferOperation = null;
                await SaveQueueAsync();

                StorageFolder dir = await ApplicationData.Current.LocalFolder.CreateFolderAsync("dl", CreationCollisionOption.OpenIfExists);
                StorageFile raw = await dir.GetFileAsync(item.VideoId + ".m4a");
                await FinalizeDownloadedAsync(item, raw, cancellation.Token);
                item.State = DownloadState.Done;
                item.Progress = 100;
                item.Message = "Salvato in Musica\\Perle\\" + item.SavedRelativePath;
                await SaveQueueAsync();
                ShowToast("Download completato", item.Title + " salvato in Musica\\Perle");
            }
            catch (OperationCanceledException)
            {
                await StopTransferAsync(operation);
                item.OperationId = null;
                item.TransferOperation = null;
                item.State = DownloadState.Cancelled;
                item.Message = "Download annullato";
                await SaveQueueAsync();
            }
            catch (Exception ex)
            {
                await StopTransferAsync(operation);
                item.OperationId = null;
                item.TransferOperation = null;
                item.State = DownloadState.Failed;
                item.Message = "Errore dopo la ripresa: " + Describe(ex) + " (usa Riprova)";
                await SaveQueueAsync();
                ShowToast("Download non riuscito", item.Title + ": " + Describe(ex));
            }
            finally
            {
                if (entered) transferGate.Release();
                if (item.CancelSource == cancellation) item.CancelSource = null;
                item.TransferOperation = null;
                cancellation.Dispose();
            }
        }

        static void UpdateProgress(DownloadItem item, DownloadOperation download)
        {
            var p = download.Progress;
            if (p.TotalBytesToReceive > 0)
            {
                item.Progress = 100.0 * p.BytesReceived / p.TotalBytesToReceive;
                item.Message = String.Format("Download… {0:0.0} / {1:0.0} MB",
                    p.BytesReceived / 1048576.0, p.TotalBytesToReceive / 1048576.0);
            }
            else
            {
                item.Message = String.Format("Download… {0:0.0} MB", p.BytesReceived / 1048576.0);
            }
        }

        static async Task DownloadOnceAsync(DownloadItem item, CancellationToken cancellation)
        {
            cancellation.ThrowIfCancellationRequested();
            var info = await YoutubeStream.GetInfoAsync(item.Url);
            cancellation.ThrowIfCancellationRequested();
            var audio = info.AdaptiveFormats.FirstOrDefault(f => f.Itag == 140) ?? YoutubeStream.BestAudio(info);
            if (audio == null || audio.Url == null) throw new ExtractionException("nessun flusso audio AAC per questo video");

            string channel = info.UploaderName ?? "";
            string name = info.Name ?? item.VideoId;
            item.Title = name;
            item.Uploader = channel;
            item.ArtistName = TitleTools.ArtistOf(channel, name) ?? channel;
            item.TrackName = TitleTools.TrackOf(channel, name) ?? name;
            item.Year = "";
            try { item.Year = await VideoDate.GetYearAsync(item.VideoId); } catch (Exception) { }
            await SaveQueueAsync();

            var local = ApplicationData.Current.LocalFolder;
            var dir = await local.CreateFolderAsync("dl", CreationCollisionOption.OpenIfExists);
            var raw = await dir.CreateFileAsync(item.VideoId + ".m4a", CreationCollisionOption.ReplaceExisting);
            item.Message = "Download…";
            await DownloadAsync(audio.Url, raw, item, cancellation);
            cancellation.ThrowIfCancellationRequested();
            await FinalizeDownloadedAsync(item, raw, cancellation);
        }

        static async Task DownloadAsync(string url, StorageFile file, DownloadItem item, CancellationToken cancellation)
        {
            var downloader = new BackgroundDownloader();
            downloader.CostPolicy = BackgroundTransferCostPolicy.Always;
            downloader.SuccessToastNotification = CreateToast("Download ricevuto", item.Title + ": Perle TV sta completando il file.");
            downloader.FailureToastNotification = CreateToast("Download non riuscito", item.Title + ": apri Perle TV per ritentare.");
            // WP8.1: request headers belong to the downloader and apply to the downloads it creates afterwards
            downloader.SetRequestHeader("User-Agent", Innertube.BrowserUserAgent);

            var download = downloader.CreateDownload(new Uri(url), file);
            item.TransferOperation = download;
            item.OperationId = download.Guid.ToString();
            await SaveQueueAsync();

            var progress = new Progress<DownloadOperation>(d => UpdateProgress(item, d));
            var started = download.StartAsync();
            await started.AsTask(cancellation, progress);
            item.TransferOperation = null;
            item.OperationId = null;
            await SaveQueueAsync();
        }

        static async Task FinalizeDownloadedAsync(DownloadItem item, StorageFile raw, CancellationToken cancellation)
        {
            cancellation.ThrowIfCancellationRequested();
            if (String.IsNullOrEmpty(item.TrackName) || String.IsNullOrEmpty(item.ArtistName))
            {
                var info = await YoutubeStream.GetInfoAsync(item.Url);
                item.Title = info.Name ?? item.VideoId;
                item.Uploader = info.UploaderName ?? "";
                item.ArtistName = TitleTools.ArtistOf(item.Uploader, item.Title) ?? item.Uploader;
                item.TrackName = TitleTools.TrackOf(item.Uploader, item.Title) ?? item.Title;
                try { item.Year = await VideoDate.GetYearAsync(item.VideoId); } catch (Exception) { item.Year = ""; }
                await SaveQueueAsync();
            }
            var local = ApplicationData.Current.LocalFolder;
            var dir = await local.CreateFolderAsync("dl", CreationCollisionOption.OpenIfExists);
            var tagged = await dir.CreateFileAsync(item.VideoId + ".tagged.m4a", CreationCollisionOption.ReplaceExisting);
            item.Message = "Tag MP4…";
            byte[] cover = new byte[0];
            try { cover = await Innertube.Http.GetByteArrayAsync(Thumbs.MediumOf(item.VideoId)); } catch (Exception) { }
            cancellation.ThrowIfCancellationRequested();

            var tags = new Mp4TagSet
            {
                Title = item.TrackName ?? item.Title ?? item.VideoId,
                Artist = item.ArtistName ?? item.Uploader ?? "",
                Album = "Perle",
                Year = item.Year ?? "",
                Cover = cover
            };
            string rawPath = raw.Path, taggedPath = tagged.Path;
            string error = await Task.Run(() => Mp4Tagger.Write(rawPath, taggedPath, tags), cancellation);
            if (!string.IsNullOrEmpty(error)) throw new IOException("tag: " + error);
            cancellation.ThrowIfCancellationRequested();

            var music = KnownFolders.MusicLibrary;
            var perle = await music.CreateFolderAsync("Perle", CreationCollisionOption.OpenIfExists);
            string artist = TitleTools.SafeName(item.ArtistName ?? item.Uploader);
            string track = TitleTools.SafeName(item.TrackName ?? item.Title ?? item.VideoId);
            if (String.IsNullOrEmpty(artist)) artist = "Sconosciuto";
            if (String.IsNullOrEmpty(track)) track = item.VideoId;
            var artistFolder = await perle.CreateFolderAsync(artist, CreationCollisionOption.OpenIfExists);
            string fileName = track + ".m4a";
            await tagged.MoveAsync(artistFolder, fileName, NameCollisionOption.ReplaceExisting);
            await raw.DeleteAsync();
            item.SavedRelativePath = artist + "\\" + fileName;
            await SaveQueueAsync();
        }

        static async Task StopTransferAsync(DownloadOperation operation)
        {
            if (operation == null) return;
            try
            {
                // Cancellation must target the WinRT operation returned by AttachAsync; cancelling only the
                // caller's wait can otherwise leave BackgroundTransfer running after the app drops its handle.
                var attached = operation.AttachAsync();
                attached.Cancel();
                try { await attached.AsTask(); } catch (Exception) { }
            }
            catch (Exception) { }
        }

        static async Task DeletePartialFilesAsync(DownloadItem item)
        {
            if (String.IsNullOrEmpty(item.VideoId)) return;
            try
            {
                var dir = await ApplicationData.Current.LocalFolder.GetFolderAsync("dl");
                foreach (string suffix in new[] { ".m4a", ".tagged.m4a" })
                {
                    try { await (await dir.GetFileAsync(item.VideoId + suffix)).DeleteAsync(); }
                    catch (FileNotFoundException) { }
                }
            }
            catch (FileNotFoundException) { }
        }

        static async Task DeleteSavedFileAsync(DownloadItem item)
        {
            if (String.IsNullOrEmpty(item.SavedRelativePath)) return;
            try
            {
                string[] parts = item.SavedRelativePath.Split(new[] { '\\', '/' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length == 0) return;
                StorageFolder folder = await KnownFolders.MusicLibrary.GetFolderAsync("Perle");
                for (int i = 0; i < parts.Length - 1; i++) folder = await folder.GetFolderAsync(parts[i]);
                await (await folder.GetFileAsync(parts[parts.Length - 1])).DeleteAsync();
            }
            catch (FileNotFoundException) { }
        }

        static bool LooksLikeVideoId(string value)
        {
            if (String.IsNullOrEmpty(value) || value.Length != 11) return false;
            foreach (char c in value)
            {
                if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
                    !(c >= '0' && c <= '9') && c != '_' && c != '-') return false;
            }
            return true;
        }

        static ToastNotification CreateToast(string title, string text)
        {
            XmlDocument xml = ToastNotificationManager.GetTemplateContent(ToastTemplateType.ToastText02);
            var nodes = xml.GetElementsByTagName("text");
            nodes.Item(0).AppendChild(xml.CreateTextNode(title ?? "Perle TV"));
            nodes.Item(1).AppendChild(xml.CreateTextNode(text ?? ""));
            return new ToastNotification(xml);
        }

        static void ShowToast(string title, string text)
        {
            try { ToastNotificationManager.CreateToastNotifier().Show(CreateToast(title, text)); }
            catch (Exception) { }
        }

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
    }
}
