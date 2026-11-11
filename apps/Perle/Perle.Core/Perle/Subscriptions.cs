/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Reads and writes the NewPipe subscriptions JSON export, including the "feed_groups" (folders)
 * added by NewPipe Perle.
 */

using System.Collections.Generic;
using System.Linq;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Perle.Extractor;

namespace Perle
{
    public sealed class Subscription
    {
        public int ServiceId { get; set; }
        public string Url { get; set; }
        public string Name { get; set; }
    }

    public sealed class FeedGroup
    {
        /// <summary>Position in the export, used as id (FeedGroupEntity.uid in NewPipe).</summary>
        public long Id { get; set; }
        public string Name { get; set; }
        public int IconId { get; set; }
        public int SortOrder { get; set; }
        public List<string> SubscriptionUrls { get; } = new List<string>();
    }

    public sealed class SubscriptionsData
    {
        /// <summary>FeedGroupEntity.GROUP_ALL_ID: all the subscriptions.</summary>
        public const long GroupAllId = -1;

        public List<Subscription> Subscriptions { get; } = new List<Subscription>();
        public List<FeedGroup> FeedGroups { get; } = new List<FeedGroup>();

        public static SubscriptionsData Parse(string json)
        {
            var root = JObject.Parse(json);
            var data = new SubscriptionsData();
            var seen = new HashSet<string>();
            foreach (var s in root.Arr("subscriptions").Objects())
            {
                var url = s.Str("url");
                if (string.IsNullOrEmpty(url) || s.Long("service_id", 0) != 0) continue; // only YouTube
                if (!seen.Add(url)) continue;
                data.Subscriptions.Add(new Subscription { ServiceId = 0, Url = url, Name = s.Str("name") ?? url });
            }
            long id = 0;
            foreach (var g in root.Arr("feed_groups").Objects().OrderBy(g => g.Long("sort_order", 0)))
            {
                var group = new FeedGroup
                {
                    Id = id++,
                    Name = g.Str("name") ?? "",
                    IconId = (int)g.Long("icon_id", 0),
                    SortOrder = (int)g.Long("sort_order", 0)
                };
                foreach (var s in g.Arr("subscriptions").Objects())
                {
                    var url = s.Str("url");
                    if (url != null && seen.Contains(url)) group.SubscriptionUrls.Add(url);
                }
                data.FeedGroups.Add(group);
            }
            return data;
        }

        public string ToJson()
        {
            var root = new JObject
            {
                ["subscriptions"] = new JArray(Subscriptions.Select(s => new JObject
                {
                    ["service_id"] = s.ServiceId, ["url"] = s.Url, ["name"] = s.Name
                })),
                ["feed_groups"] = new JArray(FeedGroups.Select(g => new JObject
                {
                    ["name"] = g.Name,
                    ["icon_id"] = g.IconId,
                    ["sort_order"] = g.SortOrder,
                    ["subscriptions"] = new JArray(g.SubscriptionUrls.Select(u => new JObject { ["service_id"] = 0, ["url"] = u }))
                })),
                ["app_version"] = "perle-w10m",
                ["app_version_int"] = 1
            };
            return root.ToString(Formatting.Indented);
        }

        public List<Subscription> SubscriptionsFor(long groupId)
        {
            if (groupId == GroupAllId) return Subscriptions.ToList();
            var group = FeedGroups.FirstOrDefault(g => g.Id == groupId);
            if (group == null) return new List<Subscription>();
            var urls = new HashSet<string>(group.SubscriptionUrls);
            return Subscriptions.Where(s => urls.Contains(s.Url)).ToList();
        }
    }
}
