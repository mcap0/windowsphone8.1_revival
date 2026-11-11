using System;
using System.Threading.Tasks;
using Mappe.Core;
using Windows.Web.Http;

namespace Mappe
{
    /// <summary>Plain text GET with the app User-Agent (used for Photon search and reverse geocoding).</summary>
    public static class Net
    {
        public static async Task<string> GetStringAsync(string url)
        {
            var client = new HttpClient();
            try
            {
                var request = new HttpRequestMessage(HttpMethod.Get, new Uri(url));
                request.Headers.TryAppendWithoutValidation("User-Agent", GeoUrls.UserAgent);
                request.Headers.TryAppendWithoutValidation("Accept", "application/json");
                HttpResponseMessage response = await client.SendRequestAsync(request);
                string body = await response.Content.ReadAsStringAsync();
                if (!response.IsSuccessStatusCode)
                    throw new InvalidOperationException("HTTP " + (int)response.StatusCode + " " + response.ReasonPhrase);
                return body;
            }
            finally
            {
                client.Dispose();
            }
        }
    }
}
