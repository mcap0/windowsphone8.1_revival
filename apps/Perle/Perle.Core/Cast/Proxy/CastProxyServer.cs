/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.proxy.CastProxyServer and HttpRequest.
 */

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using Perle.Cast.Ts;

namespace Perle.Cast.Proxy
{
    /// <summary>
    /// A tiny HTTP/1.1 server running on the phone, which forwards requests from the TV to the real
    /// video URL.
    ///
    /// It is needed because many DLNA renderers cannot play HTTPS URLs, have problems with very long
    /// URLs (googlevideo ones are more than 1000 characters long) or cannot send the extra headers
    /// YouTube requires. The TV gets a short http://&lt;phone ip&gt;:&lt;port&gt;/v/&lt;token&gt;.mp4 URL instead.
    ///
    /// GET and HEAD are supported, as well as Range requests (answered with 206 Partial Content),
    /// which TVs use to seek. Only registered URLs are served, under a random token, so the server
    /// cannot be used by others on the network as an open proxy.
    /// </summary>
    public sealed class CastProxyServer
    {
        const int ClientTimeoutMillis = 30000;
        const int BufferSize = 64 * 1024;
        const string Server = "Windows UPnP/1.0 NewPipe-Cast/1.0";

        /// <summary>DLNA.ORG_OP=01: seeking by byte range is supported; FLAGS: streaming transfer mode.</summary>
        public const string DlnaContentFeatures = "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000";

        /// <summary>A registered URL, or separate video and audio streams muxed live if Muxer is not null.</summary>
        sealed class Entry
        {
            public string Url, MimeType;
            public LiveTsMuxer Muxer;
            public string DlnaFeatures = DlnaContentFeatures;
            /// <summary>content kept in memory (subtitles), served instead of Url</summary>
            public byte[] Bytes;
            /// <summary>subtitles announced with the Samsung CaptionInfo.sec header</summary>
            public string CaptionUrl;
        }

        readonly IUpstream upstream, dashUpstream;
        readonly IPAddress bindAddress;
        readonly ConcurrentDictionary<string, Entry> entries = new ConcurrentDictionary<string, Entry>();
        readonly RandomNumberGenerator random = RandomNumberGenerator.Create();
        readonly object gate = new object();
        volatile TcpListener listener;

        /// <param name="upstream">how to download the real content</param>
        /// <param name="bindAddress">the local address to listen on (the Wi-Fi address of the phone)</param>
        /// <param name="dashUpstream">how to download the separate video and audio streams which are muxed</param>
        public CastProxyServer(IUpstream upstream, IPAddress bindAddress, IUpstream dashUpstream = null)
        {
            this.upstream = upstream; this.bindAddress = bindAddress; this.dashUpstream = dashUpstream ?? upstream;
        }

        public int Port => listener != null ? ((IPEndPoint)listener.LocalEndpoint).Port : -1;

        public bool IsRunning => listener != null;

        /// <summary>Starts listening on a free port. Does nothing if already running.</summary>
        public void Start()
        {
            lock (gate)
            {
                if (IsRunning) return;
                var l = new TcpListener(bindAddress, 0);
                l.Server.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                l.Start();
                listener = l;
                Task.Run(() => AcceptLoop(l));
                CastLog.D("Proxy: listening on " + bindAddress + ":" + Port);
            }
        }

        public void Stop()
        {
            lock (gate)
            {
                try { listener?.Stop(); } catch (SocketException) { /* already closed */ }
                listener = null;
                entries.Clear();
                CastLog.D("Proxy: stopped");
            }
        }

        /// <summary>Makes url available through the proxy.</summary>
        /// <returns>the URL to give to the TV</returns>
        public string Register(string url, string mimeType, string captionUrl = null) =>
            Add(new Entry { Url = url, MimeType = mimeType, CaptionUrl = captionUrl });

        /// <summary>Makes content (e.g. SRT subtitles) available through the proxy.</summary>
        /// <returns>the URL to give to the TV</returns>
        public string RegisterContent(byte[] content, string mimeType, string extension) =>
            Add(new Entry { Url = "", MimeType = mimeType, Bytes = content }, extension);

        /// <summary>
        /// Makes the video and audio streams of tracks available as a single MPEG-TS stream, muxed
        /// while it is sent. The returned URL accepts ?t=&lt;seconds&gt; to start from another time.
        /// </summary>
        /// <param name="format">the MIME type and DLNA profile the TV accepts</param>
        /// <returns>the URL to give to the TV</returns>
        public string RegisterMux(CastDash tracks, TsFormat format, string captionUrl = null) =>
            Add(new Entry
            {
                Url = tracks.Video.Url, MimeType = format.MimeType, Muxer = new LiveTsMuxer(dashUpstream, tracks),
                DlnaFeatures = format.DlnaFeatures, CaptionUrl = captionUrl
            }, "ts");

