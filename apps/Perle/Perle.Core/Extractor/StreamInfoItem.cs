/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of StreamInfoItem and YoutubeStreamInfoItemLockupExtractor (NewPipeExtractor 3313901),
 * keeping its en-GB text parsing.
 */

using System;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;
using Newtonsoft.Json.Linq;

namespace Perle.Extractor
{
    public enum StreamType { VideoStream, LiveStream, PostLiveStream }

    public sealed class StreamInfoItem
    {
        public string Url { get; set; }
        public string VideoId { get; set; }
        public string Name { get; set; }
        public StreamType StreamType { get; set; }
        public long Duration { get; set; } = -1;
        public long ViewCount { get; set; } = -1;
        public string UploaderName { get; set; }
        public string UploaderUrl { get; set; }
        public string TextualUploadDate { get; set; }
        public string ThumbnailUrl { get; set; }
        public bool IsShortFormContent { get; set; }
    }

    public static class StreamUrls
    {
        public static string FromVideoId(string videoId) => "https://www.youtube.com/watch?v=" + videoId;

        static readonly Regex VideoIdPattern = new Regex(@"(?:v=|youtu\.be/|/shorts/|/embed/|/live/)([A-Za-z0-9_-]{11})");

        public static string VideoIdFromUrl(string url)
        {
            var m = VideoIdPattern.Match(url ?? "");
            if (!m.Success) throw new ExtractionException("Not a YouTube video URL: " + url);
            return m.Groups[1].Value;
        }
    }

    /// <summary>Port of YoutubeStreamInfoItemLockupExtractor.</summary>
    public static class LockupExtractor
    {
        const string NoViewsLowercase = "no views";
        const string PremieresVideosText = "Premieres ";
        const string PremieresLivesText = "Scheduled for ";

        /// <returns>null for private/deleted videos (isAd in NewPipe) and non-video lockups</returns>
        public static StreamInfoItem Extract(JObject lockup, string uploaderName = null, string uploaderUrl = null)
        {
            var type = lockup.Str("contentType");
            if (type != null && type != "LOCKUP_CONTENT_TYPE_VIDEO") return null;

            string videoId = lockup.Str("contentId");
            if (string.IsNullOrEmpty(videoId))
                videoId = lockup.StrPath("rendererContext.commandContext.onTap.innertubeCommand.watchEndpoint.videoId");
            if (string.IsNullOrEmpty(videoId)) throw new ExtractionException("Could not get url");

            string name = lockup.StrPath("metadata.lockupMetadataViewModel.title.content");
            if (string.IsNullOrEmpty(name)) throw new ExtractionException("Could not get name");
            if (name == "[Private video]" || name == "[Deleted video]") return null;

            var rows = lockup.ArrPath("metadata.lockupMetadataViewModel.metadata.contentMetadataViewModel.metadataRows");
            var partsRows = rows.Objects().Where(r => r.Has("metadataParts")).Select(r => r.Arr("metadataParts")).ToList();
            var overlays = lockup.ArrPath("contentImage.thumbnailViewModel.overlays");

            var item = new StreamInfoItem
            {
                VideoId = videoId,
                Url = StreamUrls.FromVideoId(videoId),
                Name = name,
                StreamType = DetermineStreamType(overlays),
                ThumbnailUrl = BestThumbnail(lockup.ArrPath("contentImage.thumbnailViewModel.image.sources")),
                UploaderName = uploaderName,
                UploaderUrl = uploaderUrl
            };
            bool live = item.StreamType != StreamType.VideoStream;

            string dateText = null;
            if (partsRows.Count > 0)
            {
                // the views and date row is always the last one
                var last = partsRows[partsRows.Count - 1];
                if (last.Count > 0) dateText = PartText(last[last.Count - 1]);
                if (uploaderName == null && partsRows.Count > 1) item.UploaderName = PartText(partsRows[0][0]);
            }
            bool premiere = dateText != null && (dateText.Contains(PremieresVideosText) || dateText.Contains(PremieresLivesText));

            if (!live && !premiere) item.Duration = ParseDurationBadges(overlays);
            if (!live) item.TextualUploadDate = premiere
                ? dateText.Replace(PremieresVideosText, "").Replace(PremieresLivesText, "")
                : dateText;
            item.ViewCount = ViewCount(rows, partsRows, live, premiere);
            return item;
        }

