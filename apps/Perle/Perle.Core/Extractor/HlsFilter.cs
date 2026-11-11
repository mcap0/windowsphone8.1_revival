/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace Perle.Extractor
{
    /// <summary>
    /// Rewrites YouTube's HLS master playlist keeping only what the Lumia decodes in hardware:
    /// H.264 variants up to a maximum height, and the audio renditions they reference.
    /// The VP9 variants are dropped, as Windows 10 Mobile would otherwise try them.
    /// </summary>
    public static class HlsFilter
    {
        static readonly Regex Attr = new Regex(@"([A-Z0-9-]+)=(""[^""]*""|[^,]*)");

        public static Dictionary<string, string> Attributes(string line)
        {
            var d = new Dictionary<string, string>();
            int colon = line.IndexOf(':');
            if (colon < 0) return d;
            foreach (Match m in Attr.Matches(line.Substring(colon + 1)))
                d[m.Groups[1].Value] = m.Groups[2].Value.Trim('"');
            return d;
        }

        static long Bandwidth(string[] variant)
        {
            string b;
            long n;
            return Attributes(variant[0]).TryGetValue("BANDWIDTH", out b) && long.TryParse(b, out n) ? n : 0;
        }

        public static string Filter(string master, int maxHeight, bool keepSubtitles = false)
        {
            var lines = master.Replace("\r", "").Split('\n');
            var variants = new List<string[]>(); // STREAM-INF line + URI line
            var media = new List<string>();
            var header = new List<string>();
            for (int i = 0; i < lines.Length; i++)
            {
                var l = lines[i];
                if (l.StartsWith("#EXT-X-STREAM-INF:"))
                {
                    var a = Attributes(l);
                    string codecs;
                    a.TryGetValue("CODECS", out codecs);
                    codecs = codecs ?? "";
                    int height = 0;
                    string res;
                    if (a.TryGetValue("RESOLUTION", out res) && res.Contains("x")) int.TryParse(res.Split('x')[1], out height);
                    string uri = i + 1 < lines.Length ? lines[++i] : "";
                    if (codecs.StartsWith("avc1") && height <= maxHeight && uri.Length > 0)
                        variants.Add(new[] { l, uri });
                }
                else if (l.StartsWith("#EXT-X-MEDIA:")) media.Add(l);
                else if (l.StartsWith("#EXTM3U") || l.StartsWith("#EXT-X-VERSION") || l.StartsWith("#EXT-X-INDEPENDENT-SEGMENTS"))
                    header.Add(l);
            }

            var audioGroups = new HashSet<string>(variants
                .Select(v => Attributes(v[0]))
                .Where(a => a.ContainsKey("AUDIO"))
                .Select(a => a["AUDIO"]));

            var sb = new StringBuilder();
            foreach (var h in header) sb.Append(h).Append('\n');
            foreach (var m in media)
            {
                var a = Attributes(m);
                string type;
                a.TryGetValue("TYPE", out type);
                string g;
                if (type == "AUDIO" && a.TryGetValue("GROUP-ID", out g) && audioGroups.Contains(g)) sb.Append(m).Append('\n');
                else if (type == "SUBTITLES" && keepSubtitles) sb.Append(m).Append('\n');
            }
            // highest quality first, so the player starts from the best one the connection allows
            foreach (var v in variants.OrderByDescending(v => Bandwidth(v)))
            {
                var inf = keepSubtitles ? v[0] : Regex.Replace(v[0], @",SUBTITLES=""[^""]*""", "");
                sb.Append(inf).Append('\n').Append(v[1]).Append('\n');
            }
            return sb.ToString();
        }
    }
}
