/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.LanHttp and LocalNetwork.
 */

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;

namespace Perle.Cast.Dlna
{
    /// <summary>An HTTP answer read by LanHttp. Header names are lower case.</summary>
    public sealed class LanHttpResponse
    {
        public int Code { get; }
        public Dictionary<string, string> Headers { get; }
        public byte[] Body { get; }
        public LanHttpResponse(int code, Dictionary<string, string> headers, byte[] body) { Code = code; Headers = headers; Body = body; }
        public bool IsSuccessful => Code >= 200 && Code <= 299;
        public string BodyAsString() => Encoding.UTF8.GetString(Body);
    }

    /// <summary>
    /// Minimal blocking HTTP/1.1 client for talking to devices on the local network.
    /// It refuses any address that is not local (private, link-local or loopback), so it can never be
    /// used to reach the internet in clear text.
    /// </summary>
    public static class LanHttp
    {
        const int MaxBodyBytes = 4 * 1024 * 1024;
        const int MaxLineLength = 16 * 1024;

        public static LanHttpResponse Request(string url, string method = "GET", IList<KeyValuePair<string, string>> headers = null,
            byte[] body = null, int connectTimeoutMillis = 5000, int readTimeoutMillis = 10000)
        {
            Uri uri;
            try { uri = new Uri(url.Trim()); }
            catch (Exception e) { throw new IOException("Invalid URL: " + url, e); }
            if (!uri.Scheme.Equals("http", StringComparison.OrdinalIgnoreCase) || string.IsNullOrEmpty(uri.Host))
                throw new IOException("Only http:// URLs are supported on the local network: " + url);
            var address = uri.HostNameType == UriHostNameType.Dns ? Dns.GetHostAddresses(uri.Host).First() : IPAddress.Parse(uri.Host.Trim('[', ']'));
            if (!IsLocalAddress(address)) throw new IOException("Refusing to connect to non-local address " + address);
            int port = uri.IsDefaultPort ? 80 : uri.Port;
            var path = uri.PathAndQuery.Length > 0 ? uri.PathAndQuery : "/";
            var hostHeader = uri.IsDefaultPort ? uri.Host : uri.Host + ":" + port;

            using (var client = new TcpClient(address.AddressFamily))
            {
                var connect = client.ConnectAsync(address, port);
                if (!connect.Wait(connectTimeoutMillis)) throw new IOException("Connection to " + address + ":" + port + " timed out");
                if (connect.IsFaulted) throw new IOException("Could not connect to " + address + ":" + port, connect.Exception?.InnerException);
                client.ReceiveTimeout = readTimeoutMillis;
                client.SendTimeout = readTimeoutMillis;

                var head = new StringBuilder()
                    .Append(method).Append(' ').Append(path).Append(" HTTP/1.1\r\n")
                    .Append("Host: ").Append(hostHeader).Append("\r\n");
                if (headers != null) foreach (var h in headers) head.Append(h.Key).Append(": ").Append(h.Value).Append("\r\n");
                if (body != null) head.Append("Content-Length: ").Append(body.Length).Append("\r\n");
                head.Append("Connection: close\r\n\r\n");

                var output = client.GetStream();
                var headBytes = Encoding.UTF8.GetBytes(head.ToString());
                output.Write(headBytes, 0, headBytes.Length);
                if (body != null) output.Write(body, 0, body.Length);
                output.Flush();

                return ReadResponse(new BufferedStream(output), method == "HEAD");
            }
        }

        /// <returns>true for addresses that can only be on the local network: private IPv4 ranges,
        /// link-local, loopback and IPv6 unique local addresses (fc00::/7)</returns>
        public static bool IsLocalAddress(IPAddress address)
        {
            if (IPAddress.IsLoopback(address)) return true;
            var b = address.GetAddressBytes();
            if (address.AddressFamily == AddressFamily.InterNetwork)
                return b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168) || (b[0] == 169 && b[1] == 254);
            if (address.AddressFamily == AddressFamily.InterNetworkV6)
                // Java's isSiteLocalAddress is fec0::/10 for IPv6
                return address.IsIPv6LinkLocal || address.IsIPv6SiteLocal || (b[0] & 0xFE) == 0xFC;
            return false;
        }

        /// <summary>Reads status line, headers and body. Public so that it can be tested with a stream.</summary>
        public static LanHttpResponse ReadResponse(Stream input, bool isHead = false)
        {
            var statusLine = ReadLine(input) ?? throw new IOException("Empty answer");
            var parts = statusLine.Split(' ');
            if (parts.Length < 2 || !int.TryParse(parts[1], out int code)) throw new IOException("Malformed status line: " + statusLine);

            var headers = new Dictionary<string, string>();
            while (true)
            {
                var line = ReadLine(input);
                if (line == null || line.Length == 0) break;
                int colon = line.IndexOf(':');
                if (colon > 0) headers[line.Substring(0, colon).Trim().ToLowerInvariant()] = line.Substring(colon + 1).Trim();
            }

            byte[] body;
            if (isHead || code == 204 || code == 304) body = new byte[0];
            else if (headers.TryGetValue("transfer-encoding", out var te) && te.IndexOf("chunked", StringComparison.OrdinalIgnoreCase) >= 0) body = ReadChunked(input);
            else if (headers.TryGetValue("content-length", out var cl))
            {
                if (!int.TryParse(cl, out int length)) throw new IOException("Malformed Content-Length");
                if (length > MaxBodyBytes) throw new IOException("Answer too big: " + length + " bytes");
                body = ReadExactly(input, length);
            }
            else body = ReadUntilEnd(input);
            return new LanHttpResponse(code, headers, body);
        }

