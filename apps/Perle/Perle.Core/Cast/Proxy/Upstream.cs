/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of HttpRange, YoutubeRange, Upstream/UpstreamResponse (CastProxyServer.kt) and
 * OkHttpUpstream, with HttpClient instead of OkHttp.
 */

using System;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Text.RegularExpressions;
using System.Threading;
using Perle.Extractor;

namespace Perle.Cast.Proxy
{
    /// <summary>A single byte range from a "Range: bytes=..." header.</summary>
    /// <param name="Start">first byte, or null for a suffix range (bytes=-500, the last 500 bytes)</param>
    /// <param name="End">last byte (inclusive), or null for an open range (bytes=100-)</param>
    /// <param name="SuffixLength">length of a suffix range, null otherwise</param>
    public sealed class HttpRange
    {
        public readonly long? Start, End, SuffixLength;

        public HttpRange(long? start, long? end, long? suffixLength = null) { Start = start; End = end; SuffixLength = suffixLength; }

        /// <returns>the header value to forward to the upstream server</returns>
        public string ToHeaderValue() =>
            SuffixLength != null ? "bytes=-" + SuffixLength
            : End != null ? "bytes=" + Start + "-" + End
            : "bytes=" + Start + "-";

        /// <summary>
        /// Parses a Range header. Only single ranges in bytes are supported: that is what video
        /// players ask for. Multiple ranges (bytes=0-1,5-6) would need a multipart answer.
        /// </summary>
        /// <returns>the range, null if there is no header (the whole file is requested)</returns>
        /// <exception cref="ArgumentException">if the header is malformed or not supported</exception>
        public static HttpRange Parse(string header)
        {
            if (string.IsNullOrWhiteSpace(header)) return null;
            var value = header.Trim();
            Require(value.StartsWith("bytes=", StringComparison.OrdinalIgnoreCase), "Unsupported range unit: " + value);
            var spec = value.Substring("bytes=".Length).Trim();
            Require(!spec.Contains(','), "Multiple ranges are not supported: " + value);

            int dash = spec.IndexOf('-');
            Require(dash >= 0, "Malformed range: " + value);
            var startText = spec.Substring(0, dash).Trim();
            var endText = spec.Substring(dash + 1).Trim();

            if (startText.Length == 0)
            {
                Require(long.TryParse(endText, out var suffix) && suffix > 0, "Malformed suffix range: " + value);
                return new HttpRange(null, null, suffix);
            }
            Require(long.TryParse(startText, out var start) && start >= 0, "Malformed range start: " + value);
            if (endText.Length == 0) return new HttpRange(start, null);
            Require(long.TryParse(endText, out var end) && end >= start, "Malformed range end: " + value);
            return new HttpRange(start, end);
        }

        static void Require(bool condition, string message) { if (!condition) throw new ArgumentException(message); }

        /// <summary>Parses the total length from a "Content-Range: bytes 0-0/12345" header.</summary>
        /// <returns>the total length, or -1 if unknown (*) or malformed</returns>
        public static long TotalLengthFromContentRange(string contentRange)
        {
            if (contentRange == null) return -1;
            int slash = contentRange.LastIndexOf('/');
            if (slash < 0) return -1;
            return long.TryParse(contentRange.Substring(slash + 1).Trim(), out var total) ? total : -1;
        }
    }

    /// <summary>
    /// Range requests the way YouTube's own players make them for DASH streams: with a range
    /// parameter in the URL instead of the Range header, which googlevideo sometimes answers with
    /// HTTP 403 for these streams.
    /// </summary>
    public static class YoutubeRange
    {
        /// <returns>url with the range parameter for range, or null if it cannot be used</returns>
        public static string UrlWithRange(string url, HttpRange range)
        {
            if (range?.Start == null || range.End == null) return null;
            if (!url.Contains("/videoplayback") || url.Contains("&range=")) return null;
            return url + "&range=" + range.Start + "-" + range.End;
        }

        static readonly Regex Clen = new Regex(@"[?&]clen=(\d+)");

        /// <returns>the Content-Range such an answer would have had with the header: the total length
        /// comes from the clen parameter of the URL, * if it is missing</returns>
        public static string ContentRange(string url, long start, long contentLength)
        {
            var m = Clen.Match(url);
            return "bytes " + start + "-" + (start + contentLength - 1) + "/" + (m.Success ? m.Groups[1].Value : "*");
        }
    }

    /// <summary>The answer of the real server (e.g. googlevideo) to a request made by the proxy.</summary>
    public sealed class UpstreamResponse : IDisposable
    {
        public readonly int Code;
        /// <summary>the length of body, -1 if unknown</summary>
        public readonly long ContentLength;
        public readonly string ContentRange, ContentType;
        /// <summary>the content; null for answers without body</summary>
        public readonly Stream Body;
        readonly Action onClose;

        public UpstreamResponse(int code, long contentLength, string contentRange, string contentType, Stream body, Action onClose = null)
        {
            Code = code; ContentLength = contentLength; ContentRange = contentRange; ContentType = contentType; Body = body;
            this.onClose = onClose ?? (() => body?.Dispose());
        }

