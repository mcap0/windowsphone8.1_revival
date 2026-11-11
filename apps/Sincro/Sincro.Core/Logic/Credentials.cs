using Newtonsoft.Json;

namespace Sincro.Logic
{
    /// <summary>OAuth material. Same JSON shape as the PC helper writes (tools/google-auth).</summary>
    public sealed class GoogleCredentials
    {
        [JsonProperty("client_id")]
        public string ClientId { get; set; }

        [JsonProperty("client_secret")]
        public string ClientSecret { get; set; }

        [JsonProperty("refresh_token")]
        public string RefreshToken { get; set; }

        [JsonProperty("email")]
        public string Email { get; set; }

        public bool IsComplete()
        {
            return !string.IsNullOrWhiteSpace(ClientId)
                && !string.IsNullOrWhiteSpace(ClientSecret)
                && !string.IsNullOrWhiteSpace(RefreshToken);
        }
    }
}
