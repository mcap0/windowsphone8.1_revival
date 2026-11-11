/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * HLS player for WP8.1, whose MediaElement does not open HLS: reads YouTube's master playlist (VISIONOS client),
 * picks the best H.264 rendition up to maxHeight and its AAC audio rendition, downloads the segments of both
 * (they are aligned) and feeds the demuxed samples (Perle81.Native.HlsSegment) to a MediaStreamSource.
 * A seek restarts both downloads at the segment that contains the new position.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using Perle81.Native;
using Windows.Media.Core;
using Windows.Media.MediaProperties;

namespace Perle81.App
{
    public sealed class AudioOption
    {
        public string Name { get; set; }
        public string Language { get; set; }
        public string Uri { get; set; }
        public bool IsDefault { get; set; }
    }

    public sealed class HlsSource
    {
        public MediaStreamSource Source { get; private set; }
        public string Label { get; private set; }
        public IList<AudioOption> AudioOptions { get; private set; }
        public AudioOption SelectedAudioOption { get; private set; }

        /// <summary>Last download or demux error, for the page (raised on a worker thread).</summary>
        public event Action<string> Failed;

        static readonly HttpClient http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };

        Track video, audio;
        VideoStreamDescriptor videoDesc;
        int generation;

        HlsSource()
        {
            AudioOptions = new List<AudioOption>();
        }

        public static async Task<HlsSource> OpenAsync(string masterUrl, int maxHeight, string preferredLanguage = null)
        {
            var src = new HlsSource();
            string master = await http.GetStringAsync(masterUrl);
            var variant = Playlists.PickVariant(master, masterUrl, maxHeight);
            if (variant == null) throw new InvalidOperationException("nessuna variante H.264 nell'HLS");

            List<AudioOption> options = Playlists.AudioOptions(master, masterUrl, variant);
            src.AudioOptions = options;
            src.SelectedAudioOption = Playlists.PickAudioOption(options, preferredLanguage);
            string audioUrl = src.SelectedAudioOption == null
                ? Playlists.AudioUri(master, masterUrl, variant.AudioGroup)
                : src.SelectedAudioOption.Uri;

            src.video = new Track(src, Playlists.Segments(await http.GetStringAsync(variant.Uri), variant.Uri), true);
            if (audioUrl != null)
                src.audio = new Track(src, Playlists.Segments(await http.GetStringAsync(audioUrl), audioUrl), false);
            if (src.video.Segments.Count == 0) throw new InvalidOperationException("playlist video vuota");

            var vp = VideoEncodingProperties.CreateH264();
            vp.Width = (uint)variant.Width;
            vp.Height = (uint)variant.Height;
            src.videoDesc = new VideoStreamDescriptor(vp);

            MediaStreamSource mss;
            if (src.audio != null && src.audio.Segments.Count > 0)
            {
                // the first audio segment gives sample rate and channels; its samples are kept for the start
                HlsSegment first = await src.audio.FetchAsync(0);
                var ap = AudioEncodingProperties.CreateAacAdts((uint)first.SampleRate, (uint)Math.Max(1, first.Channels), 128000);
                mss = new MediaStreamSource(src.videoDesc, new AudioStreamDescriptor(ap));
            }
            else
            {
                src.audio = null;
                mss = new MediaStreamSource(src.videoDesc);
            }
            mss.CanSeek = true;
            mss.Duration = TimeSpan.FromTicks(src.video.TotalTicks);
            mss.Starting += src.OnStarting;
            mss.SampleRequested += src.OnSampleRequested;
            mss.Closed += (s, e) => src.Stop();
            src.Source = mss;
            src.Label = variant.Height + "p HLS (lettore Perle)";
            return src;
        }

        void OnStarting(MediaStreamSource sender, MediaStreamSourceStartingEventArgs args)
        {
            var req = args.Request;
            if (req.StartPosition.HasValue)
            {
                int k = video.IndexAt(req.StartPosition.Value.Ticks);
                Restart(k);
                req.SetActualStartPosition(TimeSpan.FromTicks(video.StartOf(k)));
            }
            else if (!video.Started)
            {
                Restart(0);
                req.SetActualStartPosition(TimeSpan.Zero);
            }
        }

        void Restart(int index)
        {
            int gen = Interlocked.Increment(ref generation);
            video.Start(index, gen);
            if (audio != null) audio.Start(index, gen);
        }

        void Stop()
        {
            Interlocked.Increment(ref generation);
            video.Wake();
            if (audio != null) audio.Wake();
        }

