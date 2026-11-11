/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Progressive MP4 (muxed H.264 + AAC) streams from the InnerTube player endpoint with the ANDROID_VR and IOS
 * clients: they return direct URLs (no signature cipher). Perle 8.1 plays these with MediaElement; HLS does not
 * open on Windows Phone 8.1 (it needs Windows 10 MediaSource APIs).
 */

using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using Perle.Extractor;

namespace Perle.Progressive
{
    public sealed class ProgressiveFormat
    {
        public int Itag { get; set; }
        public string Url { get; set; }
        public int Height { get; set; }
        public string QualityLabel { get; set; }
        public string Client { get; set; }
    }

    public sealed class ProgressiveResult
    {
        public List<ProgressiveFormat> Formats { get; } = new List<ProgressiveFormat>();
        /// <summary>Why no format was found, per client (for the player screen).</summary>
        public List<string> Notes { get; } = new List<string>();
    }

    public static class ProgressivePlayer
    {
        sealed class ClientSpec
        {
            public string Name, Version, UserAgent, Device, Model, Os, OsVersion;
            public int? AndroidSdk;
            public int ClientId;
        }

        static readonly ClientSpec[] Clients =
        {
            new ClientSpec
            {
                Name = "ANDROID_VR", Version = "1.65.10", ClientId = 28, AndroidSdk = 32, Device = "Oculus", Model = "Quest 3",
                Os = "Android", OsVersion = "12L",
                UserAgent = "com.google.android.apps.youtube.vr.oculus/1.65.10 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip"
            },
            new ClientSpec
            {
                Name = "IOS", Version = "20.03.02", ClientId = 5, Device = "Apple", Model = "iPhone16,2",
                Os = "iPhone", OsVersion = "18.2.1.22C161",
                UserAgent = "com.google.ios.youtube/20.03.02 (iPhone16,2; U; CPU iOS 18_2_1 like Mac OS X;)"
            }
        };

        /// <summary>Tries each client until one returns muxed formats with a direct URL.</summary>
        public static async Task<ProgressiveResult> GetAsync(string videoId, CancellationToken ct = default(CancellationToken))
        {
            var result = new ProgressiveResult();
            foreach (var spec in Clients)
            {
                try
                {
                    var found = await RequestAsync(spec, videoId, ct).ConfigureAwait(false);
                    if (found.Count > 0)
                    {
                        result.Formats.AddRange(found);
                        return result;
                    }
                    result.Notes.Add(spec.Name + ": nessun flusso MP4 diretto");
                }
                catch (OperationCanceledException) { throw; }
                catch (Exception ex) { result.Notes.Add(spec.Name + ": " + ex.Message); }
            }
            return result;
        }

        static async Task<List<ProgressiveFormat>> RequestAsync(ClientSpec spec, string videoId, CancellationToken ct)
        {
            var client = new JObject
            {
                ["clientName"] = spec.Name,
                ["clientVersion"] = spec.Version,
                ["deviceMake"] = spec.Device,
                ["deviceModel"] = spec.Model,
                ["osName"] = spec.Os,
                ["osVersion"] = spec.OsVersion,
                ["hl"] = "en",
                ["gl"] = "US"
            };
            if (spec.AndroidSdk.HasValue) client["androidSdkVersion"] = spec.AndroidSdk.Value;
            var body = new JObject
            {
                ["context"] = new JObject { ["client"] = client },
                ["videoId"] = videoId,
                ["contentCheckOk"] = true,
                ["racyCheckOk"] = true
            };

            var formats = new List<ProgressiveFormat>();
            using (var req = new HttpRequestMessage(HttpMethod.Post, Innertube.YoutubeiV1GapisUrl + "player?" + Innertube.DisablePrettyPrint))
            {
                req.Headers.TryAddWithoutValidation("User-Agent", spec.UserAgent);
                req.Content = new StringContent(body.ToString(Formatting.None), Encoding.UTF8, "application/json");
                using (var resp = await Innertube.Http.SendAsync(req, ct).ConfigureAwait(false))
                {
                    string text = await resp.Content.ReadAsStringAsync().ConfigureAwait(false);
                    if (!resp.IsSuccessStatusCode) throw new ExtractionException("HTTP " + (int)resp.StatusCode);
                    var json = JObject.Parse(text);
                    var status = json.Obj("playabilityStatus").Str("status");
                    if (status != "OK") throw new ExtractionException(json.Obj("playabilityStatus").Str("reason") ?? status ?? "non riproducibile");

                    foreach (var f in json.Obj("streamingData").Arr("formats").Objects())
                    {
                        string mime = f.Str("mimeType") ?? "";
                        string url = f.Str("url");
                        if (url == null || !mime.Contains("avc1") || !mime.Contains("mp4a")) continue;
                        formats.Add(new ProgressiveFormat
                        {
                            Itag = (int)f.Long("itag", 0),
                            Url = url,
                            Height = (int)f.Long("height", 0),
                            QualityLabel = f.Str("qualityLabel"),
                            Client = spec.Name
                        });
                    }
                }
            }
            return formats;
        }
    }
}
