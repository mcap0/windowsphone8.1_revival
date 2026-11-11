// Ports of the Cast unit tests of NewPipe Perle (app/src/test/java/org/schabi/newpipe/cast).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using Perle.Cast;
using Perle.Cast.Proxy;
using Perle.Cast.Ts;

static class T
{
    public static int Failures;
    static string current;

    public static void Run(string name, Action test)
    {
        current = name;
        try { test(); Console.WriteLine("  ok   " + name); }
        catch (Exception e) { Failures++; Console.WriteLine("  FAIL " + name + ": " + e.Message); }
    }

    public static void Eq<TV>(TV expected, TV actual, string what = "")
    {
        if (!EqualityComparer<TV>.Default.Equals(expected, actual))
            throw new Exception(what + " expected <" + expected + "> but was <" + actual + ">");
    }

    public static void Bytes(byte[] expected, byte[] actual, string what = "")
    {
        if (expected == null ? actual != null : actual == null || !expected.SequenceEqual(actual))
            throw new Exception(what + " expected " + Hex(expected) + " but was " + Hex(actual));
    }

    static string Hex(byte[] b) => b == null ? "null" : b.Length > 40 ? b.Length + " bytes" : BitConverter.ToString(b);

    public static void True(bool condition, string what = "") { if (!condition) throw new Exception("expected true: " + what); }

    public static void Throws<TE>(Action a, string what = "") where TE : Exception
    {
        try { a(); } catch (TE) { return; }
        throw new Exception(what + " should have thrown " + typeof(TE).Name);
    }
}

static class CastTests
{
    static bool RangeEq(HttpRange a, HttpRange b) =>
        a == null ? b == null : b != null && a.Start == b.Start && a.End == b.End && a.SuffixLength == b.SuffixLength;

    static void RangeIs(HttpRange expected, HttpRange actual)
    {
        if (!RangeEq(expected, actual)) throw new Exception("expected range " + expected?.ToHeaderValue() + " but was " + actual?.ToHeaderValue());
    }

    public static void HttpRangeTest()
    {
        Console.WriteLine("HttpRangeTest");
        T.Run("parsesSupportedRanges", () =>
        {
            RangeIs(null, HttpRange.Parse(null));
            RangeIs(null, HttpRange.Parse("  "));
            RangeIs(new HttpRange(0, 99), HttpRange.Parse("bytes=0-99"));
            RangeIs(new HttpRange(100, null), HttpRange.Parse("bytes=100-"));
            RangeIs(new HttpRange(null, null, 500), HttpRange.Parse("bytes=-500"));
            RangeIs(new HttpRange(5, 10), HttpRange.Parse(" Bytes = 5 - 10 ".Replace("Bytes = ", "bytes=")));
        });
        T.Run("formatsHeaderValues", () =>
        {
            T.Eq("bytes=0-99", new HttpRange(0, 99).ToHeaderValue());
            T.Eq("bytes=100-", new HttpRange(100, null).ToHeaderValue());
            T.Eq("bytes=-500", new HttpRange(null, null, 500).ToHeaderValue());
        });
        T.Run("rejectsInvalidRanges", () =>
        {
            foreach (var r in new[] { "items=0-1", "bytes=0-1,5-6", "bytes=abc", "bytes=10-5", "bytes=-0", "bytes=-", "bytes=x-" })
                T.Throws<ArgumentException>(() => HttpRange.Parse(r), r);
        });
        T.Run("parsesTotalLengthFromContentRange", () =>
        {
            T.Eq(12345L, HttpRange.TotalLengthFromContentRange("bytes 0-0/12345"));
            T.Eq(-1L, HttpRange.TotalLengthFromContentRange("bytes 0-0/*"));
            T.Eq(-1L, HttpRange.TotalLengthFromContentRange(null));
        });
    }

    public static void YoutubeRangeTest()
    {
        Console.WriteLine("YoutubeRangeTest");
        const string url = "https://rr1.googlevideo.com/videoplayback?expire=1&itag=137&clen=5000";
        T.Run("addsTheRangeParameterToVideoplaybackUrls", () => T.Eq(url + "&range=100-199", YoutubeRange.UrlWithRange(url, new HttpRange(100, 199))));
        T.Run("keepsTheHeaderWhenTheParameterCannotBeUsed", () =>
        {
            T.Eq(null, YoutubeRange.UrlWithRange(url, null));
            T.Eq(null, YoutubeRange.UrlWithRange(url, new HttpRange(100, null)));
            T.Eq(null, YoutubeRange.UrlWithRange("https://example.com/video.mp4", new HttpRange(0, 1)));
        });
        T.Run("buildsTheContentRangeFromClen", () =>
        {
            T.Eq("bytes 100-199/5000", YoutubeRange.ContentRange(url, 100, 100));
            T.Eq("bytes 0-9/*", YoutubeRange.ContentRange("https://x/videoplayback?itag=1", 0, 10));
        });
    }

