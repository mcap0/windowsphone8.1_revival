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
using Windows.UI.Xaml.Controls;
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
        public Windows.UI.Xaml.Visibility DurationVisibility =>
            Video.Duration < 0 ? Windows.UI.Xaml.Visibility.Collapsed : Windows.UI.Xaml.Visibility.Visible;

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

        /// <summary>Same channel whatever the form of the address: /channel/UC... id, else the address itself.</summary>
        static string ChannelKey(string url)
        {
            var m = System.Text.RegularExpressions.Regex.Match(url ?? "", @"/channel/(UC[A-Za-z0-9_-]{22})");
            return m.Success ? m.Groups[1].Value : (url ?? "").TrimEnd('/').ToLowerInvariant();
        }

        public static async Task<bool> IsSubscribedAsync(string channelUrl)
        {
            if (string.IsNullOrEmpty(channelUrl)) return false;
            var key = ChannelKey(channelUrl);
            return (await LoadSubscriptionsAsync()).Subscriptions.Any(s => ChannelKey(s.Url) == key);
        }

        /// <summary>The "Iscriviti" button: adds the channel to the subscriptions (all of them, no folder) or removes
        /// it (from the folders too), saved like an import, so MainPage reloads its lists.</summary>
        public static async Task SetSubscribedAsync(string channelUrl, string name, bool subscribed)
        {
            if (string.IsNullOrEmpty(channelUrl)) return;
            var data = await LoadSubscriptionsAsync();
            var key = ChannelKey(channelUrl);
            var existing = data.Subscriptions.Where(s => ChannelKey(s.Url) == key).ToList();
            if (subscribed)
            {
                if (existing.Count > 0) return;
                data.Subscriptions.Add(new Subscription { ServiceId = 0, Url = channelUrl, Name = name ?? channelUrl });
            }
            else
            {
                if (existing.Count == 0) return;
                foreach (var s in existing) data.Subscriptions.Remove(s);
                foreach (var g in data.FeedGroups) g.SubscriptionUrls.RemoveAll(u => ChannelKey(u) == key);
            }
            await Storage.WriteTextAsync(SubscriptionsFile, data.ToJson());
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

    /// <summary>"Guarda dopo": videos put aside by the user, newest first, until removed by hand.</summary>
    public static class WatchLater
    {
        public sealed class Entry
        {
            public string Url { get; set; }
            public string Title { get; set; }
            public string Uploader { get; set; }
            public long Duration { get; set; } = -1;
            public long AddedAt { get; set; }
        }

        const string FileName = "watchlater.json";
        static List<Entry> list;

        /// <summary>Bumped on every change, so that MainPage redraws its tab when it comes back.</summary>
        public static int Version;

        public static async Task<List<Entry>> AllAsync()
        {
            if (list != null) return list;
            try
            {
                var text = await AppState.Storage.ReadTextAsync(FileName);
                list = text == null ? new List<Entry>() : JsonConvert.DeserializeObject<List<Entry>>(text) ?? new List<Entry>();
            }
            catch (Exception) { list = new List<Entry>(); }
            return list;
        }

        public static async Task<bool> ContainsAsync(string url) => (await AllAsync()).Any(e => e.Url == url);

        public static async Task AddAsync(string url, string title, string uploader, long duration)
        {
            var all = await AllAsync();
            all.RemoveAll(e => e.Url == url);
            all.Insert(0, new Entry
            {
                Url = url,
                Title = title,
                Uploader = uploader,
                Duration = duration,
                AddedAt = (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalMilliseconds
            });
            await SaveAsync();
        }

        public static async Task RemoveAsync(string url)
        {
            if ((await AllAsync()).RemoveAll(e => e.Url == url) > 0) await SaveAsync();
        }

        static async Task SaveAsync()
        {
            Version++;
            try { await AppState.Storage.WriteTextAsync(FileName, JsonConvert.SerializeObject(list)); }
            catch (Exception) { }
        }

        public static PerleVideo ToVideo(Entry e) => new PerleVideo
        {
            Url = e.Url,
            Title = e.Title,
            Uploader = e.Uploader,
            Duration = e.Duration,
            TextualUploadDate = "aggiunto il " + new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddMilliseconds(e.AddedAt).ToLocalTime().ToString("d MMM")
        };

        /// <summary>The menu entry for a video: adds it, or removes it when it is already in the list.</summary>
        public static async Task<MenuFlyoutItem> MenuItemAsync(VideoItem item, Action<string> done)
        {
            bool inList = await ContainsAsync(item.Video.Url);
            var entry = new MenuFlyoutItem { Text = inList ? "Togli da Guarda dopo" : "Guarda dopo" };
            entry.Click += async (s, a) =>
            {
                if (inList) await RemoveAsync(item.Video.Url);
                else await AddAsync(item.Video.Url, item.Title, item.Uploader, item.Video.Duration);
                done?.Invoke(inList ? "Tolto da Guarda dopo: " + item.Title : "Aggiunto a Guarda dopo: " + item.Title);
            };
            return entry;
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
        public long Duration { get; set; } = -1;
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

        /// <summary>Downloads the channel feeds; every 5 channels <paramref name="partial"/> gets the list so far.</summary>
        public static async Task<LatestCache> RefreshAsync(long groupId, Action<int, int> progress, Action<LatestCache> partial = null)
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
                        var durations = await DurationsAsync(s.Url).ConfigureAwait(false);
                        lock (all)
                            foreach (var v in videos)
                            {
                                string url = StreamUrls.FromVideoId(v.VideoId);
                                long duration;
                                if (!durations.TryGetValue(url, out duration)) duration = -1;
                                all.Add(new LatestEntry
                                {
                                    Url = url,
                                    Title = v.Title,
                                    Uploader = v.Uploader,
                                    PublishedTicks = v.Published.UtcTicks,
                                    Duration = duration
                                });
                            }
                    }
                    catch (Exception) { Interlocked.Increment(ref failed); }
                    finally
                    {
                        gate.Release();
                        int now = Interlocked.Increment(ref done);
                        progress?.Invoke(now, subs.Count);
                        if (partial != null && now % 5 == 0 && now < subs.Count)
                            lock (all) partial(Build(all, failed));
                    }
                }).ToList();
                await Task.WhenAll(tasks).ConfigureAwait(false);
            }

            var cache = Build(all, failed);
            try { await AppState.Storage.WriteTextAsync(FileFor(groupId), JsonConvert.SerializeObject(cache)); }
            catch (Exception) { }
            return cache;
        }

        /// <summary>Durations of the first page of the videos tab, by URL: the feed has none. Empty if the tab fails.</summary>
        static async Task<Dictionary<string, long>> DurationsAsync(string channelUrl)
        {
            var durations = new Dictionary<string, long>();
            try
            {
                var page = await YoutubeChannel.GetVideosTabAsync(channelUrl).ConfigureAwait(false);
                foreach (var item in page.Items) durations[item.Url] = item.Duration;
            }
            catch (Exception) { }
            return durations;
        }

        static LatestCache Build(List<LatestEntry> all, int failed) => new LatestCache
        {
            Timestamp = (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalMilliseconds,
            FailedChannels = failed,
            Items = all.OrderByDescending(e => e.PublishedTicks).GroupBy(e => e.Url).Select(g => g.First()).Take(MaxItems).ToList()
        };

        public static PerleVideo ToVideo(LatestEntry e)
        {
            var published = new DateTimeOffset(e.PublishedTicks, TimeSpan.Zero).ToLocalTime();
            return new PerleVideo
            {
                Url = e.Url,
                Title = e.Title,
                Uploader = e.Uploader,
                Duration = e.Duration,
                TextualUploadDate = published.ToString("d MMM yyyy", System.Globalization.CultureInfo.CurrentCulture)
            };
        }
    }
}