        public void Dispose() => onClose();
    }

    /// <summary>Downloads the content the proxy forwards to the TV.</summary>
    public interface IUpstream
    {
        /// <param name="url">the real URL of the video</param>
        /// <param name="range">the range to request, or null for the whole file</param>
        UpstreamResponse Open(string url, HttpRange range);
    }

    /// <summary>
    /// Port of OkHttpUpstream: YouTube videoplayback URLs are requested the way NewPipe's own player
    /// does, with a POST request, the rn parameter and the headers of the client the URL was obtained
    /// with. When googlevideo answers 403 to the range parameter, the Range header is tried once.
    /// </summary>
    public sealed class HttpUpstream : IUpstream
    {
        const string YoutubeBaseUrl = "https://www.youtube.com";
        const int LoggedUrlLength = 120;
        static readonly byte[] PostBody = { 0x78, 0 };

        readonly bool useRangeParameter;
        readonly HttpClient client;
        int requestNumber;

        public HttpUpstream(bool useRangeParameter = false, HttpClient client = null)
        {
            this.useRangeParameter = useRangeParameter;
            this.client = client ?? new HttpClient(new HttpClientHandler { AllowAutoRedirect = true }) { Timeout = TimeSpan.FromSeconds(30) };
        }

        public UpstreamResponse Open(string url, HttpRange range)
        {
            bool withRangeParameter = useRangeParameter && YoutubeRange.UrlWithRange(url, range) != null;
            var response = Request(url, range, withRangeParameter);
            if (response.Code != 403 || !withRangeParameter) return response;
            response.Dispose();
            CastLog.W("HTTP 403 for range parameter " + range?.ToHeaderValue() + ", trying the Range header");
            return Request(url, range, false);
        }

        // C_VISIONOS_PATTERN and C_WEB_PATTERN of YoutubeParsingHelper
        static bool IsVisionOsStreamingUrl(string url) => url.Contains("&c=VISIONOS");
        static bool IsWebStreamingUrl(string url) => url.Contains("&c=WEB");

        UpstreamResponse Request(string url, HttpRange range, bool withRangeParameter)
        {
            bool isVideoPlayback = url.Contains("/videoplayback");
            var withRange = withRangeParameter ? YoutubeRange.UrlWithRange(url, range) : null;
            var baseUrl = withRange ?? url;
            var requestUrl = isVideoPlayback && !baseUrl.Contains("&rn=")
                ? baseUrl + "&rn=" + (Interlocked.Increment(ref requestNumber) - 1)
                : baseUrl;

            var request = new HttpRequestMessage(isVideoPlayback ? HttpMethod.Post : HttpMethod.Get, requestUrl);
            request.Headers.TryAddWithoutValidation("Accept-Encoding", "identity");
            if (withRange == null && range != null) request.Headers.TryAddWithoutValidation("Range", range.ToHeaderValue());
            request.Headers.TryAddWithoutValidation("User-Agent",
                IsVisionOsStreamingUrl(requestUrl) ? Innertube.VisionOsUserAgent : Innertube.BrowserUserAgent);
            if (IsWebStreamingUrl(requestUrl))
            {
                request.Headers.TryAddWithoutValidation("Origin", YoutubeBaseUrl);
                request.Headers.TryAddWithoutValidation("Referer", YoutubeBaseUrl);
                request.Headers.TryAddWithoutValidation("Sec-Fetch-Dest", "empty");
                request.Headers.TryAddWithoutValidation("Sec-Fetch-Mode", "cors");
                request.Headers.TryAddWithoutValidation("Sec-Fetch-Site", "cross-site");
            }
            if (isVideoPlayback) request.Content = new ByteArrayContent(PostBody);

            CastLog.D("Upstream -> " + (isVideoPlayback ? "POST" : "GET") + " " + (range?.ToHeaderValue() ?? "whole file")
                + (withRange != null ? " (as parameter) " : " ") + requestUrl.Substring(0, Math.Min(LoggedUrlLength, requestUrl.Length)) + "…");
            var response = client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult();
            int code = (int)response.StatusCode;
            if (!response.IsSuccessStatusCode) CastLog.W("Upstream <- HTTP " + code + " for " + (range?.ToHeaderValue() ?? "whole file"));
            long contentLength = response.Content.Headers.ContentLength ?? -1;
            // with the range parameter, the server answers 200 with only the bytes asked
            bool asPartial = withRange != null && code == 200;
            string contentRange;
            if (asPartial)
            {
                long start = range.Start.Value;
                long length = contentLength >= 0 ? contentLength : range.End.Value - start + 1;
                contentRange = YoutubeRange.ContentRange(url, start, length);
            }
            else
            {
                contentRange = response.Content.Headers.ContentRange?.ToString();
            }
            var body = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult();
            return new UpstreamResponse(asPartial ? 206 : code, contentLength, contentRange,
                response.Content.Headers.ContentType?.ToString(), body, () => { body.Dispose(); response.Dispose(); request.Dispose(); });
        }
    }
}