    public static void LiveTsMuxerTest()
    {
        Console.WriteLine("LiveTsMuxerTest");
        var avc = new CodecConfig.Avc(4, new List<byte[]> { new byte[] { 0x67, 1 } }, new List<byte[]> { new byte[] { 0x68, 2 } });
        byte[] Sample(params byte[][] nals) => nals.SelectMany(n => new byte[] { 0, 0, 0, (byte)n.Length }.Concat(n)).ToArray();
        byte[] AnnexB(params byte[][] nals) => nals.SelectMany(n => new byte[] { 0, 0, 0, 1 }.Concat(n)).ToArray();
        byte[] aud = { 0x09, 0xF0 }, sps = { 0x67, 1 }, pps = { 0x68, 2 }, idr = { 0x65, 7, 7 }, slice = { 0x41, 8 }, sei = { 0x06, 5, 5 };

        T.Run("addsParameterSetsBeforeKeyframes", () =>
        {
            var data = LiveTsMuxer.ToAnnexB(Sample(idr), avc, out bool isKeyframe);
            T.True(isKeyframe);
            T.Bytes(AnnexB(aud, sps, pps, idr), data);
        });
        T.Run("movesTheSeiBeforeTheSlices", () =>
        {
            // as in the first frame of some YouTube videos: [5, 6]
            var data = LiveTsMuxer.ToAnnexB(Sample(idr, sei), avc, out _);
            T.Bytes(AnnexB(aud, sps, pps, sei, idr), data);
        });
        T.Run("dropsDelimitersAndEndOfSequence", () =>
        {
            var data = LiveTsMuxer.ToAnnexB(Sample(aud, slice, new byte[] { 0x0A }), avc, out bool isKeyframe);
            T.True(!isKeyframe);
            T.Bytes(AnnexB(aud, slice), data);
        });
    }

    public static void TsFormatTest()
    {
        Console.WriteLine("TsFormatTest");
        // Part of the Sink list of a Samsung UE43CU7170 (Tizen 2025), which has no video/mp2t.
        var samsung = new List<string>
        {
            "http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;DLNA.ORG_FLAGS=ED100000000000000000000000000000",
            "http-get:*:video/mpeg:DLNA.ORG_PN=MPEG_TS_NA_ISO;DLNA.ORG_FLAGS=ED100000000000000000000000000000",
            "http-get:*:video/vnd.dlna.mpeg-tts:DLNA.ORG_PN=MPEG_TS_SD_NA;DLNA.ORG_FLAGS=ED100000000000000000000000000000",
            "http-get:*:video/vnd.dlna.mpeg-tts:DLNA.ORG_PN=MPEG_TS_HD_NA_T;DLNA.ORG_FLAGS=ED100000000000000000000000000000",
            "http-get:*:video/mpeg:DLNA.ORG_PN=MPEG_TS_HD_KO_ISO;DLNA.ORG_FLAGS=ED100000000000000000000000000000",
            "http-get:*:video/mp4:DLNA.ORG_PN=MPEG4_P2_MP4_SP_AAC;DLNA.ORG_FLAGS=ED100000000000000000000000000000"
        };
        void FormatIs(string mime, string profile, TsFormat f) { T.Eq(mime, f.MimeType); T.Eq(profile, f.Profile); }
        T.Run("neverChoosesTheTimestampedVariant", () => FormatIs("video/mpeg", null, TsFormat.Choose(string.Join(",", samsung))));
        T.Run("prefersAnH264ProfileWithAac", () =>
        {
            var sink = string.Join(",", samsung.Concat(new[]
            {
                "http-get:*:video/vnd.dlna.mpeg-tts:DLNA.ORG_PN=AVC_TS_HD_EU",
                "http-get:*:video/vnd.dlna.mpeg-tts:DLNA.ORG_PN=AVC_TS_HP_HD_AAC_ISO",
                "http-get:*:video/mpeg:DLNA.ORG_PN=AVC_TS_HD_EU_ISO;DLNA.ORG_FLAGS=ED100000000000000000000000000000",
                "http-get:*:video/mpeg:DLNA.ORG_PN=AVC_TS_HP_HD_AAC_ISO;DLNA.ORG_FLAGS=ED100000000000000000000000000000"
            }));
            var format = TsFormat.Choose(sink);
            FormatIs("video/mpeg", "AVC_TS_HP_HD_AAC_ISO", format);
            T.Eq("DLNA.ORG_PN=AVC_TS_HP_HD_AAC_ISO;" + TsFormat.LiveFeatures, format.DlnaFeatures);
        });
        T.Run("prefersMp2tWithoutProfiles", () => FormatIs("video/mp2t", null, TsFormat.Choose("http-get:*:video/mpeg:*,http-get:*:video/mp2t:*")));
        T.Run("defaultsWhenUnknown", () =>
        {
            T.Eq(TsFormat.Default, TsFormat.Choose(null));
            T.Eq(TsFormat.Default, TsFormat.Choose("http-get:*:video/vnd.dlna.mpeg-tts:*"));
            T.Eq(TsFormat.LiveFeatures, TsFormat.Default.DlnaFeatures);
        });
        T.Run("parsesOnlyVideoEntries", () =>
        {
            var parsed = TsFormat.Parse("http-get:*:audio/mpeg:DLNA.ORG_PN=MP3, http-get:*:video/mpeg:DLNA.ORG_PN=MPEG_TS_NA_ISO;X=1,"
                + "http-get:*:VIDEO/x-test:*");
            T.Eq("video/mpeg=MPEG_TS_NA_ISO|video/x-test=", string.Join("|", parsed.Select(p => p.Key + "=" + p.Value)));
        });
    }

