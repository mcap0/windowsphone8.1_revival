/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * A tiny HTTP server on 127.0.0.1 that serves one filtered HLS master playlist (only the H.264 variants up to
 * 720p, see HlsFilter). The MediaElement of WP8.1 cannot filter the variants itself, and AdaptiveMediaSource
 * is Windows 10 only. The playlist is served under a random name, one per playback.
 */

using System;
using System.Text;
using System.Threading.Tasks;
using Windows.Networking;
using Windows.Networking.Sockets;
using Windows.Storage.Streams;

namespace Perle81.App
{
    public static class PlaylistServer
    {
        const string Port = "38447";
        static StreamSocketListener listener;
        static string token = "";
        static byte[] body = new byte[0];

        /// <summary>Serves the playlist and returns the url to give to the player.</summary>
        public static async Task<Uri> ServeAsync(string playlist)
        {
            if (listener == null)
            {
                var l = new StreamSocketListener();
                l.ConnectionReceived += OnConnection;
                await l.BindEndpointAsync(new HostName("127.0.0.1"), Port);
                listener = l;
            }
            token = Guid.NewGuid().ToString("N");
            body = Encoding.UTF8.GetBytes(playlist);
            return new Uri("http://127.0.0.1:" + Port + "/" + token + ".m3u8");
        }

        static async void OnConnection(StreamSocketListener sender, StreamSocketListenerConnectionReceivedEventArgs args)
        {
            using (var socket = args.Socket)
            {
                try
                {
                    var reader = new DataReader(socket.InputStream);
                    var head = new StringBuilder();
                    while (head.Length < 8192)
                    {
                        if (await reader.LoadAsync(1) == 0) break;
                        head.Append((char)reader.ReadByte());
                        if (head.Length >= 4 && head.ToString(head.Length - 4, 4) == "\r\n\r\n") break;
                    }
                    reader.DetachStream();

                    var writer = new DataWriter(socket.OutputStream);
                    string line = head.ToString().Split('\n')[0];
                    if (line.StartsWith("GET /" + token + ".m3u8 ") && body.Length > 0)
                    {
                        writer.WriteString("HTTP/1.1 200 OK\r\nContent-Type: application/vnd.apple.mpegurl\r\nCache-Control: no-store\r\nContent-Length: "
                            + body.Length + "\r\nConnection: close\r\n\r\n");
                        writer.WriteBytes(body);
                    }
                    else
                    {
                        writer.WriteString("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    }
                    await writer.StoreAsync();
                    await writer.FlushAsync();
                    writer.DetachStream();
                }
                catch (Exception) { /* the player closed the connection: nothing to do */ }
            }
        }
    }
}
