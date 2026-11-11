/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.CastStreamSelector.
 */

using System;
using System.Collections.Generic;
using System.Linq;

namespace Perle.Cast.Dlna
{
    /// <summary>A byte range from First to Last, both included (Kotlin LongRange).</summary>
    public sealed class LongRange
    {
        public readonly long First, Last;
        public LongRange(long first, long last) { First = first; Last = last; }
    }

    /// <summary>
    /// A video stream that could be sent to the TV, decoupled from the extractor classes so that
    /// the selection rules can be unit tested without them.
    /// </summary>
    public sealed class CastStreamCandidate
    {
        public string Url { get; set; }
        public string MimeType { get; set; }
        /// <summary>the vertical resolution, e.g. 720, or 0 if unknown</summary>
        public int Height { get; set; }
        /// <summary>true for DASH video streams without audio</summary>
        public bool IsVideoOnly { get; set; }
        /// <summary>true if the stream is a single file downloadable via HTTP (not DASH, HLS, ...)</summary>
        public bool IsProgressiveHttp { get; set; }
        /// <summary>e.g. avc1.64001F, null if unknown</summary>
        public string Codec { get; set; }
        public int Width { get; set; }
        public int Bitrate { get; set; }
        /// <summary>the byte range of the MP4 initialization data, null if unknown</summary>
        public LongRange InitRange { get; set; }
        /// <summary>the byte range of the MP4 segment index (sidx), null if unknown</summary>
        public LongRange IndexRange { get; set; }
    }

    /// <summary>An audio stream that could be sent to the TV together with a video-only stream.</summary>
    public sealed class CastAudioCandidate
    {
        public string Url { get; set; }
        public string MimeType { get; set; }
        public bool IsProgressiveHttp { get; set; }
        public string Codec { get; set; }
        public int Bitrate { get; set; }
        /// <summary>false for dubbed or descriptive audio tracks</summary>
        public bool IsOriginal { get; set; } = true;
        public LongRange InitRange { get; set; }
        public LongRange IndexRange { get; set; }
        /// <summary>identifies the language/kind of the track, null when there is only one</summary>
        public string TrackId { get; set; }
    }

    /// <summary>What CastStreamSelector.Choose chose: either one Single file with audio and video, or streams to mux (Dash).</summary>
    public sealed class CastChoice
    {
        public CastStreamCandidate Single { get; }
        public CastDash Dash { get; }
        public CastChoice(CastStreamCandidate single, CastDash dash) { Single = single; Dash = dash; }
        public int Height => Single?.Height ?? Dash?.Video?.Height ?? 0;
    }

    /// <summary>
    /// Chooses the stream sent to the TV.
    ///
    /// DLNA renderers play best one file with both audio and video, so "progressive" MP4 streams with
    /// audio are used first, at the highest resolution up to MaxHeight. YouTube rarely offers them
    /// nowadays, so otherwise a video-only MP4 and an M4A audio stream, chosen by SelectDash, are
    /// muxed by the cast proxy into one MPEG-TS stream while they are sent.
    /// </summary>
    public static class CastStreamSelector
    {
        public const int MaxHeight = 720;
        public const int MaxDashHeight = 1080;
        public const string Mp4MimeType = "video/mp4";
        public const string M4aMimeType = "audio/mp4";
        const string H264 = "avc1";
        const string Aac = "mp4a";
        const string DefaultVideoCodec = "avc1.4d401f";
        const string DefaultAudioCodec = "mp4a.40.2";

        /// <param name="maxHeight">the highest resolution wanted, at most MaxHeight</param>
        public static CastStreamCandidate Select(IList<CastStreamCandidate> candidates, int maxHeight = MaxHeight) =>
            MaxBy(UsableProgressive(candidates).Where(c => c.Height <= Math.Min(maxHeight, MaxHeight)), c => c.Height);

        /// <summary>
        /// Chooses a video-only H.264 MP4 stream up to MaxDashHeight and an AAC M4A stream
        /// (original language first): the codecs MPEG-TS carries and every TV decodes. Both need
        /// their init and index ranges, used to read them fragment by fragment.
        /// </summary>
        /// <param name="audioTrackId">the wanted audio track, null for the original one</param>
        /// <returns>the streams to mux, or null if there are no usable streams</returns>
        public static CastDash SelectDash(IList<CastStreamCandidate> videos, IList<CastAudioCandidate> audios, int maxHeight = MaxDashHeight, string audioTrackId = null)
        {
            var usableVideos = UsableDashVideos(videos).ToList();
            // maxWithOrNull(compareBy height, then bitrate): the first of equal ones wins, like Kotlin
            CastStreamCandidate video = null;
            foreach (var v in usableVideos.Where(v => v.Height <= maxHeight))
                if (video == null || v.Height > video.Height || (v.Height == video.Height && v.Bitrate > video.Bitrate)) video = v;
            if (video == null) video = MinBy(usableVideos, v => v.Height);
            if (video == null) return null;

            var usableAudios = UsableDashAudios(audios).ToList();
            var ofTrack = usableAudios.Where(a => audioTrackId != null && a.TrackId == audioTrackId).ToList();
            var audio = (ofTrack.Count > 0 ? ofTrack : usableAudios)
                .OrderByDescending(a => a.IsOriginal).ThenByDescending(a => a.Bitrate).FirstOrDefault();
            if (audio == null) return null;

            return new CastDash(
                new DashTrack
                {
                    Url = video.Url, MimeType = Mp4MimeType, Codecs = video.Codec ?? DefaultVideoCodec, Bandwidth = video.Bitrate,
                    Width = video.Width, Height = video.Height,
                    InitStart = video.InitRange.First, InitEnd = video.InitRange.Last,
                    IndexStart = video.IndexRange.First, IndexEnd = video.IndexRange.Last
                },
                new DashTrack
                {
                    Url = audio.Url, MimeType = M4aMimeType, Codecs = audio.Codec ?? DefaultAudioCodec, Bandwidth = audio.Bitrate,
                    InitStart = audio.InitRange.First, InitEnd = audio.InitRange.Last,
                    IndexStart = audio.IndexRange.First, IndexEnd = audio.IndexRange.Last
                });
        }