        static StreamType DetermineStreamType(JArray overlays)
        {
            foreach (var overlay in overlays.Objects())
            {
                foreach (var badge in overlay.Obj("thumbnailOverlayBadgeViewModel").Arr("thumbnailBadges").Objects())
                {
                    var vm = badge.Obj("thumbnailBadgeViewModel");
                    if (vm.Str("badgeStyle") == "THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE") return StreamType.LiveStream;
                    if (vm.Obj("icon").Arr("sources").Objects().Any(s => s.Obj("clientResource").Str("imageName") == "LIVE"))
                        return StreamType.LiveStream;
                }
                foreach (var badge in overlay.Obj("thumbnailBottomOverlayViewModel").Arr("badges").Objects())
                    if (badge.Obj("thumbnailBadgeViewModel").Str("badgeStyle") == "THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE")
                        return StreamType.LiveStream;
            }
            return StreamType.VideoStream;
        }

        static long ParseDurationBadges(JArray overlays)
        {
            var texts = overlays.Objects()
                .SelectMany(o => o.Obj("thumbnailBottomOverlayViewModel").Arr("badges").Objects())
                .Select(b => b.Obj("thumbnailBadgeViewModel").Str("text"));
            foreach (var t in texts)
            {
                if (t == null || !t.Any(char.IsDigit)) continue;
                return ParseDurationString(t);
            }
            return -1; // e.g. only "SHORTS" or "CC" badge
        }

        static long ViewCount(JArray rows, System.Collections.Generic.List<JArray> partsRows, bool live, bool premiere)
        {
            bool membersOnly = rows.Objects().SelectMany(r => r.Arr("badges").Objects())
                .Any(b => b.Obj("badgeViewModel").Str("badgeStyle") == "BADGE_MEMBERS_ONLY");
            if (membersOnly) return -1;
            if (partsRows.Count == 0) return -1;
            if (premiere) return -1;
            if (live && partsRows.Count == 1) return 0;
            var row = partsRows[partsRows.Count - 1];
            if (row.Count == 0) return -1;
            var text = PartText(row[0]);
            if (string.IsNullOrEmpty(text)) return -1;
            var lower = text.ToLowerInvariant();
            if (lower.Contains(NoViewsLowercase)) return 0;
            if (lower.Contains("recommended")) return -1;
            try { return MixedNumberWordToLong(text); } catch (FormatException) { return -1; }
        }

        static string PartText(JToken part) => part.Obj("text").Str("content");

        public static string BestThumbnail(JArray sources)
        {
            var best = sources.Objects().OrderByDescending(s => s.Long("width", 0)).FirstOrDefault();
            var url = best?.Str("url");
            if (url != null && url.StartsWith("//")) url = "https:" + url;
            return url;
        }

        /// <summary>Port of YoutubeParsingHelper.parseDurationString.</summary>
        public static int ParseDurationString(string input)
        {
            if (!input.Any(char.IsDigit) && !input.Equals("SHORTS", StringComparison.OrdinalIgnoreCase))
                throw new ExtractionException("Error duration string contains no digits: " + input);
            var split = input.Contains(":") ? input.Split(':') : input.Split('.');
            int[] units = { 24, 60, 60, 1 };
            int offset = units.Length - split.Length;
            if (offset < 0) throw new ExtractionException("Error duration string with unknown format: " + input);
            int duration = 0;
            for (int i = 0; i < split.Length; i++)
                duration = units[i + offset] * (duration + ConvertDurationToInt(split[i]));
            return duration;
        }

        static int ConvertDurationToInt(string s)
        {
            var digits = new string((s ?? "").Where(char.IsDigit).ToArray());
            int v;
            return int.TryParse(digits, out v) ? v : 0;
        }

        static readonly Regex MultiplierPattern = new Regex(@"[\d]+([\.,][\d]+)?([KMBkmb])+");
        static readonly Regex NumberPattern = new Regex(@"([\d]+([\.,][\d]+)?)");

        /// <summary>Port of Utils.mixedNumberWordToLong ("2m views" → 2000000).</summary>
        public static long MixedNumberWordToLong(string numberWord)
        {
            var mm = MultiplierPattern.Match(numberWord);
            string multiplier = mm.Success ? mm.Groups[2].Value : "";
            var nm = NumberPattern.Match(numberWord);
            if (!nm.Success) throw new FormatException("No number in " + numberWord);
            double count = double.Parse(nm.Groups[1].Value.Replace(",", "."), CultureInfo.InvariantCulture);
            switch (multiplier.ToUpperInvariant())
            {
                case "K": return (long)(count * 1e3);
                case "M": return (long)(count * 1e6);
                case "B": return (long)(count * 1e9);
                default: return (long)count;
            }
        }
    }
}
