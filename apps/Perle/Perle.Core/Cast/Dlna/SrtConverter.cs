/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.SrtConverter.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml;

namespace Perle.Cast.Dlna
{
    /// <summary>Converts subtitles to SubRip (SRT), the only subtitle format most TVs accept from DLNA.</summary>
    public static class SrtConverter
    {
        sealed class Cue
        {
            public readonly long StartMillis, EndMillis;
            public readonly string Text;
            public Cue(long start, long end, string text) { StartMillis = start; EndMillis = end; Text = text; }
        }

        static readonly HashSet<string> Formats = new HashSet<string> { "srt", "vtt", "ttml", "xml", "dfxp" };
        static readonly Regex Tag = new Regex("<[^>]*>");
        static readonly Regex Whitespace = new Regex(@"\s+");

        public static bool IsSupported(string format) => format != null && Formats.Contains(format.ToLowerInvariant());

        /// <param name="format">the subtitle format: ttml, vtt or srt (the file suffix)</param>
        /// <returns>the subtitles as SRT, or null if the format is not supported</returns>
        public static string Convert(string text, string format)
        {
            switch (format.ToLowerInvariant())
            {
                case "srt": return text;
                case "vtt": return FromVtt(text);
                case "ttml":
                case "xml":
                case "dfxp": return FromTtml(text);
                default: return null;
            }
        }

        static double? PositiveDouble(string s) =>
            double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) && d > 0 ? d : (double?)null;

        public static string FromTtml(string xml)
        {
            var root = XmlUtils.Parse(xml).DocumentElement;
            double tickRate = PositiveDouble(AttributeNamed(root, "tickRate")) ?? 1.0;
            double frameRate = PositiveDouble(AttributeNamed(root, "frameRate")) ?? 30.0;
            var cues = new List<Cue>();
            CollectParagraphs(root, p =>
            {
                long? start = ParseTtmlTime(p.GetAttribute("begin"), tickRate, frameRate);
                if (start == null) return;
                long? end = ParseTtmlTime(p.GetAttribute("end"), tickRate, frameRate);
                if (end == null)
                {
                    var dur = ParseTtmlTime(p.GetAttribute("dur"), tickRate, frameRate);
                    if (dur == null) return;
                    end = start + dur;
                }
                var text = string.Join("\n", TextOf(p).Split('\n').Select(l => l.Trim())).Trim();
                if (text.Length > 0 && end > start) cues.Add(new Cue(start.Value, end.Value, text));
            });
            return Write(cues);
        }

        public static string FromVtt(string vtt) => Write(ParseCues(vtt));

        /// <returns>srt with the times moved offsetMillis earlier, without the cues which ended
        /// before; for a video sent to the TV from the middle</returns>
        public static string Shift(string srt, long offsetMillis) => Write(ParseCues(srt)
            .Where(c => c.EndMillis > offsetMillis)
            .Select(c => new Cue(Math.Max(c.StartMillis - offsetMillis, 0), c.EndMillis - offsetMillis, c.Text))
            .ToList());

        /// <summary>Parses WebVTT or SRT cues.</summary>
        static List<Cue> ParseCues(string text)
        {
            var cues = new List<Cue>();
            var blocks = Regex.Split(text.Replace("\r\n", "\n").Replace('\r', '\n'), "\n{2,}");
            foreach (var block in blocks)
            {
                var lines = block.Split('\n').Where(l => !string.IsNullOrWhiteSpace(l)).ToList();
                int timingIndex = lines.FindIndex(l => l.Contains("-->"));
                if (timingIndex < 0) continue;
                var timing = lines[timingIndex].Split(new[] { "-->" }, StringSplitOptions.None);
                var start = ParseClock(timing[0].Trim());
                if (start == null) continue;
                var end = ParseClock(timing[1].Trim().Split(' ')[0]);
                if (end == null) continue;
                var cueText = DecodeEntities(string.Join("\n", lines.Skip(timingIndex + 1).Select(l => Tag.Replace(l, "").Trim()))).Trim();
                if (cueText.Length > 0 && end > start) cues.Add(new Cue(start.Value, end.Value, cueText));
            }
            return cues;
        }

