/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of ClientsConstants, InnertubeClientRequestInfo and the request helpers of YoutubeParsingHelper
 * (NewPipeExtractor 3313901).
 */

using System;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace Perle.Extractor
{
    public class ExtractionException : Exception
    {
        public ExtractionException(string message, Exception inner = null) : base(message, inner) { }
    }

    public sealed class ClientInfo
    {
        public string ClientName, ClientVersion, ClientId, ClientScreen, VisitorData;
        public string Platform, DeviceMake, DeviceModel, OsName, OsVersion;
    }

    public static class Innertube
    {
        public const string YoutubeiV1Url = "https://www.youtube.com/youtubei/v1/";
        public const string YoutubeiV1GapisUrl = "https://youtubei.googleapis.com/youtubei/v1/";
        public const string DisablePrettyPrint = "prettyPrint=false";

        public const string WebClientId = "1";
        public const string WebClientName = "WEB";
        public const string WebClientVersion = "2.20260805.01.00";

        public const string VisionOsClientId = "101";
        public const string VisionOsClientName = "VISIONOS";
        public const string VisionOsClientVersion = "1.04";
        public const string VisionOsDeviceModel = "RealityDevice17,1";
        public const string VisionOsVersion = "26.6.0.23O770";
        public const string VisionOsUserAgentVersion = "26_6_0";

        /// <summary>Localization.DEFAULT of NewPipeExtractor: the text parsers below expect en-GB.</summary>
        public const string Hl = "en-GB";
        public const string Gl = "GB";

        const string CpnAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

        static readonly Random random = new Random();

        public static HttpClient Http { get; set; } = CreateHttpClient();

        public static HttpClient CreateHttpClient()
        {
            var handler = new HttpClientHandler { AutomaticDecompression = DecompressionMethods.GZip | DecompressionMethods.Deflate };
            var client = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(30) };
            return client;
        }

        public static ClientInfo WebClient() => new ClientInfo
        {
            ClientName = WebClientName, ClientVersion = WebClientVersion, ClientId = WebClientId,
            ClientScreen = "WATCH", Platform = "DESKTOP"
        };

        public static ClientInfo VisionOsClient() => new ClientInfo
        {
            ClientName = VisionOsClientName, ClientVersion = VisionOsClientVersion, ClientId = VisionOsClientId,
            ClientScreen = "WATCH", Platform = "MOBILE", DeviceMake = "Apple", DeviceModel = VisionOsDeviceModel,
            OsName = "visionOS", OsVersion = VisionOsVersion
        };

        public static string VisionOsUserAgent =>
            "com.google.visionos.youtube/" + VisionOsClientVersion + "(" + VisionOsDeviceModel
            + "; U; CPU visionOS " + VisionOsUserAgentVersion + " like Mac OS X; " + Gl + ")";

        /// <summary>Firefox ESR user agent used by NewPipe for web requests.</summary>
        public const string BrowserUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:153.0) Gecko/20100101 Firefox/153.0";

        public static string GenerateCpn() => RandomString(16);

        public static string GenerateTParameter() => RandomString(12);

        static string RandomString(int length)
        {
            var sb = new StringBuilder(length);
            lock (random)
                for (int i = 0; i < length; i++) sb.Append(CpnAlphabet[random.Next(CpnAlphabet.Length)]);
            return sb.ToString();
        }

        /// <summary>Port of prepareJsonBuilder.</summary>
        public static JObject PrepareBody(ClientInfo c)
        {
            var client = new JObject
            {
                ["clientName"] = c.ClientName,
                ["clientVersion"] = c.ClientVersion
            };
            if (c.ClientScreen != null) client["clientScreen"] = c.ClientScreen;
            if (c.Platform != null) client["platform"] = c.Platform;
            if (c.VisitorData != null) client["visitorData"] = c.VisitorData;
            if (c.DeviceMake != null) client["deviceMake"] = c.DeviceMake;
            if (c.DeviceModel != null) client["deviceModel"] = c.DeviceModel;
            if (c.OsName != null) client["osName"] = c.OsName;
            if (c.OsVersion != null) client["osVersion"] = c.OsVersion;
            client["hl"] = Hl;
            client["gl"] = Gl;
            client["utcOffsetMinutes"] = 0;

            return new JObject
            {
                ["context"] = new JObject
                {
                    ["client"] = client,
                    ["request"] = new JObject { ["internalExperimentFlags"] = new JArray(), ["useSsl"] = true },
                    ["user"] = new JObject { ["lockedSafetyMode"] = false }
                }
            };
        }

        /// <summary>Headers of getYouTubeHeaders: client info, origin/referer and the CONSENT cookie.</summary>
        static void AddWebHeaders(HttpRequestMessage req)
        {
            req.Headers.TryAddWithoutValidation("X-YouTube-Client-Name", WebClientId);
            req.Headers.TryAddWithoutValidation("X-YouTube-Client-Version", WebClientVersion);
            req.Headers.TryAddWithoutValidation("Origin", "https://www.youtube.com");
            req.Headers.TryAddWithoutValidation("Referer", "https://www.youtube.com");
            req.Headers.TryAddWithoutValidation("Cookie", "SOCS=CAE=");
            req.Headers.TryAddWithoutValidation("User-Agent", BrowserUserAgent);
        }

        static void AddVisionOsHeaders(HttpRequestMessage req)
        {
            req.Headers.TryAddWithoutValidation("User-Agent", VisionOsUserAgent);
            req.Headers.TryAddWithoutValidation("X-Goog-Api-Format-Version", "2");
        }

        public static async Task<JObject> PostAsync(string url, JObject body, bool visionOs, CancellationToken ct)
        {
            using (var req = new HttpRequestMessage(HttpMethod.Post, url))
            {
                if (visionOs) AddVisionOsHeaders(req); else AddWebHeaders(req);
                req.Content = new StringContent(body.ToString(Formatting.None), Encoding.UTF8, "application/json");
                using (var resp = await Http.SendAsync(req, ct).ConfigureAwait(false))
                {
                    string text = await resp.Content.ReadAsStringAsync().ConfigureAwait(false);
                    if (!resp.IsSuccessStatusCode)
                        throw new ExtractionException("HTTP " + (int)resp.StatusCode + " da " + url);
                    if (text.Length == 0 || text[0] != '{')
                        throw new ExtractionException("Risposta non JSON da " + url);
                    return JObject.Parse(text);
                }
            }
        }

        /// <summary>Port of getVisitorDataFromInnertube (visitor_id endpoint).</summary>
        public static async Task<string> GetVisitorDataAsync(ClientInfo c, string baseUrl, bool visionOs, CancellationToken ct)
        {
            var json = await PostAsync(baseUrl + "visitor_id?" + DisablePrettyPrint, PrepareBody(c), visionOs, ct).ConfigureAwait(false);
            var visitorData = json.Obj("responseContext").Str("visitorData");
            if (string.IsNullOrEmpty(visitorData)) throw new ExtractionException("Could not get visitorData");
            return visitorData;
        }

        /// <summary>A browse request with the WEB client (channel pages and their continuations).</summary>
        public static Task<JObject> BrowseAsync(JObject extra, CancellationToken ct)
        {
            var body = PrepareBody(WebClient());
            foreach (var p in extra.Properties()) body[p.Name] = p.Value;
            return PostAsync(YoutubeiV1Url + "browse?" + DisablePrettyPrint, body, false, ct);
        }
    }
}