        async void OnSampleRequested(MediaStreamSource sender, MediaStreamSourceSampleRequestedEventArgs args)
        {
            var req = args.Request;
            var deferral = req.GetDeferral();
            try
            {
                Track t = req.StreamDescriptor == videoDesc ? video : audio;
                req.Sample = t == null ? null : await t.NextAsync();
            }
            catch (Exception ex)
            {
                req.Sample = null;
                Report(ex.Message);
            }
            deferral.Complete();
        }

        void Report(string message)
        {
            var f = Failed;
            if (f != null) f(message);
        }

        // ---- one rendition (video or audio): download loop and sample queue --------------------------

        sealed class Track
        {
            const int MaxQueued = 3;   // segments demuxed ahead of playback

            readonly HlsSource owner;
            readonly bool isVideo;
            public readonly List<Playlists.Segment> Segments;
            readonly long[] starts;
            public long TotalTicks { get; private set; }
            public bool Started { get; private set; }

            readonly object gate = new object();
            readonly Queue<Queue<MediaStreamSample>> queue = new Queue<Queue<MediaStreamSample>>();
            bool ended;
            int gen;
            TaskCompletionSource<bool> dataReady = new TaskCompletionSource<bool>();
            TaskCompletionSource<bool> spaceFree = new TaskCompletionSource<bool>();
            HlsSegment prefetched;  // segment 0 read by OpenAsync

            public Track(HlsSource owner, List<Playlists.Segment> segments, bool isVideo)
            {
                this.owner = owner;
                this.isVideo = isVideo;
                Segments = segments;
                starts = new long[segments.Count + 1];
                for (int i = 0; i < segments.Count; i++) starts[i + 1] = starts[i] + segments[i].Ticks;
                TotalTicks = starts[segments.Count];
            }

            public long StartOf(int index) { return starts[Math.Max(0, Math.Min(index, Segments.Count - 1))]; }

            public int IndexAt(long ticks)
            {
                for (int i = Segments.Count - 1; i > 0; i--) if (starts[i] <= ticks) return i;
                return 0;
            }

            public async Task<HlsSegment> FetchAsync(int index)
            {
                if (index == 0 && prefetched != null) { var p = prefetched; prefetched = null; return p; }
                string url = Segments[index].Uri;
                Exception last = null;
                for (int attempt = 0; attempt < 3; attempt++)
                {
                    try
                    {
                        byte[] data = await http.GetByteArrayAsync(url);
                        HlsSegment seg = await Task.Run(() => isVideo ? HlsSegment.ParseVideo(data) : HlsSegment.ParseAudio(data));
                        if (!seg.Ok) throw new InvalidOperationException("segmento " + index + ": " + seg.Error);
                        if (index == 0 && !isVideo && !Started) prefetched = seg;
                        return seg;
                    }
                    catch (Exception ex) { last = ex; }
                    await Task.Delay(1000 * (attempt + 1));
                }
                throw new InvalidOperationException((isVideo ? "video" : "audio") + ", segmento " + index + ": " + last.Message);
            }

            public void Start(int index, int generation)
            {
                lock (gate)
                {
                    gen = generation;
                    queue.Clear();
                    ended = false;
                    Started = true;
                }
                Wake();
                var ignored = LoopAsync(index, generation);
            }

            /// <summary>Wakes every waiter (new generation or stop): they re-check their state.</summary>
            public void Wake()
            {
                TaskCompletionSource<bool> d, s;
                lock (gate)
                {
                    d = dataReady; dataReady = new TaskCompletionSource<bool>();
                    s = spaceFree; spaceFree = new TaskCompletionSource<bool>();
                }
                d.TrySetResult(true);
                s.TrySetResult(true);
            }

            async Task LoopAsync(int index, int generation)
            {
                for (int i = index; i < Segments.Count; i++)
                {
                    // wait for room in the queue
                    while (true)
                    {
                        Task wait;
                        lock (gate)
                        {
                            if (gen != generation || owner.generation != generation) return;
                            if (queue.Count < MaxQueued) break;
                            wait = spaceFree.Task;
                        }
                        await wait;
                    }
                    HlsSegment seg;
                    try { seg = await FetchAsync(i); }
                    catch (Exception ex)
                    {
                        if (gen == generation) owner.Report(ex.Message);
                        break;
                    }
                    TaskCompletionSource<bool> ready;
                    lock (gate)
                    {
                        if (gen != generation) return;
                        queue.Enqueue(new Queue<MediaStreamSample>(seg.Samples));
                        ready = dataReady; dataReady = new TaskCompletionSource<bool>();
                    }
                    ready.TrySetResult(true);
                }
                TaskCompletionSource<bool> done;
                lock (gate)
                {
                    if (gen != generation) return;
                    ended = true;
                    done = dataReady; dataReady = new TaskCompletionSource<bool>();
                }
                done.TrySetResult(true);
            }

