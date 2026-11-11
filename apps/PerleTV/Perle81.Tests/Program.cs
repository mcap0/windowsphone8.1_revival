/*
 * Console runner for Perle81 shared unit tests.
 */
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Threading.Tasks;
using Perle;
using Perle.Extractor;

static class Program
{
    static int failures;

    static void Check(bool condition, string what)
    {
        Console.WriteLine((condition ? "  ok   " : "  FAIL ") + what);
        if (!condition) failures++;
    }

    static StreamInfoItem Item(string id, long views, long duration = 600,
        StreamType type = StreamType.VideoStream, bool shorts = false) =>
        new StreamInfoItem
        {
            Url = "https://example.com/" + id,
            Name = id,
            ViewCount = views,
            Duration = duration,
            StreamType = type,
            IsShortFormContent = shorts
        };

    static void UnitTests()
    {
        Console.WriteLine("PerleSelection");
        var items = new[]
        {
            Item("a", 10), Item("b", 500), Item("short", 9999, shorts: true),
            Item("live", 9999, type: StreamType.LiveStream),
            Item("pastLive", 9999, type: StreamType.PostLiveStream),
            Item("tooShort", 9999, duration: 119), Item("c", 300), Item("b", 500)
        };
        var selected = PerleSelection.SelectTopVideos(items, 2).Select(i => i.Name).ToList();
        Check(selected.SequenceEqual(new[] { "b", "c" }),
            "selectTopVideosKeepsMostViewedRegularVideos");

        var big = Enumerable.Range(1, 10).Select(i => "big" + i).ToList();
        var small = new List<string> { "small1", "small2" };
        var other = new List<string> { "other1", "other2", "other3" };
        bool ok = true;
        for (int seed = 0; seed < 20; seed++)
        {
            var result = PerleSelection.Interleave(
                new IList<string>[] { big, small, other, new List<string>() }, new Random(seed));
            ok &= result.Count == big.Count + small.Count + other.Count;
            ok &= new HashSet<string>(result).SetEquals(big.Concat(small).Concat(other));
            ok &= new HashSet<string>(result.Take(3).Select(s =>
                new string(s.TakeWhile(char.IsLetter).ToArray())))
                .SetEquals(new[] { "big", "small", "other" });
            ok &= small.All(result.Take(6).Contains);
        }
        Check(ok, "interleaveGivesEveryChannelTheSameWeight");
        Check(PerleSelection.Interleave(new List<IList<string>>()).Count == 0,
            "interleaveOfNothingIsEmpty");

        Console.WriteLine("Parsing");
        Check(LockupExtractor.ParseDurationString("40:44") == 2444, "duration 40:44");
        Check(LockupExtractor.ParseDurationString("1:02:03") == 3723, "duration 1:02:03");
        Check(LockupExtractor.MixedNumberWordToLong("2m views") == 2000000, "2m views");
        Check(LockupExtractor.MixedNumberWordToLong("12K views") == 12000, "12K views");
        Check(LockupExtractor.MixedNumberWordToLong("1.5M views") == 1500000, "1.5M views");
        Check(LockupExtractor.MixedNumberWordToLong("950 views") == 950, "950 views");

    }

    sealed class MemoryStorage : IStorage
    {
        readonly Dictionary<string, string> files = new Dictionary<string, string>();
        public Task<string> ReadTextAsync(string name) =>
            Task.FromResult(files.TryGetValue(name, out var text) ? text : null);
        public Task WriteTextAsync(string name, string text)
        {
            files[name] = text;
            return Task.CompletedTask;
        }
        public Task DeleteAsync(string name)
        {
            files.Remove(name);
            return Task.CompletedTask;
        }
    }

    static async Task LiveTests(string subscriptionsPath, string groupName)
    {
        Console.WriteLine("Live: subscriptions");
        var data = SubscriptionsData.Parse(File.ReadAllText(subscriptionsPath));
        Check(data.Subscriptions.Count > 0,
            data.Subscriptions.Count + " subscriptions, " + data.FeedGroups.Count + " folders");
        var group = data.FeedGroups.First(g => g.Name == groupName);
        var subs = data.SubscriptionsFor(group.Id);
        Check(subs.Count == group.SubscriptionUrls.Count,
            "folder '" + group.Name + "': " + subs.Count + " channels");

        Console.WriteLine("Live: channel videos tab");
        var page = await YoutubeChannel.GetVideosTabAsync(subs[0].Url);
        Check(page.Items.Count > 0, page.ChannelName + ": " + page.Items.Count + " videos on the first page");
        var first = page.Items[0];
        Check(first.Duration > 0 && first.ViewCount >= 0 && first.ThumbnailUrl != null,
            "first: '" + first.Name + "' " + first.Duration + "s " + first.ViewCount + " views, " + first.TextualUploadDate);
        Check(page.NextPage != null, "has a continuation");
        var more = await YoutubeChannel.GetMoreItemsAsync(page);
        Check(more.Items.Count > 0, "second page: " + more.Items.Count + " videos");

        Console.WriteLine("Live: Perle for the folder");
        var repo = new PerleRepository(new MemoryStorage());
        var stopwatch = System.Diagnostics.Stopwatch.StartNew();
        var cache = await repo.FetchAsync(subs, group.Id, null);
        Check(cache.FailedChannels == 0 && cache.Channels.Count > 0,
            cache.Channels.Count + " channels loaded, " + cache.FailedChannels + " failed, " +
            stopwatch.Elapsed.TotalSeconds.ToString("0.0") + "s");
        var list = await repo.BuildListAsync(cache);
        foreach (var video in list.Take(8))
            Console.WriteLine("       " + video.Uploader + " — " + video.Title + " (" + video.ViewCount + ")");
        Check((await repo.ReadCacheAsync(group.Id)) != null, "cache written");

        Console.WriteLine("Live: stream info (VISIONOS)");
        var info = await YoutubeStream.GetInfoAsync(list[0].Url);
        var videoFormat = YoutubeStream.BestVideo(info, 720);
        var audioFormat = YoutubeStream.BestAudio(info);
        Check(videoFormat != null && audioFormat != null,
            "H.264 " + videoFormat?.QualityLabel + " itag " + videoFormat?.Itag +
            " + AAC itag " + audioFormat?.Itag + ", HLS " + (info.HlsUrl != null));
        using (var request = new HttpRequestMessage(HttpMethod.Get, videoFormat.Url))
        {
            request.Headers.TryAddWithoutValidation("User-Agent", Innertube.VisionOsUserAgent);
            request.Headers.TryAddWithoutValidation("Range", "bytes=0-65535");
            var response = await Innertube.Http.SendAsync(request);
            Check((int)response.StatusCode == 206, "video URL answers " + (int)response.StatusCode);
        }
    }

    static async Task<int> Main(string[] args)
    {
        UnitTests();
        if (args.Length >= 2) await LiveTests(args[0], args[1]);
        Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        return failures == 0 ? 0 : 1;
    }
}
