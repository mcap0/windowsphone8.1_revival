/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.CastTracks.
 */

using System.Collections.Generic;
using System.Linq;

namespace Perle.Cast
{
    /// <summary>
    /// One stream of a DASH presentation: a single MP4 file whose segments are found through its
    /// segment index.
    /// </summary>
    public sealed class DashTrack
    {
        public string Url { get; set; }
        public string MimeType { get; set; }
        public string Codecs { get; set; }
        /// <summary>bits per second, 0 if unknown</summary>
        public int Bandwidth { get; set; }
        /// <summary>first byte of the initialization data (ftyp + moov)</summary>
        public long InitStart { get; set; }
        /// <summary>last byte (included) of the initialization data</summary>
        public long InitEnd { get; set; }
        /// <summary>first byte of the segment index (sidx)</summary>
        public long IndexStart { get; set; }
        /// <summary>last byte (included) of the segment index</summary>
        public long IndexEnd { get; set; }
        public int Width { get; set; }
        public int Height { get; set; }

        public static DashTrack From(Perle.Extractor.StreamFormat f) => new DashTrack
        {
            Url = f.Url, MimeType = f.MimeType, Codecs = f.Codecs, Bandwidth = (int)f.Bitrate,
            InitStart = f.InitStart, InitEnd = f.InitEnd, IndexStart = f.IndexStart, IndexEnd = f.IndexEnd,
            Width = f.Width, Height = f.Height
        };
    }

    public sealed class CastDash
    {
        public DashTrack Video { get; set; }
        public DashTrack Audio { get; set; }
        public CastDash(DashTrack video, DashTrack audio) { Video = video; Audio = audio; }
    }

    /// <summary>How the MPEG-TS stream is announced to the TV: MIME type and DLNA profile.</summary>
    /// <param name="Profile">the DLNA.ORG_PN value, or null when the TV declares none that fits</param>
    public sealed class TsFormat
    {
        public string MimeType { get; }
        public string Profile { get; }
        public TsFormat(string mimeType, string profile) { MimeType = mimeType; Profile = profile; }

        /// <summary>The fourth field of the protocolInfo and the contentFeatures.dlna.org header.</summary>
        public string DlnaFeatures => Profile != null ? "DLNA.ORG_PN=" + Profile + ";" + LiveFeatures : LiveFeatures;

        /// <summary>No seeking by bytes or time, converted content, streaming transfer mode.</summary>
        public const string LiveFeatures = "DLNA.ORG_OP=00;DLNA.ORG_CI=1;DLNA.ORG_FLAGS=01700000000000000000000000000000";

        public static readonly TsFormat Default = new TsFormat("video/mp2t", null);

        /// <summary>
        /// video/vnd.dlna.mpeg-tts is the timestamped variant, with 192-byte packets: the
        /// muxer writes plain 188-byte packets, which DLNA calls video/mpeg with _ISO profiles.
        /// </summary>
        const string Timestamped = "video/vnd.dlna.mpeg-tts";

        /// <summary>Without a fitting profile, in order of preference.</summary>
        static readonly string[] PlainMimeTypes = { "video/mp2t", "video/mpeg", "video/x-mpeg-ts" };

        /// <summary>
        /// Chooses from the formats the TV declares, as TVs differ in the name they expect:
        /// an H.264 MPEG-TS profile with 188-byte packets (AVC_TS_..._ISO), AAC and HD first.
        /// </summary>
        /// <param name="sinkProtocolInfo">the Sink answer of GetProtocolInfo, or null if unknown</param>
        public static TsFormat Choose(string sinkProtocolInfo)
        {
            var declared = Parse(sinkProtocolInfo);
            var avc = declared.Where(d => d.Key != Timestamped && d.Value != null && d.Value.StartsWith("AVC_TS_") && d.Value.EndsWith("_ISO")).ToList();
            if (avc.Count > 0)
            {
                // maxByOrNull keeps the first of equal ones
                var best = avc[0];
                int Score(string p) => (p.Contains("AAC") ? 4 : 0) + (p.Contains("HD") ? 2 : 0) + (p.Contains("HP") ? 1 : 0);
                foreach (var d in avc) if (Score(d.Value) > Score(best.Value)) best = d;
                return new TsFormat(best.Key, best.Value);
            }
            var mimeTypes = new HashSet<string>(declared.Select(d => d.Key));
            var plain = PlainMimeTypes.FirstOrDefault(mimeTypes.Contains);
            return plain != null ? new TsFormat(plain, null) : Default;
        }

        /// <returns>the video entries of sinkProtocolInfo as (MIME type, DLNA profile)</returns>
        public static List<KeyValuePair<string, string>> Parse(string sinkProtocolInfo)
        {
            var result = new List<KeyValuePair<string, string>>();
            foreach (var entry in (sinkProtocolInfo ?? "").Split(','))
            {
                var fields = entry.Trim().Split(new[] { ':' }, 4);
                if (fields.Length < 3) continue;
                var mime = fields[2].ToLowerInvariant();
                if (!mime.StartsWith("video/")) continue;
                string profile = fields.Length > 3
                    ? fields[3].Split(';').FirstOrDefault(f => f.StartsWith("DLNA.ORG_PN="))?.Substring("DLNA.ORG_PN=".Length)
                    : null;
                result.Add(new KeyValuePair<string, string>(mime, profile));
            }
            return result;
        }
    }
}
