using System;
using System.Threading.Tasks;
using Newtonsoft.Json;
using Sincro.Logic;
using Windows.Web.Http;

namespace Sincro.Core
{
    /// <summary>"Importa dal PC": downloads the JSON written by tools/google-auth, validates it and stores it.</summary>
    public static class SetupService
    {
        public const string DefaultAddress = "http://192.168.1.148:8765/google-auth.json";

        public static async Task<string> ImportAsync(string address)
        {
            Uri uri;
            if (!Uri.TryCreate(address ?? "", UriKind.Absolute, out uri)) throw new ArgumentException("Indirizzo non valido.");

            string text;
            using (var client = Http.NoCacheClient())
            {
                HttpResponseMessage resp = await client.GetAsync(uri);
                if (!resp.IsSuccessStatusCode) throw new InvalidOperationException("Il PC non risponde (" + (int)resp.StatusCode + ").");
                text = await resp.Content.ReadAsStringAsync();
            }

            GoogleCredentials creds = JsonConvert.DeserializeObject<GoogleCredentials>(text);
            if (creds == null || !creds.IsComplete()) throw new InvalidOperationException("File di accesso incompleto.");

            // Validate: the refresh token must give an access token, and we read the account email.
            var api = new GoogleApi(creds);
            await api.RefreshAsync();
            creds.Email = await api.FetchEmailAsync();

            Vault.Save(creds);
            return creds.Email ?? "(account senza email)";
        }
    }
}
