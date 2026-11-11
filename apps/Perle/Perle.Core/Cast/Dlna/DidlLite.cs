/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.DidlLite (CastMedia, UpnpTime, DidlLite).
 */

using System;
using System.Globalization;
using System.Linq;

namespace Perle.Cast.Dlna
{
    /// <summary>What is sent to the TV.</summary>
    public sealed class CastMedia
    {
        /// <summary>the URL the TV downloads the video from (the local proxy or the direct link)</summary>
        public string Url { get; set; }
        public string Title { get; set; }
        public string Uploader { get; set; }
        /// <summary>e.g. video/mp4</summary>
        public string MimeType { get; set; }
        /// <summary>the duration, or a value &lt;= 0 if unknown</summary>
        public long DurationSeconds { get; set; }
        /// <summary>optional album art shown by some TVs</summary>
        public string ThumbnailUrl { get; set; }
        public CastDash Dash { get; set; }
        /// <summary>The fourth field of the protocolInfo, null for DidlLite.DlnaFeatures.</summary>
        public string DlnaFeatures { get; set; }
        /// <summary>URL of SubRip subtitles for the TV, null for none</summary>
        public string SubtitleUrl { get; set; }

        /// <summary>Kotlin data class copy().</summary>
        public CastMedia Copy() => (CastMedia)MemberwiseClone();
    }

    /// <summary>Time format used by UPnP: H+:MM:SS (e.g. 0:03:25), optionally followed by fractions.</summary>
    public static class UpnpTime
    {
        /// <returns>seconds formatted as H:MM:SS, as needed by Seek and the duration attribute</returns>
        public static string Format(long seconds)
        {
            long s = Math.Max(seconds, 0);
            return string.Format(CultureInfo.InvariantCulture, "{0}:{1:00}:{2:00}", s / 3600, (s % 3600) / 60, s % 60);
        }

        /// <summary>Parses H+:MM:SS[.F+] or H+:MM:SS[.F0/F1].</summary>
        /// <returns>the time in seconds, or -1 if it is unknown (NOT_IMPLEMENTED, empty, ...)</returns>
        public static long Parse(string text)
        {
            if (string.IsNullOrWhiteSpace(text)) return -1;
            var parts = text.Trim().Split('.')[0].Split(':');
            if (parts.Length != 3) return -1;
            var numbers = new long[3];
            for (int i = 0; i < 3; i++)
            {
                var p = parts[i].Trim();
                if (p.StartsWith("+")) p = p.Substring(1);
                if (!long.TryParse(p, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out numbers[i])) return -1;
            }
            return numbers[0] * 3600 + numbers[1] * 60 + numbers[2];
        }
    }

    /// <summary>
    /// Builds the DIDL-Lite metadata (CurrentURIMetaData) describing the video.
    ///
    /// Many renderers, Samsung TVs in particular, refuse SetAVTransportURI when the metadata is
    /// empty or invalid, so a complete item with title, creator, class and res is always sent.
    /// </summary>
    public static class DidlLite
    {
        /// <summary>
        /// The fourth field of the protocolInfo is the DLNA profile. * means "anything" and is
        /// accepted by most renderers.
        /// </summary>
        public const string DlnaFeatures = "*";

        public const string SrtMimeType = "text/srt";
        const string SecNamespace = "http://www.sec.co.kr/";

        public static string ProtocolInfo(string mimeType, string features = null) => "http-get:*:" + mimeType + ":" + (features ?? DlnaFeatures);

        /// <returns>the DIDL-Lite document, not yet escaped for being put inside a SOAP envelope</returns>
        public static string Build(CastMedia media, string itemId = "0")
        {
            var title = XmlUtils.Escape(media.Title);
            var uploader = XmlUtils.Escape(media.Uploader);
            var duration = media.DurationSeconds > 0 ? " duration=\"" + UpnpTime.Format(media.DurationSeconds) + ".000\"" : "";
            var albumArt = media.ThumbnailUrl != null ? "<upnp:albumArtURI>" + XmlUtils.Escape(media.ThumbnailUrl) + "</upnp:albumArtURI>" : "";
            // Samsung TVs read the sec: elements, other renderers a second res of type text/srt
            var subtitles = media.SubtitleUrl != null ? XmlUtils.Escape(media.SubtitleUrl) : null;
            var captionInfo = subtitles != null
                ? "<sec:CaptionInfoEx sec:type=\"srt\">" + subtitles + "</sec:CaptionInfoEx><sec:CaptionInfo sec:type=\"srt\">" + subtitles + "</sec:CaptionInfo>"
                : "";
            var subtitleRes = subtitles != null ? "<res protocolInfo=\"" + XmlUtils.Escape(ProtocolInfo(SrtMimeType)) + "\">" + subtitles + "</res>" : "";

            return "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\""
                + " xmlns:dc=\"http://purl.org/dc/elements/1.1/\""
                + " xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\""
                + " xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\""
                + (subtitles != null ? " xmlns:sec=\"" + SecNamespace + "\"" : "")
                + ">"
                + "<item id=\"" + XmlUtils.Escape(itemId) + "\" parentID=\"-1\" restricted=\"1\">"
                + "<dc:title>" + title + "</dc:title>"
                + "<dc:creator>" + uploader + "</dc:creator>"
                + "<upnp:artist>" + uploader + "</upnp:artist>"
                + "<upnp:class>object.item.videoItem</upnp:class>"
                + albumArt
                + captionInfo
                + "<res protocolInfo=\"" + XmlUtils.Escape(ProtocolInfo(media.MimeType, media.DlnaFeatures)) + "\"" + duration + ">"
                + XmlUtils.Escape(media.Url)
                + "</res>"
                + subtitleRes
                + "</item>"
                + "</DIDL-Lite>";
        }
    }
}