    // --- CastProxyServerTest ---

    const string RealUrl = "https://rr1---sn-xyz.googlevideo.com/videoplayback?itag=18&x=1";

    sealed class FakeUpstream : IUpstream
    {
        public readonly byte[] Content = Enumerable.Range(0, 10000).Select(i => (byte)(i % 251)).ToArray();
        public readonly List<HttpRange> Requested = new List<HttpRange>();

        /// <summary>Behaves like googlevideo: honors ranges, answers 416 past the end.</summary>
        public UpstreamResponse Open(string url, HttpRange range)
        {
            lock (Requested) Requested.Add(range);
            if (url != RealUrl) throw new IOException("unexpected url " + url);
            long total = Content.Length;
            if (range == null) return new UpstreamResponse(200, total, null, "video/mp4", new MemoryStream(Content));
            long start = range.Start ?? Math.Max(total - range.SuffixLength.Value, 0);
            long end = Math.Min(range.End ?? total - 1, total - 1);
            if (start >= total) return new UpstreamResponse(416, 0, "bytes */" + total, null, null);
            var slice = Content.Skip((int)start).Take((int)(end - start + 1)).ToArray();
            return new UpstreamResponse(206, slice.Length, "bytes " + start + "-" + end + "/" + total, "video/mp4", new MemoryStream(slice));
        }
    }

    sealed class FailingUpstream : IUpstream
    {
        public UpstreamResponse Open(string url, HttpRange range) => throw new IOException("offline");
    }

    sealed class Response
    {
        public int Code;
        public Dictionary<string, string> Headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        public byte[] Body;
        public string H(string name) => Headers.TryGetValue(name, out var v) ? v : null;
        public long ContentLength => H("Content-Length") is string s ? long.Parse(s) : -1;
    }

    /// <summary>Like HttpURLConnection in the Kotlin test, but on a raw socket so every header is visible.</summary>
    static Response Open(string url, string method = "GET", string range = null)
    {
        var m = Regex.Match(url, @"^http://([^/:]+):(\d+)(/.*)$");
        using (var client = new TcpClient(m.Groups[1].Value, int.Parse(m.Groups[2].Value)))
        {
            var stream = client.GetStream();
            var request = method + " " + m.Groups[3].Value + " HTTP/1.1\r\nHost: x\r\n" + (range != null ? "Range: " + range + "\r\n" : "") + "\r\n";
            var bytes = Encoding.ASCII.GetBytes(request);
            stream.Write(bytes, 0, bytes.Length);
            var all = new MemoryStream();
            stream.CopyTo(all);
            var data = all.ToArray();
            int headerEnd = IndexOf(data, Encoding.ASCII.GetBytes("\r\n\r\n"));
            var lines = Encoding.ASCII.GetString(data, 0, headerEnd).Split(new[] { "\r\n" }, StringSplitOptions.None);
            var r = new Response { Code = int.Parse(lines[0].Split(' ')[1]) };
            foreach (var l in lines.Skip(1)) { int c = l.IndexOf(':'); r.Headers[l.Substring(0, c).Trim()] = l.Substring(c + 1).Trim(); }
            r.Body = data.Skip(headerEnd + 4).ToArray();
            return r;
        }
    }

