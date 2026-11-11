/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Small WebVTT parser used by the WP8.1 player.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;

namespace Perle81.App
{
    public sealed class SubtitleCue
    {
        public TimeSpan Start { get; set; }
        public TimeSpan End { get; set; }
        public string Text { get; set; }
    }

    public static class SubtitleCues
    {
        static readonly Regex TimingLine = new Regex(
            @"^\s*(\d{1,2}:)?\d{2}:\d{2}[.,]\d{3}\s+-->\s+(\d{1,2}:)?\d{2}:\d{2}[.,]\d{3}",
            RegexOptions.None);
        static readonly Regex Timestamp = new Regex(
            @"^\s*((?:\d{1,2}:)?\d{2}:\d{2}[.,]\d{3})\s+-->\s+((?:\d{1,2}:)?\d{2}:\d{2}[.,]\d{3})",
            RegexOptions.None);
        static readonly Regex Markup = new Regex(@"<[^>]*>");
        static readonly Regex Entities = new Regex(@"&(amp|lt|gt|quot|nbsp);", RegexOptions.IgnoreCase);

        public static List<SubtitleCue> Parse(string webVtt)
        {
            var result = new List<SubtitleCue>();
            if (string.IsNullOrEmpty(webVtt)) return result;

            string normalized = webVtt.Replace("\r\n", "\n").Replace('\r', '\n');
            string[] lines = normalized.Split('\n');
            int i = 0;
            while (i < lines.Length)
            {
                Match timing = Timestamp.Match(lines[i]);
                if (!timing.Success)
                {
                    i++;
                    continue;
                }

                TimeSpan start, end;
                if (!TryParseTime(timing.Groups[1].Value, out start) ||
                    !TryParseTime(timing.Groups[2].Value, out end) || end <= start)
                {
                    i++;
                    continue;
                }

                var textLines = new List<string>();
                i++;
                while (i < lines.Length && lines[i].Length > 0)
                {
                    string cleaned = Markup.Replace(lines[i], "");
                    cleaned = DecodeEntities(cleaned);
                    textLines.Add(cleaned);
                    i++;
                }

                string text = string.Join("\n", textLines).Trim();
                if (text.Length > 0)
                    result.Add(new SubtitleCue { Start = start, End = end, Text = text });
            }

            return result.OrderBy(c => c.Start).ThenBy(c => c.End).ToList();
        }

        /// <summary>Returns the text active at the given position; cues are searched in start-time order.</summary>
        public static string TextAt(IList<SubtitleCue> cues, TimeSpan position)
        {
            if (cues == null || cues.Count == 0) return "";

            long ticks = position.Ticks;
            int low = 0, high = cues.Count;
            while (low < high)
            {
                int middle = low + (high - low) / 2;
                if (cues[middle].Start.Ticks <= ticks) low = middle + 1;
                else high = middle;
            }

            int latest = low - 1;
            if (latest < 0) return "";

            var active = new List<SubtitleCue>();
            for (int i = latest; i >= 0; i--)
            {
                if (cues[i].Start.Ticks > ticks) continue;
                if (cues[i].End.Ticks > ticks) active.Add(cues[i]);
            }
            if (active.Count == 0) return "";
            active.Reverse();
            return string.Join("\n", active.Select(c => c.Text).ToArray());
        }

        static bool TryParseTime(string value, out TimeSpan time)
        {
            time = TimeSpan.Zero;
            if (string.IsNullOrEmpty(value)) return false;

            string normalized = value.Trim().Replace(',', '.');
            string[] parts = normalized.Split(':');
            int hours = 0, minutes, seconds;
            string secondsPart;
            if (parts.Length == 3)
            {
                if (!int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out hours) ||
                    !int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out minutes))
                    return false;
                secondsPart = parts[2];
            }
            else if (parts.Length == 2)
            {
                if (!int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out minutes))
                    return false;
                secondsPart = parts[1];
            }
            else return false;

            string[] secondParts = secondsPart.Split('.');
            if (secondParts.Length != 2 ||
                !int.TryParse(secondParts[0], NumberStyles.None, CultureInfo.InvariantCulture, out seconds))
                return false;

            int milliseconds;
            if (!int.TryParse(secondParts[1].PadRight(3, '0').Substring(0, 3),
                NumberStyles.None, CultureInfo.InvariantCulture, out milliseconds))
                return false;
            if (minutes >= 60 && parts.Length == 3) return false;
            if (seconds >= 60) return false;

            try
            {
                time = new TimeSpan(0, hours, minutes, seconds, milliseconds);
                return true;
            }
            catch (ArgumentOutOfRangeException)
            {
                return false;
            }
        }

        static string DecodeEntities(string text)
        {
            return Entities.Replace(text, match =>
            {
                switch (match.Groups[1].Value.ToLowerInvariant())
                {
                    case "amp": return "&";
                    case "lt": return "<";
                    case "gt": return ">";
                    case "quot": return "\"";
                    case "nbsp": return "\u00a0";
                    default: return match.Value;
                }
            });
        }
    }
}
