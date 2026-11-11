/*
 * SPDX-FileCopyrightText: 2026 NewPipe contributors <https://newpipe.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of org.schabi.newpipe.cast.dlna.Ssdp.
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading.Tasks;
#if !WINDOWS_PHONE_APP
using System.Net.Sockets;
#else
using Windows.Networking;
using Windows.Networking.Connectivity;
using Windows.Networking.Sockets;
using Windows.Storage.Streams;
#endif

namespace Perle.Cast.Dlna
{
    /// <summary>One answer to a M-SEARCH, i.e. one device (or service) announcing itself.</summary>
    public sealed class SsdpResponse
    {
        /// <summary>The URL of the device description XML.</summary>
        public string Location { get; set; }
        /// <summary>The ST header, e.g. urn:schemas-upnp-org:device:MediaRenderer:1.</summary>
        public string SearchTarget { get; set; }
        /// <summary>The unique service name, used to recognize duplicates.</summary>
        public string Usn { get; set; }
        /// <summary>The SERVER header, only useful for the log.</summary>
        public string Server { get; set; }
    }

    /// <summary>
    /// SSDP (Simple Service Discovery Protocol), the discovery part of UPnP: a M-SEARCH request is
    /// sent via UDP multicast to 239.255.255.250:1900 and devices answer via unicast to the sender.
    /// </summary>
    public static class Ssdp
    {
        public const string MulticastAddress = "239.255.255.250";
        public const int Port = 1900;
        public const string MediaRenderer = "urn:schemas-upnp-org:device:MediaRenderer:1";
        public const string AvTransport = "urn:schemas-upnp-org:service:AVTransport:1";

        /// <summary>How long a device can wait before answering, in seconds (the MX header).</summary>
        public const int MxSeconds = 2;

        /// <summary>Builds a M-SEARCH request. The trailing empty line is required by HTTP-over-UDP.</summary>
        public static string BuildSearchRequest(string searchTarget, int mxSeconds = MxSeconds) =>
            "M-SEARCH * HTTP/1.1\r\n"
            + "HOST: " + MulticastAddress + ":" + Port + "\r\n"
            + "MAN: \"ssdp:discover\"\r\n"
            + "MX: " + mxSeconds + "\r\n"
            + "ST: " + searchTarget + "\r\n"
            + "USER-AGENT: Windows UPnP/1.1 NewPipe/1.0\r\n"
            + "\r\n";

        /// <summary>
        /// Parses an answer to a M-SEARCH (HTTP/1.1 200 OK) or a NOTIFY announcement.
        /// Header names are case insensitive, as some devices write Location or location.
        /// </summary>
        /// <returns>The parsed response, or null if it is not a valid answer with a LOCATION header.</returns>
        public static SsdpResponse ParseResponse(string message)
        {
            var lines = message.Split(new[] { "\r\n", "\n" }, StringSplitOptions.None);
            var statusLine = lines.FirstOrDefault()?.Trim();
            if (statusLine == null) return null;
            var statusParts = statusLine.Split(' ');
            bool isSearchAnswer = statusLine.StartsWith("HTTP/", StringComparison.OrdinalIgnoreCase)
                && statusParts.Length > 1 && statusParts[1] == "200";
            bool isNotify = statusLine.StartsWith("NOTIFY", StringComparison.OrdinalIgnoreCase);
            if (!isSearchAnswer && !isNotify) return null;

            var headers = new Dictionary<string, string>();
            foreach (var line in lines.Skip(1))
            {
                int colon = line.IndexOf(':');
                if (colon <= 0) continue;
                var name = line.Substring(0, colon).Trim().ToUpperInvariant();
                if (!headers.ContainsKey(name)) headers[name] = line.Substring(colon + 1).Trim();
            }

            string nts;
            if (isNotify && headers.TryGetValue("NTS", out nts)
                && nts.Equals("ssdp:byebye", StringComparison.OrdinalIgnoreCase)) return null;
            string location;
            if (!headers.TryGetValue("LOCATION", out location) || location.Length == 0) return null;
            string st;
            string nt;
            string usn;
            string server;
            headers.TryGetValue("ST", out st);
            headers.TryGetValue("NT", out nt);
            headers.TryGetValue("USN", out usn);
            headers.TryGetValue("SERVER", out server);
            return new SsdpResponse { Location = location, SearchTarget = st ?? nt, Usn = usn, Server = server };
        }

        /// <summary>
        /// Sends M-SEARCH requests and collects the answers for timeoutMillis. Call this from a
        /// background thread; WP8.1 uses DatagramSocket while other targets use a UDP socket.
        /// </summary>
        public static void Search(IPAddress localAddress, Action<SsdpResponse> onResponse,
            IList<string> searchTargets = null, long timeoutMillis = 4000)
        {
            searchTargets = searchTargets ?? new[] { MediaRenderer, AvTransport };
#if WINDOWS_PHONE_APP
            SearchWinRt(localAddress, onResponse, searchTargets, timeoutMillis);
#else
            SearchSocket(localAddress, onResponse, searchTargets, timeoutMillis);
#endif
        }

#if !WINDOWS_PHONE_APP
        static void SearchSocket(IPAddress localAddress, Action<SsdpResponse> onResponse,
            IList<string> searchTargets, long timeoutMillis)
        {
            var seen = new HashSet<string>();
            using (var socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp))
            {
                socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
                socket.Bind(new IPEndPoint(localAddress ?? IPAddress.Any, 0));
                socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastTimeToLive, 4);
                if (localAddress != null)
                {
                    try { socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastInterface, localAddress.GetAddressBytes()); }
                    catch (SocketException e) { CastLog.W("SSDP: could not use interface " + localAddress, e); }
                }
                var group = new IPEndPoint(IPAddress.Parse(MulticastAddress), Port);
                long start = Environment.TickCount;
                long deadline = start + timeoutMillis;
                var sendTimes = new[] { 0L, 1000L, 2000L }.Select(t => start + t).ToArray();
                int nextSend = 0;
                var buffer = new byte[4096];

                while (true)
                {
                    long now = Environment.TickCount;
                    if (now >= deadline) break;
                    if (nextSend < sendTimes.Length && now >= sendTimes[nextSend])
                    {
                        foreach (var target in searchTargets)
                        {
                            var data = Encoding.UTF8.GetBytes(BuildSearchRequest(target));
                            socket.SendTo(data, group);
                        }
                        CastLog.D("SSDP: sent M-SEARCH #" + (nextSend + 1) + " for [" + string.Join(", ", searchTargets) + "]");
                        nextSend++;
                    }

                    long waitUntil = nextSend < sendTimes.Length ? Math.Min(deadline, sendTimes[nextSend]) : deadline;
                    socket.ReceiveTimeout = (int)Math.Max(waitUntil - now, 1);
                    EndPoint from = new IPEndPoint(IPAddress.Any, 0);
                    int length;
                    try { length = socket.ReceiveFrom(buffer, ref from); }
                    catch (SocketException e) when (e.SocketErrorCode == SocketError.TimedOut) { continue; }

                    var text = Encoding.UTF8.GetString(buffer, 0, length);
                    var response = ParseResponse(text);
                    var sender = (from as IPEndPoint)?.Address;
                    if (response == null)
                    {
                        CastLog.D("SSDP: unexpected answer from " + sender + ":\n" + text.Trim());
                        continue;
                    }
                    if (seen.Add(response.Location))
                    {
                        CastLog.D("SSDP: answer from " + sender + ": " + response.Location);
                        onResponse(response);
                    }
                }
            }
        }
#else
        static void SearchWinRt(IPAddress localAddress, Action<SsdpResponse> onResponse,
            IList<string> searchTargets, long timeoutMillis)
        {
            var socket = new DatagramSocket();
            var seen = new HashSet<string>();
            var seenGate = new object();
            socket.MessageReceived += (sender, args) =>
            {
                string text;
                using (var reader = args.GetDataReader())
                    text = reader.ReadString(reader.UnconsumedBufferLength);
                var response = ParseResponse(text);
                if (response == null)
                {
                    CastLog.D("SSDP: unexpected answer from " + args.RemoteAddress + ":\n" + text.Trim());
                    return;
                }
                bool isNew;
                lock (seenGate) isNew = seen.Add(response.Location);
                if (!isNew) return;
                CastLog.D("SSDP: answer from " + args.RemoteAddress + ": " + response.Location);
                try { onResponse(response); }
                catch (Exception e) { CastLog.E("SSDP: response callback failed", e); }
            };

            try
            {
                HostName localHost = null;
                if (localAddress != null)
                {
                    localHost = NetworkInformation.GetHostNames().FirstOrDefault(h =>
                        h.Type == HostNameType.Ipv4 &&
                        string.Equals(h.CanonicalName, localAddress.ToString(), StringComparison.OrdinalIgnoreCase));
                }

                if (localHost != null) socket.BindEndpointAsync(localHost, "0").AsTask().GetAwaiter().GetResult();
                else socket.BindServiceNameAsync("0").AsTask().GetAwaiter().GetResult();
                socket.JoinMulticastGroup(new HostName(MulticastAddress));

                long start = DateTime.UtcNow.Ticks / TimeSpan.TicksPerMillisecond;
                long deadline = start + timeoutMillis;
                var sendTimes = new[] { start, start + 1000, start + 2000 };
                int nextSend = 0;

                while (true)
                {
                    long now = DateTime.UtcNow.Ticks / TimeSpan.TicksPerMillisecond;
                    if (now >= deadline) break;
                    if (nextSend < sendTimes.Length && now >= sendTimes[nextSend])
                    {
                        foreach (var target in searchTargets)
                        {
                            using (var output = socket.GetOutputStreamAsync(
                                new HostName(MulticastAddress), Port.ToString()).AsTask().GetAwaiter().GetResult())
                            using (var writer = new DataWriter(output))
                            {
                                writer.WriteString(BuildSearchRequest(target));
                                writer.StoreAsync().AsTask().GetAwaiter().GetResult();
                                writer.FlushAsync().AsTask().GetAwaiter().GetResult();
                            }
                        }
                        CastLog.D("SSDP: sent M-SEARCH #" + (nextSend + 1) + " for [" + string.Join(", ", searchTargets) + "]");
                        nextSend++;
                        continue;
                    }

                    long waitUntil = nextSend < sendTimes.Length ? Math.Min(deadline, sendTimes[nextSend]) : deadline;
                    int wait = (int)Math.Max(1, waitUntil - now);
                    Task.Delay(wait).Wait();
                }
            }
            finally
            {
                socket.Dispose();
            }
        }
#endif
    }
}
