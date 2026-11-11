/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Shared state of the pages: subscriptions, the Perle repository, the watch history and the view models.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Perle;
using Perle.Extractor;
using Perle.Feeds;
using System.Threading;
using Windows.Storage;
using Windows.UI.Xaml.Media.Imaging;

namespace Perle81.App
{
    public sealed class VideoItem
    {
        public PerleVideo Video { get; }
        BitmapImage thumbnail;

        public VideoItem(PerleVideo v)
        {
            Video = v;
        }

        public string Title => Video.Title;
        public string Uploader => Video.Uploader;

        /// <summary>Created on first use, so that a long list only decodes the thumbnails it shows.</summary>
        public BitmapImage Thumbnail
        {
            get
            {
                if (thumbnail == null && Thumbs.Medium(Video.Url) != null)
                    thumbnail = new BitmapImage(new Uri(Thumbs.Medium(Video.Url))) { DecodePixelWidth = 160 };
                return thumbnail;
            }
        }

        public string DurationText => Video.Duration < 0 ? "" : Format.Duration(Video.Duration);

        public string Details
        {
            get
            {
                var parts = new List<string>();
                if (Video.ViewCount.HasValue) parts.Add(Format.Views(Video.ViewCount.Value));
                if (!string.IsNullOrEmpty(Video.TextualUploadDate)) parts.Add(Video.TextualUploadDate);
                return string.Join(" · ", parts);
            }
        }
    }

    /// <summary>
    /// Thumbnails: YouTube serves WebP for some videos, which Windows Phone 8.1 does not decode; the mqdefault
    /// JPEG of the same video is always available.
    /// </summary>
    public static class Thumbs
    {
        public static string MediumOf(string videoId) => "https://i.ytimg.com/vi/" + videoId + "/mqdefault.jpg";

        public static string Medium(string videoUrl)
        {
            try { return "https://i.ytimg.com/vi/" + StreamUrls.VideoIdFromUrl(videoUrl) + "/mqdefault.jpg"; }
            catch (Exception) { return null; }
        }
    }

    public static class Format
    {
        public static string Views(long v) =>
            v >= 1000000000 ? (v / 1e9).ToString("0.#") + " Mrd visualizzazioni"
            : v >= 1000000 ? (v / 1e6).ToString("0.#") + " Mln visualizzazioni"
            : v >= 1000 ? (v / 1e3).ToString("0.#") + "K visualizzazioni"
            : v + " visualizzazioni";

        /// <summary>Localization.getDurationString: 1:02:05, 4:07.</summary>
        public static string Duration(long seconds)
        {
            if (seconds < 0) seconds = 0;
            long h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
            return h > 0 ? h + ":" + m.ToString("00") + ":" + s.ToString("00") : m + ":" + s.ToString("00");
        }
    }

    public sealed class GroupChoice
    {
        public long Id { get; set; }
        public string Name { get; set; }
    }

    /// <summary>One watched video, newest first. The repository keeps the set of urls for the filter.</summary>
    public sealed class HistoryEntry
    {
        public string Url { get; set; }
        public string Title { get; set; }
        public string Uploader { get; set; }
        public long WatchedAt { get; set; }

        [JsonIgnore]
        public BitmapImage Thumbnail
        {
            get
            {
                string u = Thumbs.Medium(Url);
                return u == null ? null : new BitmapImage(new Uri(u)) { DecodePixelWidth = 160 };
            }
        }
        [JsonIgnore]
        public string Details => new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddMilliseconds(WatchedAt).ToLocalTime().ToString("d MMM, HH:mm");
    }

    public static class AppState
    {
        public static readonly WinRtStorage Storage = new WinRtStorage();
        public static readonly PerleRepository Repository = new PerleRepository(Storage);
        public static SubscriptionsData Subscriptions;
        /// <summary>Changes on every import, so that MainPage knows it has to reload.</summary>
        public static int SubscriptionsVersion;
        /// <summary>The file chosen by FileOpenPicker, waiting for ImportPage.</summary>
        public static StorageFile PendingImport;

        const string SubscriptionsFile = "subscriptions.json";
        const string HistoryFile = "history81.json";
        const int MaxHistory = 500;
        static List<HistoryEntry> history;

        public static async Task<SubscriptionsData> LoadSubscriptionsAsync()
        {
            if (Subscriptions != null) return Subscriptions;
            var text = await Storage.ReadTextAsync(SubscriptionsFile);
            if (text == null)
            {
                // first start: the export bundled with this build
                try
                {
                    var bundled = await StorageFile.GetFileFromApplicationUriAsync(new Uri("ms-appx:///Assets/subscriptions.json"));
                    text = await FileIO.ReadTextAsync(bundled);
                    await Storage.WriteTextAsync(SubscriptionsFile, text);
                }
                catch (Exception) { text = "{}"; }
            }
            Subscriptions = SubscriptionsData.Parse(text);
            return Subscriptions;
        }

        public static async Task ImportAsync(string json)
        {
            var data = SubscriptionsData.Parse(json); // throws on invalid files, before overwriting anything
            await Storage.WriteTextAsync(SubscriptionsFile, json);
            Subscriptions = data;
            SubscriptionsVersion++;
        }

