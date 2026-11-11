using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using Sincro.Logic;
using Windows.Web.Http;
using Windows.Web.Http.Headers;

namespace Sincro.Core
{
    public sealed class GoogleApiException : Exception
    {
        public int Status { get; private set; }
        public string Body { get; private set; }

        public GoogleApiException(int status, string body)
            : base("Google ha risposto " + status + ": " + Shorten(body))
        {
            Status = status;
            Body = body;
        }

        private static string Shorten(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            return s.Length > 200 ? s.Substring(0, 200) : s;
        }
    }

    /// <summary>Access-token handling and JSON GETs against Google over HTTPS (Windows.Web.Http).</summary>
    public sealed class GoogleApi
    {
        public const string TokenUrl = "https://oauth2.googleapis.com/token";
        public const string PeopleMeUrl = "https://people.googleapis.com/v1/people/me?personFields=emailAddresses";

        private readonly GoogleCredentials _creds;
        private string _accessToken;
        private DateTimeOffset _expires = DateTimeOffset.MinValue;

        public GoogleApi(GoogleCredentials creds)
        {
            if (creds == null || !creds.IsComplete()) throw new InvalidOperationException("Accesso Google non impostato.");
            _creds = creds;
        }

        /// <summary>Exchanges the refresh token for a 1 h access token.</summary>
        public async Task RefreshAsync()
        {
            var form = new List<KeyValuePair<string, string>>
            {
                new KeyValuePair<string, string>("client_id", _creds.ClientId),
                new KeyValuePair<string, string>("client_secret", _creds.ClientSecret),
                new KeyValuePair<string, string>("refresh_token", _creds.RefreshToken),
                new KeyValuePair<string, string>("grant_type", "refresh_token"),
            };
            using (var client = Http.NoCacheClient())
            {
                HttpResponseMessage resp = await client.PostAsync(new Uri(TokenUrl), new HttpFormUrlEncodedContent(form));
                string body = await resp.Content.ReadAsStringAsync();
                if (!resp.IsSuccessStatusCode) throw new GoogleApiException((int)resp.StatusCode, body);
                JObject json = GoogleJsonReader.Parse(body);
                _accessToken = (string)json["access_token"];
                int seconds = (int?)json["expires_in"] ?? 3600;
                // Renew a few minutes early.
                _expires = DateTimeOffset.UtcNow.AddSeconds(seconds - 300);
                if (string.IsNullOrEmpty(_accessToken)) throw new GoogleApiException((int)resp.StatusCode, body);
            }
        }

        public async Task<JObject> GetJsonAsync(string url)
        {
            if (_accessToken == null || DateTimeOffset.UtcNow >= _expires) await RefreshAsync();
            RawResponse resp = await GetRawAsync(url);
            if (resp.Status == 401)
            {
                await RefreshAsync();
                resp = await GetRawAsync(url);
            }
            if (resp.Status < 200 || resp.Status > 299) throw new GoogleApiException(resp.Status, resp.Body);
            return GoogleJsonReader.Parse(resp.Body);
        }

        private sealed class RawResponse
        {
            public int Status;
            public string Body;
        }

        private async Task<RawResponse> GetRawAsync(string url)
        {
            using (var client = Http.NoCacheClient())
            {
                var req = new HttpRequestMessage(HttpMethod.Get, new Uri(url));
                req.Headers.Authorization = new HttpCredentialsHeaderValue("Bearer", _accessToken);
                HttpResponseMessage resp = await client.SendRequestAsync(req);
                return new RawResponse { Status = (int)resp.StatusCode, Body = await resp.Content.ReadAsStringAsync() };
            }
        }

        /// <summary>Email of the account that granted consent (People API people/me).</summary>
        public async Task<string> FetchEmailAsync()
        {
            JObject json = await GetJsonAsync(PeopleMeUrl);
            var emails = json["emailAddresses"] as JArray;
            if (emails == null || emails.Count == 0) return null;
            return (string)emails[0]["value"];
        }
    }
}
