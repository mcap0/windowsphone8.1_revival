/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.local.feed.perle.PerleSelection.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using Perle.Extractor;

namespace Perle
{
    /// <summary>
    /// Pure helpers used by "Perle" to pick the most viewed videos of each channel and to
    /// mix them so that every channel has the same weight in the final list.
    /// </summary>
    public static class PerleSelection
    {
        public const int VideosPerChannel = 8;
        public const long MinDurationSeconds = 120L;

        /// <summary>
        /// Keeps only regular videos (no shorts, no live streams, no past live streams,
        /// no videos shorter than <see cref="MinDurationSeconds"/>) and returns the <paramref name="limit"/> most viewed ones.
        /// </summary>
        public static List<StreamInfoItem> SelectTopVideos(IEnumerable<StreamInfoItem> items, int limit = VideosPerChannel)
        {
            var seen = new HashSet<string>();
            return items
                .Where(it => it.StreamType == StreamType.VideoStream
                    && !it.IsShortFormContent
                    && it.Duration >= MinDurationSeconds)
                .Where(it => seen.Add(it.Url))
                // OrderByDescending is stable, like Kotlin's sortedByDescending
                .OrderByDescending(it => it.ViewCount)
                .Take(limit)
                .ToList();
        }

        /// <summary>
        /// Shuffles the videos of each channel and then interleaves the channels: every round takes
        /// one video from each channel which still has videos left, in a random channel order.
        /// This way no channel dominates the list, regardless of how many videos it has.
        /// </summary>
        public static List<T> Interleave<T>(IEnumerable<IList<T>> perChannel, Random random = null)
        {
            random = random ?? new Random();
            var queues = perChannel
                .Where(c => c.Count > 0)
                .Select(c => new Queue<T>(Shuffled(c, random)))
                .ToList();
            var result = new List<T>(queues.Sum(q => q.Count));
            var remaining = queues;
            while (remaining.Count > 0)
            {
                foreach (var q in Shuffled(remaining, random)) result.Add(q.Dequeue());
                remaining = remaining.Where(q => q.Count > 0).ToList();
            }
            return result;
        }

        /// <summary>Fisher–Yates shuffle returning a new list, like Kotlin's shuffled().</summary>
        public static List<T> Shuffled<T>(IEnumerable<T> source, Random random)
        {
            var list = source.ToList();
            for (int i = list.Count - 1; i > 0; i--)
            {
                int j = random.Next(i + 1);
                var tmp = list[i]; list[i] = list[j]; list[j] = tmp;
            }
            return list;
        }
    }
}
