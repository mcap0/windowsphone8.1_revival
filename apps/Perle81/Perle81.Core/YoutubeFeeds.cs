/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * The RSS feed of a channel (15 latest uploads), used by the "Ultimi" tab. Shorts are left out.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Xml.Linq;
using Perle.Extractor;

namespace Perle.Feeds
{
    public sealed class FeedVideo
    {
        public string VideoId { get; set; }
        public string Title { get; set; }
        public string Uploader { get; set; }
        public DateTimeOffset Published { get; set; }
    }

    public static class YoutubeFeeds
    {
        static readonly XNamespace Atom = "http://www.w3.org/2005/Atom";
        static readonly XNamespace Yt = "http://www.youtube.com/xml/schemas/2015";

        public static string FeedUrl(string channelId) => "https://www.youtube.com/feeds/videos.xml?channel_id=" + channelId;

        public static async Task<List<FeedVideo>> LoadAsync(string channelId, CancellationToken ct = default(CancellationToken))
        {
            string xml = await Innertube.Http.GetStringAsync(FeedUrl(channelId)).ConfigureAwait(false);
            ct.ThrowIfCancellationRequested();
            return Parse(xml);
        }

        public static List<FeedVideo> Parse(string xml)
        {
            var doc = XDocument.Parse(xml);
            var videos = new List<FeedVideo>();
            foreach (var entry in doc.Root.Elements(Atom + "entry"))
            {
                string id = entry.Element(Yt + "videoId") == null ? null : entry.Element(Yt + "videoId").Value;
                if (string.IsNullOrEmpty(id)) continue;
                bool isShort = entry.Elements(Atom + "link").Any(l => (l.Attribute("href") != null) && l.Attribute("href").Value.Contains("/shorts/"));
                if (isShort) continue;

                var author = entry.Element(Atom + "author");
                var name = author == null ? null : author.Element(Atom + "name");
                var published = entry.Element(Atom + "published");
                DateTimeOffset when;
                if (published == null || !DateTimeOffset.TryParse(published.Value, CultureInfo.InvariantCulture, DateTimeStyles.None, out when))
                    when = DateTimeOffset.MinValue;

                videos.Add(new FeedVideo
                {
                    VideoId = id,
                    Title = entry.Element(Atom + "title") == null ? "" : entry.Element(Atom + "title").Value,
                    Uploader = name == null ? "" : name.Value,
                    Published = when
                });
            }
            return videos;
        }
    }
}
