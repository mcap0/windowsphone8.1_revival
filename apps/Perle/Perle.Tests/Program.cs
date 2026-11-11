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

    static StreamInfoItem Item(string id, long views, long duration = 600, StreamType type = StreamType.VideoStream, bool shorts = false) =>
        new StreamInfoItem { Url = "https://example.com/" + id, Name = id, ViewCount = views, Duration = duration, StreamType = type, IsShortFormContent = shorts };

    // Ported from PerleSelectionTest.kt
    static void UnitTests()
    {
        Console.WriteLine("PerleSelection");
        var items = new[]
        {
            Item("a", 10), Item("b", 500), Item("short", 9999, shorts: true), Item("live", 9999, type: StreamType.LiveStream),
            Item("pastLive", 9999, type: StreamType.PostLiveStream), Item("tooShort", 9999, duration: 119), Item("c", 300), Item("b", 500)
        };
        var selected = PerleSelection.SelectTopVideos(items, 2).Select(i => i.Name).ToList();
        Check(selected.SequenceEqual(new[] { "b", "c" }), "selectTopVideosKeepsMostViewedRegularVideos");

        var big = Enumerable.Range(1, 10).Select(i => "big" + i).ToList();
        var small = new List<string> { "small1", "small2" };
        var other = new List<string> { "other1", "other2", "other3" };
        bool ok = true;
        for (int seed = 0; seed < 20; seed++)
        {
            var result = PerleSelection.Interleave(new IList<string>[] { big, small, other, new List<string>() }, new Random(seed));
            ok &= result.Count == big.Count + small.Count + other.Count;
            ok &= new HashSet<string>(result).SetEquals(big.Concat(small).Concat(other));
            ok &= new HashSet<string>(result.Take(3).Select(s => new string(s.TakeWhile(char.IsLetter).ToArray()))).SetEquals(new[] { "big", "small", "other" });
            ok &= small.All(result.Take(6).Contains);
        }
        Check(ok, "interleaveGivesEveryChannelTheSameWeight");
        Check(PerleSelection.Interleave(new List<IList<string>>()).Count == 0, "interleaveOfNothingIsEmpty");

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
        public Task<string> ReadTextAsync(string name) => Task.FromResult(files.TryGetValue(name, out var t) ? t : null);
        public Task WriteTextAsync(string name, string text) { files[name] = text; return Task.CompletedTask; }
        public Task DeleteAsync(string name) { files.Remove(name); return Task.CompletedTask; }
    }

    static async Task LiveTests(string subscriptionsPath, string groupName)
    {
        Console.WriteLine("Live: subscriptions");
        var data = SubscriptionsData.Parse(File.ReadAllText(subscriptionsPath));
        Check(data.Subscriptions.Count > 0, data.Subscriptions.Count + " subscriptions, " + data.FeedGroups.Count + " folders");
        var group = data.FeedGroups.First(g => g.Name == groupName);
        var subs = data.SubscriptionsFor(group.Id);
        Check(subs.Count == group.SubscriptionUrls.Count, "folder '" + group.Name + "': " + subs.Count + " channels");

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
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var cache = await repo.FetchAsync(subs, group.Id, null);
        Check(cache.FailedChannels == 0 && cache.Channels.Count > 0,
            cache.Channels.Count + " channels loaded, " + cache.FailedChannels + " failed, " + sw.Elapsed.TotalSeconds.ToString("0.0") + "s");
        var list = await repo.BuildListAsync(cache);
        foreach (var v in list.Take(8)) Console.WriteLine("       " + v.Uploader + " — " + v.Title + " (" + v.ViewCount + ")");
        Check((await repo.ReadCacheAsync(group.Id)) != null, "cache written");

        Console.WriteLine("Live: stream info (VISIONOS)");
        var info = await YoutubeStream.GetInfoAsync(list[0].Url);
        var video = YoutubeStream.BestVideo(info, 720);
        var audio = YoutubeStream.BestAudio(info);
        Check(video != null && audio != null, "H.264 " + video?.QualityLabel + " itag " + video?.Itag + " + AAC itag " + audio?.Itag + ", HLS " + (info.HlsUrl != null));
        using (var req = new HttpRequestMessage(HttpMethod.Get, video.Url))
        {
            req.Headers.TryAddWithoutValidation("User-Agent", Innertube.VisionOsUserAgent);
            req.Headers.TryAddWithoutValidation("Range", "bytes=0-65535");
            var resp = await Innertube.Http.SendAsync(req);
            Check((int)resp.StatusCode == 206, "video URL answers " + (int)resp.StatusCode);
        }
    }

    /// <summary>Stops the muxer after a number of bytes, like a TV closing the connection.</summary>
    sealed class LimitedFile : FileStream
    {
        readonly long limit;
        public LimitedFile(string path, long limit) : base(path, FileMode.Create) { this.limit = limit; }
        public override void Write(byte[] buffer, int offset, int count)
        {
            if (Position + count > limit) throw new IOException("limit reached");
            base.Write(buffer, offset, count);
        }
    }

    static async Task MuxTest(string videoId, string outPath, long maxBytes, double startSeconds)
    {
        Console.WriteLine("Live: TS mux of " + videoId + " from " + startSeconds + " s");
        Perle.Cast.CastLog.Sink = (level, message, error) => Console.WriteLine("       " + level + " " + message);
        var info = await YoutubeStream.GetInfoAsync(videoId);
        var video = YoutubeStream.BestVideo(info, 720);
        var audio = YoutubeStream.BestAudio(info);
        var dash = new Perle.Cast.CastDash(Perle.Cast.DashTrack.From(video), Perle.Cast.DashTrack.From(audio));
        var muxer = new Perle.Cast.Ts.LiveTsMuxer(new Perle.Cast.Proxy.HttpUpstream(), dash);
        using (var file = new LimitedFile(outPath, maxBytes))
        {
            try { muxer.Write(file, startSeconds); } catch (IOException e) when (e.Message == "limit reached") { }
        }
        Check(new FileInfo(outPath).Length > 1000000, "wrote " + new FileInfo(outPath).Length / 1024 + " KB");
    }

    static async Task<int> Main(string[] args)
    {
        if (args.Length >= 1 && args[0] == "mux")
        {
            await MuxTest(args[1], args[2], long.Parse(args[3]), double.Parse(args[4], System.Globalization.CultureInfo.InvariantCulture));
            Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
            return failures;
        }
        if (args.Length >= 2 && args[0] == "castitem")
        {
            // live check of what Cast to TV gets for a video: qualities, audio tracks, subtitles
            var info = await Perle.Extractor.YoutubeStream.GetInfoAsync(args[1]);
            var item = new Perle.Cast.CastItem(info);
            Console.WriteLine("castable " + item.IsCastable + ", qualities " + string.Join(",", item.Qualities)
                + ", audio tracks " + string.Join(" | ", item.AudioTrackIds.Select(item.AudioTrackName)));
            Console.WriteLine("subtitles: " + string.Join(" | ", item.Subtitles.Select(Perle.Cast.CastItem.SubtitleName)));
            var choice = item.Choose(null, null);
            Console.WriteLine("auto choice: " + (choice.Single != null ? choice.Single.Height + "p progressive" : choice.Dash.Video.Height + "p muxed " + choice.Dash.Audio.Codecs));
            var first = item.FindSubtitles("it|false") ?? item.FindSubtitles("en|false") ?? item.Subtitles.FirstOrDefault();
            if (first != null)
            {
                var vtt = Perle.Cast.CastService.DownloadText(first.UrlFor(Perle.Cast.CastItem.SubtitleFormat));
                var srt = Perle.Cast.Dlna.SrtConverter.Convert(vtt, "vtt");
                Console.WriteLine("subtitles " + first.LanguageTag + ": " + vtt.Length + " chars of VTT -> " + (srt?.Length ?? -1) + " chars of SRT");
                Console.WriteLine(srt == null ? "" : string.Join("\n", srt.Split('\n').Take(8)));
            }
            return 0;
        }
        UnitTests();
        CastTests.RunAll();
        DlnaTests.RunAll();
        failures += T.Failures;
        if (args.Length >= 2) await LiveTests(args[0], args[1]);
        Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
        return failures == 0 ? 0 : 1;
    }
}
