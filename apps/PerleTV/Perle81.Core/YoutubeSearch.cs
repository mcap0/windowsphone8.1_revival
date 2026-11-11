/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Video search (InnerTube "search" with the WEB client, "videos only" filter), port of YoutubeSearchQueryHandler.
 * Only used by Perle 8.1: the UWP app and the tests do not link it.
 */

using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using Perle.Extractor;

namespace Perle.Search
{
    /// <summary>A channel found by the search, opened with ChannelPage.</summary>
    public sealed class ChannelResult
    {
        public string Name { get; set; }
        public string Url { get; set; }
        public string Details { get; set; }
    }

    public static class YoutubeSearch
    {
        /// <summary>The "Videos" filter of the YouTube search page.</summary>
        public const string VideosFilterParams = "EgIQAQ==";
        /// <summary>The "Channels" filter of the YouTube search page.</summary>
        public const string ChannelsFilterParams = "EgIQAg==";

        public static async Task<List<ChannelResult>> SearchChannelsAsync(string query, CancellationToken ct = default(CancellationToken))
        {
            var body = Innertube.PrepareBody(Innertube.WebClient());
            body["query"] = query;
            body["params"] = ChannelsFilterParams;
            var json = await Innertube.PostAsync(Innertube.YoutubeiV1Url + "search?" + Innertube.DisablePrettyPrint, body, false, ct)
                .ConfigureAwait(false);

            var items = new List<ChannelResult>();
            var sections = json.ObjPath("contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer").Arr("contents");
            foreach (var section in sections.Objects())
            {
                foreach (var entry in section.Obj("itemSectionRenderer").Arr("contents").Objects())
                {
                    var channel = entry.Obj("channelRenderer");
                    var id = channel.Str("channelId");
                    if (id == null) continue;
                    items.Add(new ChannelResult
                    {
                        Name = channel.Obj("title").Text(),
                        Url = YoutubeChannel.UrlFromChannelId(id),
                        Details = channel.Obj("subscriberCountText").Text()
                    });
                }
            }
            return items;
        }

        public static async Task<List<StreamInfoItem>> SearchVideosAsync(string query, CancellationToken ct = default(CancellationToken))
        {
            var body = Innertube.PrepareBody(Innertube.WebClient());
            body["query"] = query;
            body["params"] = VideosFilterParams;
            var json = await Innertube.PostAsync(Innertube.YoutubeiV1Url + "search?" + Innertube.DisablePrettyPrint, body, false, ct)
                .ConfigureAwait(false);

            var items = new List<StreamInfoItem>();
            var sections = json.ObjPath("contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer").Arr("contents");
            foreach (var section in sections.Objects())
            {
                foreach (var entry in section.Obj("itemSectionRenderer").Arr("contents").Objects())
                {
                    var video = entry.Obj("videoRenderer");
                    if (video.Has("videoId")) items.Add(VideoRendererExtractor.Extract(video));
                }
            }
            return items;
        }
    }
}
