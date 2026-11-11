using System;
using System.Linq;
using Newtonsoft.Json;
using Sincro.Logic;
using Windows.Security.Credentials;

namespace Sincro.Core
{
    /// <summary>Stores the Google OAuth material in the phone's PasswordVault (one credential).</summary>
    public static class Vault
    {
        public const string Resource = "Sincro.Google";
        private const string User = "cuenta";

        public static GoogleCredentials Load()
        {
            try
            {
                var vault = new PasswordVault();
                var found = vault.FindAllByResource(Resource).FirstOrDefault();
                if (found == null) return null;
                found = vault.Retrieve(Resource, found.UserName);
                var creds = JsonConvert.DeserializeObject<GoogleCredentials>(found.Password);
                return creds != null && creds.IsComplete() ? creds : null;
            }
            catch (Exception)
            {
                // FindAllByResource throws when nothing is stored.
                return null;
            }
        }

        public static void Save(GoogleCredentials creds)
        {
            Clear();
            var vault = new PasswordVault();
            vault.Add(new PasswordCredential(Resource, User, JsonConvert.SerializeObject(creds)));
        }

        public static void Clear()
        {
            try
            {
                var vault = new PasswordVault();
                foreach (PasswordCredential c in vault.FindAllByResource(Resource).ToList())
                {
                    vault.Remove(c);
                }
            }
            catch (Exception)
            {
                // Nothing stored.
            }
        }
    }
}