    static int IndexOf(byte[] data, byte[] pattern)
    {
        for (int i = 0; i + pattern.Length <= data.Length; i++)
            if (!pattern.Where((b, j) => data[i + j] != b).Any()) return i;
        return -1;
    }

    static void WithServer(Action<CastProxyServer, FakeUpstream, string> test)
    {
        var upstream = new FakeUpstream();
        var server = new CastProxyServer(upstream, IPAddress.Loopback);
        server.Start();
        try { test(server, upstream, server.Register(RealUrl, "video/mp4")); }
        finally { server.Stop(); }
    }

    public static void CastProxyServerTest()
    {
        Console.WriteLine("CastProxyServerTest");
        T.Run("registeredUrlIsShortAndHttp", () => WithServer((s, u, url) =>
            T.True(Regex.IsMatch(url, @"^http://127\.0\.0\.1:\d+/v/[0-9a-f]{24}\.mp4$"), url)));
        T.Run("getWithoutRangeReturnsWholeFile", () => WithServer((s, u, url) =>
        {
            var r = Open(url);
            T.Eq(200, r.Code);
            T.Eq("video/mp4", r.H("Content-Type"));
            T.Eq("bytes", r.H("Accept-Ranges"));
            T.Eq((long)u.Content.Length, r.ContentLength);
            T.Eq("Streaming", r.H("transferMode.dlna.org"));
            T.Bytes(u.Content, r.Body);
            T.True(u.Requested.Count == 1 && u.Requested[0] == null, "one request without range");
        }));
        T.Run("getWithRangeReturnsPartialContent", () => WithServer((s, u, url) =>
        {
            var r = Open(url, range: "bytes=100-199");
            T.Eq(206, r.Code);
            T.Eq("bytes 100-199/10000", r.H("Content-Range"));
            T.Eq(100L, r.ContentLength);
            T.Bytes(u.Content.Skip(100).Take(100).ToArray(), r.Body);
            T.True(u.Requested.Count == 1 && RangeEq(u.Requested[0], new HttpRange(100, 199)));
        }));
        T.Run("getWithOpenAndSuffixRanges", () => WithServer((s, u, url) =>
        {
            var open = Open(url, range: "bytes=9990-");
            T.Eq(206, open.Code);
            T.Eq("bytes 9990-9999/10000", open.H("Content-Range"));
            T.Bytes(u.Content.Skip(9990).ToArray(), open.Body);
            var suffix = Open(url, range: "bytes=-5");
            T.Eq(206, suffix.Code);
            T.Eq("bytes 9995-9999/10000", suffix.H("Content-Range"));
            T.Bytes(u.Content.Skip(9995).ToArray(), suffix.Body);
        }));
        T.Run("headReturnsTotalLengthWithoutBody", () => WithServer((s, u, url) =>
        {
            var r = Open(url, "HEAD");
            T.Eq(200, r.Code);
            T.Eq((long)u.Content.Length, r.ContentLength);
            T.Eq("video/mp4", r.H("Content-Type"));
            T.Eq(0, r.Body.Length);
            // only one byte was asked to the upstream to learn the length
            T.True(u.Requested.Count == 1 && RangeEq(u.Requested[0], new HttpRange(0, 0)));
        }));
        T.Run("headWithRangeReturnsPartialHeaders", () => WithServer((s, u, url) =>
        {
            var r = Open(url, "HEAD", "bytes=10-19");
            T.Eq(206, r.Code);
            T.Eq("bytes 10-19/10000", r.H("Content-Range"));
            T.Eq(10L, r.ContentLength);
        }));
        T.Run("rangePastTheEndIsNotSatisfiable", () => WithServer((s, u, url) =>
        {
            var r = Open(url, range: "bytes=20000-");
            T.Eq(416, r.Code);
            T.Eq("bytes */10000", r.H("Content-Range"));
        }));
        T.Run("malformedRangeIsNotSatisfiable", () => WithServer((s, u, url) =>
        {
            T.Eq(416, Open(url, range: "bytes=0-1,4-5").Code);
            T.Eq(0, u.Requested.Count);
        }));
        T.Run("unknownTokenIsNotFound", () => WithServer((s, u, url) =>
        {
            T.Eq(404, Open(Regex.Replace(url, "/v/[0-9a-f]+", "/v/0123456789abcdef01234567")).Code);
            T.Eq(404, Open(url.Substring(0, url.IndexOf("/v/")) + "/other").Code);
        }));
        T.Run("otherMethodsAreNotAllowed", () => WithServer((s, u, url) =>
        {
            using (var client = new TcpClient("127.0.0.1", s.Port))
            {
                var bytes = Encoding.ASCII.GetBytes("POST /v/x.mp4 HTTP/1.1\r\nHost: a\r\n\r\n");
                client.GetStream().Write(bytes, 0, bytes.Length);
                var answer = new StreamReader(client.GetStream()).ReadToEnd();
                T.True(answer.StartsWith("HTTP/1.1 405"), answer);
            }
        }));
        T.Run("upstreamFailureIsBadGateway", () =>
        {
            var failing = new CastProxyServer(new FailingUpstream(), IPAddress.Loopback);
            failing.Start();
            try { T.Eq(502, Open(failing.Register(RealUrl, "video/mp4")).Code); }
            finally { failing.Stop(); }
        });
        T.Run("parsesRequestHeaders", () =>
        {
            var raw = "GET /v/abc.mp4 HTTP/1.1\r\nHost: x\r\nRANGE: bytes=1-2\r\ngetcontentFeatures.dlna.org: 1\r\n\r\n";
            var request = HttpRequest.Read(new MemoryStream(Encoding.ASCII.GetBytes(raw)));
            T.Eq("GET", request.Method);
            T.Eq("/v/abc.mp4", request.Path);
            T.Eq("bytes=1-2", request.Header("Range"));
            T.Eq("1", request.Header("getContentFeatures.dlna.org"));
        });
        T.Run("liveStreamAnswersHeadWithoutLengthAndRefusesRanges", () => WithServer((s, u, url0) =>
        {
            DashTrack Track(string trackUrl) => new DashTrack { Url = trackUrl, MimeType = "video/mp4", Codecs = "avc1", Bandwidth = 1, InitStart = 0, InitEnd = 10, IndexStart = 11, IndexEnd = 20 };
            var url = s.RegisterMux(new CastDash(Track("https://g/v"), Track("https://g/a")), new TsFormat("video/mpeg", "AVC_TS_HD_EU_ISO"));
            T.True(url.EndsWith(".ts"));
            var head = Open(url, "HEAD");
            T.Eq(200, head.Code);
            T.Eq("video/mpeg", head.H("Content-Type"));
            T.Eq(-1L, head.ContentLength);
            T.Eq("DLNA.ORG_PN=AVC_TS_HD_EU_ISO;" + TsFormat.LiveFeatures, head.H("contentFeatures.dlna.org"));
            T.Eq(416, Open(url, range: "bytes=100-").Code);
            // HEAD does not download anything
            T.Eq(0, u.Requested.Count);
        }));
        T.Run("parsesStartTime", () =>
        {
            T.Eq(0.0, CastProxyServer.StartSeconds("/v/abc.ts"));
            T.Eq(125.0, CastProxyServer.StartSeconds("/v/abc.ts?t=125"));
            T.Eq(0.0, CastProxyServer.StartSeconds("/v/abc.ts?t=-4"));
        });
        T.Run("servesContentAndAnnouncesCaptions", () => WithServer((s, u, url) =>
        {
            var srt = Encoding.UTF8.GetBytes("1\n00:00:01,000 --> 00:00:02,000\nHi\n\n");
            var subtitleUrl = s.RegisterContent(srt, "text/srt", "srt");
            T.True(subtitleUrl.EndsWith(".srt"));
            var subtitles = Open(subtitleUrl);
            T.Eq(200, subtitles.Code);
            T.Eq("text/srt", subtitles.H("Content-Type"));
            T.Bytes(srt, subtitles.Body);
            var video = Open(s.Register(RealUrl, "video/mp4", subtitleUrl), "HEAD");
            T.Eq(200, video.Code);
            T.Eq(subtitleUrl, video.H("CaptionInfo.sec"));
            T.Eq(null, Open(url, "HEAD").H("CaptionInfo.sec"));
        }));
    }

    public static int RunAll()
    {
        HttpRangeTest();
        YoutubeRangeTest();
        LiveTsMuxerTest();
        TsFormatTest();
        CastProxyServerTest();
        return T.Failures;
    }
}