        /// <summary>Chooses what to cast for the user's choices.</summary>
        /// <param name="height">the wanted resolution, null for automatic: a progressive MP4 if there is one
        /// (the TV can seek in it), otherwise the best muxed streams</param>
        /// <param name="audioTrackId">the wanted audio track, null for the original one. Another track is
        /// only available as a separate stream, so it is always muxed.</param>
        public static CastChoice Choose(IList<CastStreamCandidate> progressive, IList<CastStreamCandidate> videoOnly, IList<CastAudioCandidate> audios,
            int? height = null, string audioTrackId = null)
        {
            bool wantsOtherAudio = audioTrackId != null && UsableDashAudios(audios).Any(a => a.TrackId == audioTrackId && !a.IsOriginal);
            if (!wantsOtherAudio)
            {
                var single = height == null ? Select(progressive) : UsableProgressive(progressive).FirstOrDefault(c => c.Height == height);
                if (single != null) return new CastChoice(single, null);
            }
            var dash = SelectDash(videoOnly, audios, height ?? MaxDashHeight, audioTrackId);
            if (dash != null) return new CastChoice(null, dash);
            // e.g. a resolution available only as progressive MP4 with another audio track asked
            var fallback = Select(progressive, height ?? MaxHeight) ?? Select(progressive);
            return fallback != null ? new CastChoice(fallback, null) : null;
        }

        /// <returns>the resolutions that can be cast, highest first</returns>
        public static List<int> Qualities(IList<CastStreamCandidate> progressive, IList<CastStreamCandidate> videoOnly)
        {
            var progressiveHeights = UsableProgressive(progressive).Select(c => c.Height).Where(h => h >= 1 && h <= MaxHeight);
            var dashHeights = UsableDashVideos(videoOnly).Select(c => c.Height);
            return progressiveHeights.Concat(dashHeights).Distinct().OrderByDescending(h => h).ToList();
        }

        /// <returns>the ids of the audio tracks that can be cast, the original one first, or an empty
        /// list if there is only one track</returns>
        public static List<string> AudioTracks(IList<CastAudioCandidate> audios)
        {
            var tracks = UsableDashAudios(audios).Where(a => a.TrackId != null)
                .OrderByDescending(a => a.IsOriginal).Select(a => a.TrackId).Distinct().ToList();
            return tracks.Count > 1 ? tracks : new List<string>();
        }

        static IEnumerable<CastStreamCandidate> UsableProgressive(IEnumerable<CastStreamCandidate> candidates) => candidates.Where(c =>
            !c.IsVideoOnly && c.IsProgressiveHttp && !string.IsNullOrEmpty(c.Url) && string.Equals(c.MimeType, Mp4MimeType, StringComparison.OrdinalIgnoreCase));

        static IEnumerable<CastStreamCandidate> UsableDashVideos(IEnumerable<CastStreamCandidate> videos) => videos.Where(c =>
            c.IsVideoOnly && c.IsProgressiveHttp && !string.IsNullOrEmpty(c.Url)
            && string.Equals(c.MimeType, Mp4MimeType, StringComparison.OrdinalIgnoreCase)
            && c.Height >= 1 && c.Height <= MaxDashHeight
            && (c.Codec == null || c.Codec.StartsWith(H264, StringComparison.OrdinalIgnoreCase))
            && HasRanges(c.InitRange, c.IndexRange));

        static IEnumerable<CastAudioCandidate> UsableDashAudios(IEnumerable<CastAudioCandidate> audios) => audios.Where(a =>
            a.IsProgressiveHttp && !string.IsNullOrEmpty(a.Url)
            && string.Equals(a.MimeType, M4aMimeType, StringComparison.OrdinalIgnoreCase)
            && (a.Codec == null || a.Codec.StartsWith(Aac, StringComparison.OrdinalIgnoreCase))
            && HasRanges(a.InitRange, a.IndexRange));

        /// <returns>the range from start to end (both included), or null if not valid</returns>
        public static LongRange RangeOf(long start, long end) => start >= 0 && end > start ? new LongRange(start, end) : null;

        static bool HasRanges(LongRange initRange, LongRange indexRange) => initRange != null && indexRange != null;

        /// <summary>maxByOrNull: the first of equal ones wins.</summary>
        static TItem MaxBy<TItem>(IEnumerable<TItem> items, Func<TItem, int> key) where TItem : class
        {
            TItem best = null; int bestKey = int.MinValue;
            foreach (var i in items) { int k = key(i); if (best == null || k > bestKey) { best = i; bestKey = k; } }
            return best;
        }

        static TItem MinBy<TItem>(IEnumerable<TItem> items, Func<TItem, int> key) where TItem : class
        {
            TItem best = null; int bestKey = int.MaxValue;
            foreach (var i in items) { int k = key(i); if (best == null || k < bestKey) { best = i; bestKey = k; } }
            return best;
        }
    }
}