        static byte[] ReadChunked(Stream input)
        {
            var result = new MemoryStream();
            while (true)
            {
                var sizeLine = ReadLine(input) ?? throw new IOException("Unexpected end of chunked body");
                var sizeText = sizeLine.Split(';')[0].Trim();
                if (!int.TryParse(sizeText, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out int size)) throw new IOException("Malformed chunk size: " + sizeLine);
                if (size == 0)
                {
                    // skip trailers until the empty line
                    while (true)
                    {
                        var trailer = ReadLine(input);
                        if (string.IsNullOrEmpty(trailer)) break;
                    }
                    return result.ToArray();
                }
                if (result.Length + size > MaxBodyBytes) throw new IOException("Answer too big");
                var chunk = ReadExactly(input, size);
                result.Write(chunk, 0, chunk.Length);
                ReadLine(input); // the CRLF after the chunk
            }
        }

        static byte[] ReadExactly(Stream input, int length)
        {
            var buffer = new byte[length];
            int read = 0;
            while (read < length)
            {
                int count = input.Read(buffer, read, length - read);
                if (count <= 0) throw new IOException("Unexpected end of body (" + read + " of " + length + " bytes)");
                read += count;
            }
            return buffer;
        }

        static byte[] ReadUntilEnd(Stream input)
        {
            var result = new MemoryStream();
            var buffer = new byte[8192];
            while (true)
            {
                int count = input.Read(buffer, 0, buffer.Length);
                if (count <= 0) break;
                result.Write(buffer, 0, count);
                if (result.Length > MaxBodyBytes) throw new IOException("Answer too big");
            }
            return result.ToArray();
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
                line.Append((char)c); // ISO-8859-1
                if (line.Length > MaxLineLength) throw new IOException("Line too long");
            }
        }
    }

    /// <summary>
    /// The phone's address on the local network (usually Wi-Fi), which the TV uses to reach the
    /// proxy, and the interface SSDP requests are sent on.
    /// </summary>
    public sealed class LocalAddress
    {
        public string InterfaceName { get; }
        public IPAddress Address { get; }
        public LocalAddress(string interfaceName, IPAddress address) { InterfaceName = interfaceName; Address = address; }
    }

    public static class LocalNetwork
    {
        /// <summary>Replaced by the app with a WinRT based lookup if NetworkInterface is not available.</summary>
        public static Func<LocalAddress> Finder = FindWithNetworkInterface;

        /// <summary>
        /// Finds the IPv4 address of the phone on the local network. Wi-Fi interfaces are preferred,
        /// then other interfaces with a private address (Ethernet, hotspot, ...). Mobile data, tunnels
        /// and VPNs are skipped: carriers and VPNs often use private addresses too, but the TV cannot
        /// reach them.
        /// </summary>
        /// <returns>the address, or null if the phone is not connected to a local network</returns>
        public static LocalAddress Find() => Finder();

        static LocalAddress FindWithNetworkInterface()
        {
            var candidates = new List<KeyValuePair<int, LocalAddress>>();
            NetworkInterface[] interfaces;
            try { interfaces = NetworkInterface.GetAllNetworkInterfaces(); }
            catch (Exception e)
            {
                CastLog.E("Could not list network interfaces", e);
                return null;
            }
            foreach (var ni in interfaces)
            {
                try
                {
                    if (ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType == NetworkInterfaceType.Loopback
                        || ni.NetworkInterfaceType == NetworkInterfaceType.Tunnel || ni.NetworkInterfaceType == NetworkInterfaceType.Ppp
                        || ni.NetworkInterfaceType == NetworkInterfaceType.Wwanpp || ni.NetworkInterfaceType == NetworkInterfaceType.Wwanpp2)
                        continue;
                    foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                    {
                        var a = ua.Address;
                        if (a.AddressFamily == AddressFamily.InterNetwork && LanHttp.IsLocalAddress(a) && !IPAddress.IsLoopback(a) && a.GetAddressBytes()[0] != 169)
                            candidates.Add(new KeyValuePair<int, LocalAddress>(PriorityOf(ni.NetworkInterfaceType), new LocalAddress(ni.Name, a)));
                    }
                }
                catch (Exception e) { CastLog.W("Skipping interface " + ni.Name, e); }
            }
            CastLog.D("Local addresses: " + string.Join(", ", candidates.Select(c => c.Value.InterfaceName + "=" + c.Value.Address)));
            return candidates.OrderBy(c => c.Key).Select(c => c.Value).FirstOrDefault();
        }

        static int PriorityOf(NetworkInterfaceType type)
        {
            switch (type)
            {
                case NetworkInterfaceType.Wireless80211: return 0;
                case NetworkInterfaceType.Ethernet: return 1;
                default: return 3;
            }
        }
    }
}
