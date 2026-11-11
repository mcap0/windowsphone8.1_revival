/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of the parts of YoutubeChannelHelper and YoutubeChannelTabExtractor used by Perle
 * (videos tab of a channel and its continuations).
 */

using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;

namespace Perle.Extractor
{
    public sealed class ChannelTabPage
    {
        public string ChannelName { get; set; }
        public string ChannelUrl { get; set; }
        public List<StreamInfoItem> Items { get; } = new List<StreamInfoItem>();
        /// <summary>Continuation token, null when there are no more pages (Page.isValid in NewPipe).</summary>
        public string NextPage { get; set; }
    }

    public static class YoutubeChannel
    {
        public const string VideosTabParams = "EgZ2aWRlb3PyBgQKAjoA";

        static readonly Regex ChannelIdPattern = new Regex(@"/channel/(UC[A-Za-z0-9_-]{22})");

        public static string ChannelIdFromUrl(string url)
        {
            var m = ChannelIdPattern.Match(url ?? "");
            if (!m.Success) throw new ExtractionException("Unsupported channel URL: " + url);
            return m.Groups[1].Value;
        }

        public static string UrlFromChannelId(string id) => "https://www.youtube.com/channel/" + id;

        /// <summary>First page of the videos tab of a channel (getChannelTab).</summary>
        public static async Task<ChannelTabPage> GetVideosTabAsync(string channelUrl, CancellationToken ct = default(CancellationToken))
        {
            string channelId = ChannelIdFromUrl(channelUrl);
            var json = await Innertube.BrowseAsync(new JObject { ["browseId"] = channelId, ["params"] = VideosTabParams }, ct)
                .ConfigureAwait(false);

            CheckAlerts(json);
            var page = new ChannelTabPage
            {
                ChannelName = json.ObjPath("metadata.channelMetadataRenderer").Str("title"),
                ChannelUrl = UrlFromChannelId(json.ObjPath("metadata.channelMetadataRenderer").Str("externalId") ?? channelId)
            };

            var tab = json.ObjPath("contents.twoColumnBrowseResultsRenderer").Arr("tabs").Objects()
                .Select(t => t.Obj("tabRenderer"))
                .FirstOrDefault(t => t.Bool("selected"));
            if (tab == null) return page; // the channel has no videos tab

            var contents = tab.ObjPath("content.richGridRenderer").Arr("contents");
            if (contents.Count == 0) contents = tab.ObjPath("content.sectionListRenderer").Arr("contents");
            Collect(contents, page);
            return page;
        }

        /// <summary>A following page of the videos tab (getMoreChannelTabItems).</summary>
        public static async Task<ChannelTabPage> GetMoreItemsAsync(ChannelTabPage previous, CancellationToken ct = default(CancellationToken))
        {
            var json = await Innertube.BrowseAsync(new JObject { ["continuation"] = previous.NextPage }, ct).ConfigureAwait(false);
            var page = new ChannelTabPage { ChannelName = previous.ChannelName, ChannelUrl = previous.ChannelUrl };
            foreach (var action in json.Arr("onResponseReceivedActions").Objects())
            {
                var items = action.Obj("appendContinuationItemsAction").Arr("continuationItems");
                if (items.Count == 0) items = action.Obj("reloadContinuationItemsCommand").Arr("continuationItems");
                Collect(items, page);
            }
            return page;
        }

        static void Collect(JArray contents, ChannelTabPage page)
        {
            foreach (var c in contents.Objects())
            {
                var content = c.Obj("richItemRenderer").Obj("content");
                var lockup = content.Obj("lockupViewModel");
                if (lockup.HasValues)
                {
                    var item = LockupExtractor.Extract(lockup, page.ChannelName, page.ChannelUrl);
                    if (item != null) page.Items.Add(item);
                    continue;
                }
                var video = content.Obj("videoRenderer");
                if (video.HasValues)
                {
                    var item = VideoRendererExtractor.Extract(video, page.ChannelName, page.ChannelUrl);
                    if (item != null) page.Items.Add(item);
                    continue;
                }
                var token = c.ObjPath("continuationItemRenderer.continuationEndpoint.continuationCommand").Str("token");
                if (token != null) page.NextPage = token;
            }
        }

        static void CheckAlerts(JObject json)
        {
            foreach (var alert in json.Arr("alerts").Objects())
            {
                var r = alert.Obj("alertRenderer");
                if (r.Str("type") == "ERROR") throw new ExtractionException("YouTube: " + (r.Obj("text").Text() ?? "error"));
            }
        }
    }

    /// <summary>Minimal port of YoutubeStreamInfoItemExtractor for the older videoRenderer items.</summary>
    public static class VideoRendererExtractor
    {
        public static StreamInfoItem Extract(JObject v, string uploaderName = null, string uploaderUrl = null)
        {
            var id = v.Str("videoId");
            if (string.IsNullOrEmpty(id)) return null;
            var item = new StreamInfoItem
            {
                VideoId = id,
                Url = StreamUrls.FromVideoId(id),
                Name = v.Obj("title").Text(),
                UploaderName = uploaderName ?? v.Obj("ownerText").Text() ?? v.Obj("longBylineText").Text(),
                UploaderUrl = uploaderUrl ?? OwnerUrl(v),
                TextualUploadDate = v.Obj("publishedTimeText").Text(),
                ThumbnailUrl = LockupExtractor.BestThumbnail(v.Obj("thumbnail").Arr("thumbnails"))
            };
            bool live = v.Arr("badges").Objects().Any(b => b.Obj("metadataBadgeRenderer").Str("style") == "BADGE_STYLE_TYPE_LIVE_NOW")
                || v.Arr("thumbnailOverlays").Objects().Any(o => o.Obj("thumbnailOverlayTimeStatusRenderer").Str("style") == "LIVE");
            item.StreamType = live ? StreamType.LiveStream : StreamType.VideoStream;
            var length = v.Obj("lengthText").Text();
            if (!live && length != null && length.Any(char.IsDigit)) item.Duration = LockupExtractor.ParseDurationString(length);
            var views = v.Obj("viewCountText").Text();
            if (views != null)
            {
                var lower = views.ToLowerInvariant();
                if (lower.Contains("no views")) item.ViewCount = 0;
                else if (Regex.IsMatch(views, @"\d[KMBkmb]"))
                    try { item.ViewCount = LockupExtractor.MixedNumberWordToLong(views); } catch (System.FormatException) { }
                else
                    // full count, e.g. "1,234,567 views"
                {
                    long n;
                    item.ViewCount = long.TryParse(new string(views.Where(char.IsDigit).ToArray()), out n) ? n : -1;
                }
            }
            item.IsShortFormContent = v.ObjPath("navigationEndpoint.reelWatchEndpoint").HasValues;
            return item;
        }

        /// <summary>Channel URL of the uploader, when the item links to a channel id (handles /@name have none).</summary>
        static string OwnerUrl(JObject v)
        {
            var id = v.Obj("ownerText").Arr("runs").Objects()
                .Select(r => r.ObjPath("navigationEndpoint.browseEndpoint").Str("browseId"))
                .FirstOrDefault(b => b != null && b.StartsWith("UC"));
            return id == null ? null : YoutubeChannel.UrlFromChannelId(id);
        }
    }
}
