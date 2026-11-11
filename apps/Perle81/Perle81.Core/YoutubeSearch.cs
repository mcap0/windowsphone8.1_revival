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
    public static class YoutubeSearch
    {
        /// <summary>The "Videos" filter of the YouTube search page.</summary>
        public const string VideosFilterParams = "EgIQAQ==";

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