        string Add(Entry entry, string extension = null)
        {
            var bytes = new byte[12];
            random.GetBytes(bytes);
            var token = string.Concat(bytes.Select(b => b.ToString("x2", CultureInfo.InvariantCulture)));
            entries[token] = entry;
            return "http://" + HostForUrl() + ":" + Port + "/v/" + token + "." + (extension ?? ExtensionFor(entry.MimeType));
        }

        string HostForUrl()
        {
            var host = bindAddress.ToString();
            return host.Contains(':') ? "[" + host + "]" : host;
        }

        async Task AcceptLoop(TcpListener l)
        {
            while (listener == l)
            {
                try
                {
                    var client = await l.AcceptTcpClientAsync().ConfigureAwait(false);
                    var _ = Task.Run(() => HandleClient(client));
                }
                catch (ObjectDisposedException) { break; }
                catch (SocketException e)
                {
                    if (listener == l) CastLog.E("Proxy: accept failed", e);
                    else break;
                }
                catch (Exception e) { CastLog.E("Proxy: accept failed", e); }
            }
        }

        void HandleClient(TcpClient client)
        {
            using (client)
            {
                try
                {
                    client.ReceiveTimeout = ClientTimeoutMillis;
                    client.SendTimeout = ClientTimeoutMillis;
                    var stream = client.GetStream();
                    var input = new BufferedStream(stream);
                    var request = HttpRequest.Read(input);
                    if (request == null) return;
                    Handle(request, (client.Client.RemoteEndPoint as IPEndPoint)?.Address.ToString(), stream);
                }
                catch (Exception e) when (IsClosedByClient(e))
                {
                    // the TV closed the connection, e.g. because it seeks: this is normal
                    CastLog.D("Proxy: connection closed by client (" + e.Message + ")");
                }
                catch (Exception e)
                {
                    CastLog.E("Proxy: error while handling a request", e);
                }
            }
        }

        /// <summary>SocketException in Kotlin: on .NET a broken connection surfaces as IOException wrapping it.</summary>
        static bool IsClosedByClient(Exception e) => e is SocketException || e.InnerException is SocketException;

        /// <summary>Handles one request. Public so that it can be tested without sockets.</summary>
        public void Handle(HttpRequest request, string clientAddress, Stream output)
        {
            var rangeHeader = request.Header("Range");
            CastLog.D("Proxy <- " + request.Method + " " + request.Path + " from " + clientAddress + (rangeHeader != null ? " (Range: " + rangeHeader + ")" : ""));
            if (request.Method != "GET" && request.Method != "HEAD")
            {
                WriteSimple(output, 405, "Method Not Allowed", "Allow: GET, HEAD\r\n");
                return;
            }
            var pathNoQuery = request.Path.Split('?')[0];
            var token = (pathNoQuery.StartsWith("/v/") ? pathNoQuery.Substring(3) : pathNoQuery).Split('.')[0];
            if (!request.Path.StartsWith("/v/") || !entries.TryGetValue(token, out var entry))
            {
                WriteSimple(output, 404, "Not Found");
                return;
            }
            if (entry.Muxer != null)
            {
                HandleMux(request, entry, entry.Muxer, output);
                return;
            }
            if (entry.Bytes != null)
            {
                WriteHeaders(output, 200, "OK", entry.MimeType, entry.Bytes.Length, null);
                if (request.Method == "GET") output.Write(entry.Bytes, 0, entry.Bytes.Length);
                output.Flush();
                return;
            }

            HttpRange range;
            try { range = HttpRange.Parse(rangeHeader); }
            catch (ArgumentException e)
            {
                CastLog.W("Proxy: " + e.Message);
                WriteSimple(output, 416, "Range Not Satisfiable");
                return;
            }
            bool isHead = request.Method == "HEAD";

            // For HEAD requests without range only the first byte is asked, to learn the total
            // length from Content-Range without downloading the whole file
            var upstreamRange = isHead && range == null ? new HttpRange(0, 0) : range;
            UpstreamResponse upstreamResponse;
            try { upstreamResponse = upstream.Open(entry.Url, upstreamRange); }
            catch (Exception e) when (e is IOException || e is System.Net.Http.HttpRequestException || e is TaskCanceledException)
            {
                CastLog.E("Proxy: could not reach the video server", e);
                WriteSimple(output, 502, "Bad Gateway");
                return;
            }
            using (var response = upstreamResponse)
            {
                CastLog.D("Proxy: upstream HTTP " + response.Code + ", length " + response.ContentLength + ", range " + response.ContentRange + ", type " + response.ContentType);
                if (response.Code == 416)
                {
                    long total = HttpRange.TotalLengthFromContentRange(response.ContentRange);
                    WriteSimple(output, 416, "Range Not Satisfiable", total >= 0 ? "Content-Range: bytes */" + total + "\r\n" : "");
                }
                else if (response.Code < 200 || response.Code > 299)
                {
                    WriteSimple(output, 502, "Bad Gateway");
                }
                else if (isHead && range == null)
                {
                    long total = response.Code == 206 ? HttpRange.TotalLengthFromContentRange(response.ContentRange) : response.ContentLength;
                    WriteHeaders(output, 200, "OK", entry.MimeType, total, null, entry.CaptionUrl);
                }
                else
                {
                    bool partial = response.Code == 206;
                    // if the upstream ignored the range, the whole file is sent with 200
                    WriteHeaders(output, partial ? 206 : 200, partial ? "Partial Content" : "OK", entry.MimeType,
                        response.ContentLength, partial ? response.ContentRange : null, entry.CaptionUrl);
                    if (!isHead && response.Body != null) Copy(response.Body, output);
                }
                output.Flush();
            }
        }