        /// <summary>HH:MM:SS.mmm, MM:SS.mmm or HH:MM:SS,mmm</summary>
        static long? ParseClock(string text)
        {
            var parts = text.Replace(',', '.').Split(':');
            if (parts.Length < 2 || parts.Length > 3) return null;
            long hours = 0;
            if (parts.Length == 3 && !long.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out hours)) return null;
            if (!long.TryParse(parts[parts.Length - 2], NumberStyles.None, CultureInfo.InvariantCulture, out long minutes)) return null;
            if (!double.TryParse(parts[parts.Length - 1], NumberStyles.Float, CultureInfo.InvariantCulture, out double seconds)) return null;
            return hours * 3600000 + minutes * 60000 + JavaRound(seconds * 1000);
        }

        /// <summary>Math.round of Java: floor(x + 0.5).</summary>
        static long JavaRound(double x) => (long)Math.Floor(x + 0.5);

        /// <summary>
        /// TTML clock time (HH:MM:SS.mmm, HH:MM:SS:frames) or offset time (1.5s, 1500ms,
        /// 2h, 3m, 250t, 12f).
        /// </summary>
        static long? ParseTtmlTime(string text, double tickRate, double frameRate)
        {
            var value = text?.Trim();
            if (string.IsNullOrEmpty(value)) return null;
            if (value.Contains(':'))
            {
                var parts = value.Split(':');
                if (parts.Length == 4)
                {
                    var clock = ParseClock(string.Join(":", parts.Take(3)));
                    if (clock == null) return null;
                    if (!double.TryParse(parts[3], NumberStyles.Float, CultureInfo.InvariantCulture, out double frames)) return null;
                    return clock + JavaRound(frames * 1000 / frameRate);
                }
                return ParseClock(value);
            }
            int unitStart = value.Length;
            while (unitStart > 0 && char.IsLetter(value[unitStart - 1])) unitStart--;
            var unit = value.Substring(unitStart);
            if (!double.TryParse(value.Substring(0, unitStart), NumberStyles.Float, CultureInfo.InvariantCulture, out double number)) return null;
            double millis;
            switch (unit)
            {
                case "h": millis = number * 3600000; break;
                case "m": millis = number * 60000; break;
                case "s":
                case "": millis = number * 1000; break;
                case "ms": millis = number; break;
                case "t": millis = number * 1000 / tickRate; break;
                case "f": millis = number * 1000 / frameRate; break;
                default: return null;
            }
            return JavaRound(millis);
        }

        static void CollectParagraphs(XmlNode node, Action<XmlElement> onParagraph)
        {
            foreach (var child in node.ChildNodes.OfType<XmlElement>())
            {
                if (LocalNameOf(child) == "p") onParagraph(child);
                else CollectParagraphs(child, onParagraph);
            }
        }

        static string TextOf(XmlNode node)
        {
            var text = new StringBuilder();
            foreach (XmlNode child in node.ChildNodes)
            {
                if (child.NodeType == XmlNodeType.Text || child.NodeType == XmlNodeType.CDATA
                    || child.NodeType == XmlNodeType.Whitespace || child.NodeType == XmlNodeType.SignificantWhitespace)
                    text.Append(Whitespace.Replace(child.Value, " "));
                else if (child is XmlElement e && LocalNameOf(e) == "br") text.Append('\n');
                else if (child is XmlElement e2) text.Append(TextOf(e2));
            }
            return text.ToString();
        }

        static string AttributeNamed(XmlElement element, string localName)
        {
            foreach (XmlAttribute a in element.Attributes)
            {
                var name = !string.IsNullOrEmpty(a.LocalName) ? a.LocalName : a.Name.Substring(a.Name.IndexOf(':') + 1);
                if (name == localName) return a.Value;
            }
            return null;
        }

        static string LocalNameOf(XmlElement e) => !string.IsNullOrEmpty(e.LocalName) ? e.LocalName : e.Name.Substring(e.Name.IndexOf(':') + 1);

        static string DecodeEntities(string text) => text
            .Replace("&lt;", "<")
            .Replace("&gt;", ">")
            .Replace("&nbsp;", " ")
            .Replace("&quot;", "\"")
            .Replace("&#39;", "'")
            .Replace("&amp;", "&");

        static string Write(List<Cue> cues)
        {
            var srt = new StringBuilder();
            int index = 0;
            foreach (var cue in cues.OrderBy(c => c.StartMillis))
            {
                srt.Append(++index).Append('\n')
                    .Append(FormatTime(cue.StartMillis)).Append(" --> ").Append(FormatTime(cue.EndMillis)).Append('\n')
                    .Append(cue.Text).Append("\n\n");
            }
            return srt.ToString();
        }

        public static string FormatTime(long millis)
        {
            long m = Math.Max(millis, 0);
            return string.Format(CultureInfo.InvariantCulture, "{0:00}:{1:00}:{2:00},{3:000}", m / 3600000, (m / 60000) % 60, (m / 1000) % 60, m % 1000);
        }
    }
}