            /// <summary>Next sample in decode order, or null at the end of the stream.</summary>
            public async Task<MediaStreamSample> NextAsync()
            {
                while (true)
                {
                    Task wait;
                    TaskCompletionSource<bool> freed = null;
                    lock (gate)
                    {
                        while (queue.Count > 0 && queue.Peek().Count == 0)
                        {
                            queue.Dequeue();
                            freed = spaceFree; spaceFree = new TaskCompletionSource<bool>();
                        }
                        if (queue.Count > 0)
                        {
                            var s = queue.Peek().Dequeue();
                            if (freed != null) freed.TrySetResult(true);
                            return s;
                        }
                        if (ended || owner.generation != gen) return null;
                        wait = dataReady.Task;
                    }
                    if (freed != null) freed.TrySetResult(true);
                    await wait;
                }
            }
        }
    }

    /// <summary>The few parts of HLS playlists that YouTube uses.</summary>
    public static class Playlists
    {
        public sealed class Variant
        {
            public string Uri, AudioGroup;
            public string VideoCodecFamily, AudioCodecFamily;
            public int Width, Height;
            public long Bandwidth;
        }

        public sealed class Segment
        {
            public string Uri;
            public long Ticks;
        }

        public static Variant PickVariant(string master, string baseUrl, int maxHeight)
        {
            return Variants(master, baseUrl, maxHeight)
                .OrderByDescending(v => v.Height).ThenBy(v => v.Bandwidth).FirstOrDefault();
        }

        public static List<AudioOption> AudioOptions(string master, string baseUrl, Variant chosen)
        {
            var groups = new HashSet<string>();
            if (!string.IsNullOrEmpty(chosen.AudioGroup)) groups.Add(chosen.AudioGroup);

            // YouTube can publish one otherwise identical video variant for each dubbed audio group.
            // Include the other groups only when both the video codec family and AAC codec match.
            foreach (var variant in Variants(master, baseUrl, int.MaxValue))
            {
                if (variant.AudioGroup == null || variant.AudioCodecFamily == null) continue;
                if (chosen.AudioCodecFamily != null && variant.AudioCodecFamily != chosen.AudioCodecFamily) continue;
                if (chosen.VideoCodecFamily != null && variant.VideoCodecFamily != chosen.VideoCodecFamily) continue;
                groups.Add(variant.AudioGroup);
            }

            var result = new List<AudioOption>();
            foreach (string line in Lines(master))
            {
                if (!line.StartsWith("#EXT-X-MEDIA:")) continue;
                var attributes = Attributes(line.Substring(13));
                if (Get(attributes, "TYPE") != "AUDIO") continue;
                string group = Get(attributes, "GROUP-ID");
                string uri = Get(attributes, "URI");
                if (group == null || uri == null || !groups.Contains(group)) continue;

                string resolved = Resolve(baseUrl, uri);
                if (result.Any(o => o.Uri == resolved)) continue;
                result.Add(new AudioOption
                {
                    Name = Get(attributes, "NAME"),
                    Language = Get(attributes, "LANGUAGE"),
                    Uri = resolved,
                    IsDefault = Get(attributes, "DEFAULT") == "YES"
                });
            }
            return result;
        }

        public static AudioOption PickAudioOption(IList<AudioOption> options, string preferredLanguage)
        {
            if (options == null || options.Count == 0) return null;
            if (!string.IsNullOrWhiteSpace(preferredLanguage))
            {
                var exact = options.FirstOrDefault(o =>
                    string.Equals(o.Language, preferredLanguage, StringComparison.OrdinalIgnoreCase));
                if (exact != null) return exact;

                string preferredPrimary = PrimaryLanguage(preferredLanguage);
                var related = options.FirstOrDefault(o =>
                    !string.IsNullOrEmpty(o.Language) &&
                    string.Equals(PrimaryLanguage(o.Language), preferredPrimary, StringComparison.OrdinalIgnoreCase));
                if (related != null) return related;
            }
            return options.FirstOrDefault(o => o.IsDefault) ?? options[0];
        }

        public static string AudioUri(string master, string baseUrl, string group)
        {
            string fallback = null;
            foreach (string line in Lines(master))
            {
                if (!line.StartsWith("#EXT-X-MEDIA:")) continue;
                var a = Attributes(line.Substring(13));
                if (Get(a, "TYPE") != "AUDIO" || Get(a, "URI") == null) continue;
                if (group != null && Get(a, "GROUP-ID") != group) continue;
                string uri = Resolve(baseUrl, Get(a, "URI"));
                if (Get(a, "DEFAULT") == "YES") return uri;
                if (fallback == null) fallback = uri;
            }
            return fallback;
        }

        public static List<Segment> Segments(string playlist, string baseUrl)
        {
            var list = new List<Segment>();
            string[] lines = Lines(playlist);
            for (int i = 0; i < lines.Length; i++)
            {
                if (!lines[i].StartsWith("#EXTINF:")) continue;
                string d = lines[i].Substring(8).Split(',')[0];
                double sec;
                double.TryParse(d, NumberStyles.Float, CultureInfo.InvariantCulture, out sec);
                string uri = NextUri(lines, i);
                if (uri != null) list.Add(new Segment { Uri = Resolve(baseUrl, uri), Ticks = (long)(sec * 10000000) });
            }
            return list;
        }

        static List<Variant> Variants(string master, string baseUrl, int maxHeight)
        {
            var list = new List<Variant>();
            string[] lines = Lines(master);
            for (int i = 0; i < lines.Length; i++)
            {
                if (!lines[i].StartsWith("#EXT-X-STREAM-INF:")) continue;
                var attributes = Attributes(lines[i].Substring(18));
                string codecs = Get(attributes, "CODECS") ?? "";
                string videoCodec = CodecStartingWith(codecs, "avc1");
                if (videoCodec == null) continue;
                string uri = NextUri(lines, i);
                if (uri == null) continue;

                var variant = new Variant
                {
                    Uri = Resolve(baseUrl, uri),
                    AudioGroup = Get(attributes, "AUDIO"),
                    VideoCodecFamily = CodecFamily(videoCodec),
                    AudioCodecFamily = CodecStartingWith(codecs, "mp4a")
                };

                string resolution = Get(attributes, "RESOLUTION");
                if (resolution != null && resolution.Contains("x"))
                {
                    int.TryParse(resolution.Split('x')[0], out variant.Width);
                    int.TryParse(resolution.Split('x')[1], out variant.Height);
                }
                long.TryParse(Get(attributes, "BANDWIDTH") ?? "0", out variant.Bandwidth);
                if (variant.Height > 0 && variant.Height <= maxHeight) list.Add(variant);
            }
            return list;
        }

        static string CodecStartingWith(string codecs, string prefix)
        {
            foreach (string codec in codecs.Split(',').Select(c => c.Trim()))
                if (codec.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) return codec;
            return null;
        }

        static string CodecFamily(string codec)
        {
            if (codec == null) return null;
            int dot = codec.IndexOf('.');
            return dot < 0 ? codec : codec.Substring(0, dot);
        }

        static string PrimaryLanguage(string language)
        {
            if (language == null) return "";
            int separator = language.IndexOf('-');
            return separator < 0 ? language : language.Substring(0, separator);
        }

        static string[] Lines(string text)
        {
            return text.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0).ToArray();
        }

        static string NextUri(string[] lines, int i)
        {
            for (int j = i + 1; j < lines.Length; j++)
            {
                if (lines[j].StartsWith("#EXTINF") || lines[j].StartsWith("#EXT-X-STREAM-INF")) return null;
                if (!lines[j].StartsWith("#")) return lines[j];
            }
            return null;
        }

        static string Resolve(string baseUrl, string uri)
        {
            return new Uri(new Uri(baseUrl), uri).AbsoluteUri;
        }

        static string Get(Dictionary<string, string> a, string key)
        {
            string v;
            return a.TryGetValue(key, out v) ? v : null;
        }

        /// <summary>KEY=VALUE,KEY="quoted, value" attribute lists.</summary>
        static Dictionary<string, string> Attributes(string s)
        {
            var d = new Dictionary<string, string>();
            int i = 0;
            while (i < s.Length)
            {
                int eq = s.IndexOf('=', i);
                if (eq < 0) break;
                string key = s.Substring(i, eq - i).Trim();
                string value;
                int j = eq + 1;
                if (j < s.Length && s[j] == '"')
                {
                    int end = s.IndexOf('"', j + 1);
                    if (end < 0) end = s.Length;
                    value = s.Substring(j + 1, end - j - 1);
                    i = end + 1;
                }
                else
                {
                    int end = s.IndexOf(',', j);
                    if (end < 0) end = s.Length;
                    value = s.Substring(j, end - j);
                    i = end;
                }
                d[key] = value;
                if (i < s.Length && s[i] == ',') i++;
            }
            return d;
        }
    }
}
