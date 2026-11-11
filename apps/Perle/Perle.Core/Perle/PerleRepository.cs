/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.local.feed.perle.PerleRepository and of the state building of
 * PerleViewModel. Storage is abstracted so that the same code runs in the app and in the tests.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Perle.Extractor;

namespace Perle
{
    /// <summary>Text files in the app's local folder (ApplicationData.LocalFolder on the phone).</summary>
    public interface IStorage
    {
        Task<string> ReadTextAsync(string name);   // null if missing
        Task WriteTextAsync(string name, string text);
        Task DeleteAsync(string name);
    }

    public sealed class PerleVideo
    {
        public int ServiceId { get; set; }
        public string Url { get; set; }
        public string Title { get; set; }
        public long Duration { get; set; }
        public string Uploader { get; set; }
        public string UploaderUrl { get; set; }
        public string ThumbnailUrl { get; set; }
        public long? ViewCount { get; set; }
        public string TextualUploadDate { get; set; }

        public static PerleVideo From(StreamInfoItem item) => new PerleVideo
        {
            ServiceId = 0,
            Url = item.Url,
            Title = item.Name ?? "",
            Duration = item.Duration,
            Uploader = item.UploaderName ?? "",
            UploaderUrl = item.UploaderUrl,
            ThumbnailUrl = item.ThumbnailUrl,
            ViewCount = item.ViewCount >= 0 ? item.ViewCount : (long?)null,
            TextualUploadDate = item.TextualUploadDate
        };
    }

    public sealed class PerleCache
    {
        public long Timestamp { get; set; }
        public List<List<PerleVideo>> Channels { get; set; } = new List<List<PerleVideo>>();
        public int FailedChannels { get; set; }
    }

    /// <summary>Loads the most viewed videos of every channel of a feed group and caches them on disk.</summary>
    public sealed class PerleRepository
    {
        public const int MaxConcurrentChannels = 4;
        public const int MaxPagesPerChannel = 5;
        public static readonly TimeSpan CacheDuration = TimeSpan.FromDays(7);

        const string HiddenFile = "perle_hidden.json";
        const string HistoryFile = "history.json";

        readonly IStorage storage;
        HashSet<string> hidden;

        public PerleRepository(IStorage storage) { this.storage = storage; }

        static string CacheFile(long groupId) => "perle_group_" + groupId + ".json";

        static readonly DateTime UnixEpoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);

        // DateTimeOffset.ToUnixTimeMilliseconds is missing from the Windows Phone 8.1 profile
        static long Now() => (long)(DateTime.UtcNow - UnixEpoch).TotalMilliseconds;

        public async Task<PerleCache> ReadCacheAsync(long groupId)
        {
            try
            {
                var text = await storage.ReadTextAsync(CacheFile(groupId)).ConfigureAwait(false);
                return text == null ? null : JsonConvert.DeserializeObject<PerleCache>(text);
            }
            catch (Exception) { return null; }
        }

        public bool IsFresh(PerleCache cache) => Now() - cache.Timestamp < (long)CacheDuration.TotalMilliseconds;