        /// <summary>
        /// Sends the live MPEG-TS stream. Its length is unknown, so it is sent until the end without
        /// Content-Length, and byte ranges other than "from the start" cannot be served: the TV seeks
        /// by asking for a new stream with ?t=&lt;seconds&gt;.
        /// </summary>
        void HandleMux(HttpRequest request, Entry entry, LiveTsMuxer muxer, Stream output)
        {
            var header = request.Header("Range");
            HttpRange range = null;
            try { range = HttpRange.Parse(header); } catch (ArgumentException) { }
            if (header != null && (range == null || range.Start != 0 || range.End != null))
            {
                CastLog.W("Proxy: range " + header + " not possible on a live stream");
                WriteSimple(output, 416, "Range Not Satisfiable");
                return;
            }
            double start = StartSeconds(request.Path);
            WriteStreamHeaders(output, entry.MimeType, entry.DlnaFeatures, entry.CaptionUrl);
            if (request.Method == "HEAD")
            {
                output.Flush();
                return;
            }
            try
            {
                muxer.Write(new BufferedStream(output, BufferSize), start);
            }
            catch (Exception e) when (IsClosedByClient(e))
            {
                CastLog.D("Proxy: TV closed the live stream (" + e.Message + ")");
            }
            catch (IOException e)
            {
                // the headers are already sent, closing the connection is the only way to tell
                CastLog.E("Proxy: live stream failed", e);
            }
        }

        static void WriteAscii(Stream output, string text)
        {
            var bytes = Latin1(text);
            output.Write(bytes, 0, bytes.Length);
        }

        /// <summary>ISO-8859-1, which .NET Native lacks: every char below 256 is one byte.</summary>
        public static byte[] Latin1(string s)
        {
            var b = new byte[s.Length];
            for (int i = 0; i < s.Length; i++) b[i] = s[i] < 256 ? (byte)s[i] : (byte)'?';
            return b;
        }

        static void WriteStreamHeaders(Stream output, string contentType, string dlnaFeatures, string captionUrl)
        {
            var headers = "HTTP/1.1 200 OK\r\n"
                + "Content-Type: " + contentType + "\r\n"
                + "transferMode.dlna.org: Streaming\r\n"
                + CaptionHeader(captionUrl)
                + "contentFeatures.dlna.org: " + dlnaFeatures + "\r\n"
                + "Server: " + Server + "\r\n"
                + "Connection: close\r\n"
                + "\r\n";
            CastLog.D("Proxy -> 200 OK, live stream " + contentType);
            WriteAscii(output, headers);
        }