        public static async Task<List<HistoryEntry>> HistoryAsync()
        {
            if (history != null) return history;
            try
            {
                var text = await Storage.ReadTextAsync(HistoryFile);
                history = text == null ? new List<HistoryEntry>() : JsonConvert.DeserializeObject<List<HistoryEntry>>(text) ?? new List<HistoryEntry>();
            }
            catch (Exception) { history = new List<HistoryEntry>(); }
            return history;
        }

        public static async Task AddToHistoryAsync(StreamInfo info)
        {
            var list = await HistoryAsync();
            list.RemoveAll(h => h.Url == info.Url);
            list.Insert(0, new HistoryEntry
            {
                Url = info.Url,
                Title = info.Name,
                Uploader = info.UploaderName,
                WatchedAt = (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalMilliseconds
            });
            if (list.Count > MaxHistory) list.RemoveRange(MaxHistory, list.Count - MaxHistory);
            await Storage.WriteTextAsync(HistoryFile, JsonConvert.SerializeObject(list));
            await Repository.AddToHistoryAsync(info.Url);
        }
    }

    /// <summary>Where each video was stopped, per viewer (LocalSettings).</summary>
    public static class Positions
    {
        static string Key(string url) => "pos:" + url;

        public static long Get(string url)
        {
            try
            {
                object v = Windows.Storage.ApplicationData.Current.LocalSettings.Values[Key(url)];
                return v is string ? long.Parse((string)v) : 0;
            }
            catch (Exception) { return 0; }
        }

        public static void Save(string url, long seconds)
        {
            try { Windows.Storage.ApplicationData.Current.LocalSettings.Values[Key(url)] = seconds.ToString(); }
            catch (Exception) { }
        }
    }
}

namespace Perle81.App
{
    public sealed class LatestEntry
    {
        public string Url { get; set; }
        public string Title { get; set; }
        public string Uploader { get; set; }
        public long PublishedTicks { get; set; }
    }

    public sealed class LatestCache
    {
        public long Timestamp { get; set; }
        public int FailedChannels { get; set; }
        public List<LatestEntry> Items { get; set; } = new List<LatestEntry>();
    }

    /// <summary>The "Ultimi" tab: the latest uploads of the subscriptions of a folder, from the channel feeds.</summary>
    public static class LatestStore
    {
        public const int MaxConcurrent = 4;
        public const int MaxItems = 300;
        public static readonly TimeSpan Fresh = TimeSpan.FromHours(1);

        static string FileFor(long groupId) => "latest81-" + groupId + ".json";

        public static async Task<LatestCache> ReadAsync(long groupId)
        {
            try
            {
                var text = await AppState.Storage.ReadTextAsync(FileFor(groupId));
                return text == null ? null : JsonConvert.DeserializeObject<LatestCache>(text);
            }
            catch (Exception) { return null; }
        }

        public static bool IsFresh(LatestCache cache) =>
            cache != null && DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddMilliseconds(cache.Timestamp) < Fresh;

        public static async Task<LatestCache> RefreshAsync(long groupId, Action<int, int> progress)
        {
            var subs = (await AppState.LoadSubscriptionsAsync()).SubscriptionsFor(groupId);
            var all = new List<LatestEntry>();
            int done = 0, failed = 0;
            progress?.Invoke(0, subs.Count);
            using (var gate = new SemaphoreSlim(MaxConcurrent))
            {
                var tasks = subs.Select(async s =>
                {
                    await gate.WaitAsync().ConfigureAwait(false);
                    try
                    {
                        // channel URLs with a handle (/@name) have no channel id in them: those are skipped
                        string channelId = YoutubeChannel.ChannelIdFromUrl(s.Url);
                        var videos = await YoutubeFeeds.LoadAsync(channelId).ConfigureAwait(false);
                        lock (all)
                            foreach (var v in videos)
                                all.Add(new LatestEntry
                                {
                                    Url = StreamUrls.FromVideoId(v.VideoId),
                                    Title = v.Title,
                                    Uploader = v.Uploader,
                                    PublishedTicks = v.Published.UtcTicks
                                });
                    }
                    catch (Exception) { Interlocked.Increment(ref failed); }
                    finally
                    {
                        gate.Release();
                        progress?.Invoke(Interlocked.Increment(ref done), subs.Count);
                    }
                }).ToList();
                await Task.WhenAll(tasks).ConfigureAwait(false);
            }

            var cache = new LatestCache
            {
                Timestamp = (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalMilliseconds,
                FailedChannels = failed,
                Items = all.OrderByDescending(e => e.PublishedTicks).GroupBy(e => e.Url).Select(g => g.First()).Take(MaxItems).ToList()
            };
            try { await AppState.Storage.WriteTextAsync(FileFor(groupId), JsonConvert.SerializeObject(cache)); }
            catch (Exception) { }
            return cache;
        }

        public static PerleVideo ToVideo(LatestEntry e)
        {
            var published = new DateTimeOffset(e.PublishedTicks, TimeSpan.Zero).ToLocalTime();
            return new PerleVideo
            {
                Url = e.Url,
                Title = e.Title,
                Uploader = e.Uploader,
                Duration = -1,
                TextualUploadDate = published.ToString("d MMM yyyy", System.Globalization.CultureInfo.CurrentCulture)
            };
        }
    }
}