        /// <summary>
        /// Downloads the videos of every channel in the group, at most <see cref="MaxConcurrentChannels"/> at a
        /// time. Channels which fail to load are skipped and counted in <see cref="PerleCache.FailedChannels"/>.
        /// </summary>
        public async Task<PerleCache> FetchAsync(IList<Subscription> subscriptions, long groupId,
            Action<int, int> onProgress, CancellationToken ct = default(CancellationToken),
            Action<PerleCache> onPartial = null, int partialEvery = 5)
        {
            int done = 0, failed = 0, total = subscriptions.Count;
            onProgress?.Invoke(0, total);
            var results = new List<List<PerleVideo>>();
            using (var gate = new SemaphoreSlim(MaxConcurrentChannels))
            {
                var tasks = subscriptions.Select(async s =>
                {
                    await gate.WaitAsync(ct).ConfigureAwait(false);
                    try
                    {
                        var videos = await LoadChannelAsync(s, ct).ConfigureAwait(false);
                        if (videos.Count > 0) lock (results) results.Add(videos);
                    }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception) { Interlocked.Increment(ref failed); }
                    finally
                    {
                        gate.Release();
                        int d = Interlocked.Increment(ref done);
                        onProgress?.Invoke(d, total);
                        // show what is there every few channels instead of waiting for all of them
                        if (onPartial != null && d < total && d % partialEvery == 0)
                        {
                            List<List<PerleVideo>> snapshot;
                            lock (results) snapshot = results.ToList();
                            onPartial(new PerleCache { Timestamp = Now(), Channels = snapshot, FailedChannels = failed });
                        }
                    }
                }).ToList();
                await Task.WhenAll(tasks).ConfigureAwait(false);
            }
            var cache = new PerleCache { Timestamp = Now(), Channels = results, FailedChannels = failed };
            try { await storage.WriteTextAsync(CacheFile(groupId), JsonConvert.SerializeObject(cache)).ConfigureAwait(false); }
            catch (Exception) { /* like writeCache: a failed write only loses the cache */ }
            return cache;
        }

        /// <summary>
        /// YouTube's channel tab has no popularity sort in NewPipeExtractor, so like withPopularSort
        /// returning null this reads up to <see cref="MaxPagesPerChannel"/> pages of the videos tab.
        /// </summary>
        public static async Task<List<PerleVideo>> LoadChannelAsync(Subscription subscription, CancellationToken ct)
        {
            var page = await YoutubeChannel.GetVideosTabAsync(subscription.Url, ct).ConfigureAwait(false);
            var items = new List<StreamInfoItem>(page.Items);
            int pages = 1;
            while (pages < MaxPagesPerChannel && page.NextPage != null)
            {
                page = await YoutubeChannel.GetMoreItemsAsync(page, ct).ConfigureAwait(false);
                items.AddRange(page.Items);
                pages++;
            }
            return PerleSelection.SelectTopVideos(items).Select(PerleVideo.From).ToList();
        }

        public async Task<HashSet<string>> HiddenUrlsAsync()
        {
            if (hidden != null) return hidden;
            var text = await storage.ReadTextAsync(HiddenFile).ConfigureAwait(false);
            hidden = text == null ? new HashSet<string>() : JsonConvert.DeserializeObject<HashSet<string>>(text) ?? new HashSet<string>();
            return hidden;
        }

        public async Task SetHiddenAsync(string url, bool isHidden)
        {
            var urls = await HiddenUrlsAsync().ConfigureAwait(false);
            if (isHidden) urls.Add(url); else urls.Remove(url);
            await storage.WriteTextAsync(HiddenFile, JsonConvert.SerializeObject(urls)).ConfigureAwait(false);
        }

        public async Task ClearHiddenAsync()
        {
            hidden = new HashSet<string>();
            await storage.DeleteAsync(HiddenFile).ConfigureAwait(false);
        }

        /// <summary>The urls of all the streams in the watch history.</summary>
        public async Task<HashSet<string>> WatchedUrlsAsync()
        {
            var text = await storage.ReadTextAsync(HistoryFile).ConfigureAwait(false);
            return text == null ? new HashSet<string>() : JsonConvert.DeserializeObject<HashSet<string>>(text) ?? new HashSet<string>();
        }

        public async Task AddToHistoryAsync(string url)
        {
            var urls = await WatchedUrlsAsync().ConfigureAwait(false);
            if (urls.Add(url)) await storage.WriteTextAsync(HistoryFile, JsonConvert.SerializeObject(urls)).ConfigureAwait(false);
        }

        /// <summary>Port of PerleViewModel.buildState: drops watched and hidden videos, then interleaves.</summary>
        public async Task<List<PerleVideo>> BuildListAsync(PerleCache cache, Random random = null)
        {
            var excluded = new HashSet<string>(await WatchedUrlsAsync().ConfigureAwait(false));
            excluded.UnionWith(await HiddenUrlsAsync().ConfigureAwait(false));
            var channels = cache.Channels.Select(v => (IList<PerleVideo>)v.Where(x => !excluded.Contains(x.Url)).ToList());
            return PerleSelection.Interleave(channels, random);
        }
    }
}