        static void WriteHeaders(Stream output, int code, string reason, string contentType, long contentLength, string contentRange, string captionUrl = null)
        {
            var headers = new StringBuilder()
                .Append("HTTP/1.1 ").Append(code).Append(' ').Append(reason).Append("\r\n")
                .Append("Content-Type: ").Append(contentType).Append("\r\n")
                .Append("Accept-Ranges: bytes\r\n");
            if (contentLength >= 0) headers.Append("Content-Length: ").Append(contentLength).Append("\r\n");
            if (contentRange != null) headers.Append("Content-Range: ").Append(contentRange).Append("\r\n");
            headers.Append(CaptionHeader(captionUrl))
                .Append("transferMode.dlna.org: Streaming\r\n")
                .Append("contentFeatures.dlna.org: ").Append(DlnaContentFeatures).Append("\r\n")
                .Append("Server: ").Append(Server).Append("\r\n")
                .Append("Connection: close\r\n")
                .Append("\r\n");
            CastLog.D("Proxy -> " + code + " " + reason + ", " + contentType + ", length " + contentLength + (contentRange != null ? ", " + contentRange : ""));
            WriteAscii(output, headers.ToString());
        }

        static string CaptionHeader(string captionUrl) => captionUrl != null ? "CaptionInfo.sec: " + captionUrl + "\r\n" : "";

        static void WriteSimple(Stream output, int code, string reason, string extraHeaders = "")
        {
            CastLog.D("Proxy -> HTTP " + code + " " + reason);
            WriteAscii(output, "HTTP/1.1 " + code + " " + reason + "\r\n"
                + extraHeaders
                + "Content-Length: 0\r\n"
                + "Server: " + Server + "\r\n"
                + "Connection: close\r\n"
                + "\r\n");
            output.Flush();
        }

        static void Copy(Stream input, Stream output)
        {
            var buffer = new byte[BufferSize];
            while (true)
            {
                int read = input.Read(buffer, 0, buffer.Length);
                if (read <= 0) break;
                output.Write(buffer, 0, read);
            }
        }

        /// <returns>the start time asked with ?t=&lt;seconds&gt;, 0 if absent</returns>
        public static double StartSeconds(string path)
        {
            int q = path.IndexOf('?');
            if (q < 0) return 0.0;
            var t = path.Substring(q + 1).Split('&').FirstOrDefault(p => p.StartsWith("t="));
            if (t == null || !double.TryParse(t.Substring(2), NumberStyles.Float, CultureInfo.InvariantCulture, out var v)) return 0.0;
            return Math.Max(v, 0.0);
        }

        public static string ExtensionFor(string mimeType)
        {
            switch (mimeType.ToLowerInvariant())
            {
                case "video/mp4": return "mp4";
                case "video/webm": return "webm";
                case "audio/mp4":
                case "audio/m4a": return "m4a";
                case "video/3gpp": return "3gp";
                default: return "bin";
            }
        }
    }

    /// <summary>A parsed HTTP request: only what the proxy needs.</summary>
    public sealed class HttpRequest
    {
        const int MaxHeaderBytes = 16 * 1024;

        public string Method { get; }
        public string Path { get; }
        /// <summary>the headers, with lower case names</summary>
        public Dictionary<string, string> Headers { get; }

        public HttpRequest(string method, string path, Dictionary<string, string> headers) { Method = method; Path = path; Headers = headers; }

        public string Header(string name) => Headers.TryGetValue(name.ToLowerInvariant(), out var v) ? v : null;

        /// <summary>Reads the request line and headers from input.</summary>
        /// <returns>the request, or null if the connection was closed before a request arrived</returns>
        /// <exception cref="IOException">if the request is malformed or too big</exception>
        public static HttpRequest Read(Stream input)
        {
            var requestLine = ReadLine(input);
            if (requestLine == null) return null;
            var parts = requestLine.Split(' ');
            if (parts.Length < 2) throw new IOException("Malformed request line: " + requestLine);
            var headers = new Dictionary<string, string>();
            int total = requestLine.Length;
            while (true)
            {
                var line = ReadLine(input);
                if (line == null || line.Length == 0) break;
                total += line.Length;
                if (total > MaxHeaderBytes) throw new IOException("Request headers too big");
                int colon = line.IndexOf(':');
                if (colon > 0) headers[line.Substring(0, colon).Trim().ToLowerInvariant()] = line.Substring(colon + 1).Trim();
            }
            return new HttpRequest(parts[0].ToUpperInvariant(), parts[1], headers);
        }

        static string ReadLine(Stream input)
        {
            var line = new StringBuilder();
            while (true)
            {
                int c = input.ReadByte();
                if (c < 0) return line.Length == 0 ? null : line.ToString();
                if (c == '\n')
                {
                    var s = line.ToString();
                    return s.EndsWith("\r") ? s.Substring(0, s.Length - 1) : s;
                }
                line.Append((char)c);
                if (line.Length > MaxHeaderBytes) throw new IOException("Request line too long");
            }
        }
    }
}
