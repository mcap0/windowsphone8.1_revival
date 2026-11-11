using System;
using Newtonsoft.Json;
using Posta.Core.Model;
using Windows.Security.Credentials;
using Windows.Storage;

namespace Posta.Services
{
    /// <summary>
    /// Account settings go to LocalSettings (no secrets). The app password goes only to
    /// PasswordVault, keyed by resource name and account e-mail.
    /// </summary>
    public static class AccountStore
    {
        private const string SettingsKey = "posta.account.v1";
        private const string VaultResource = "Posta.Mail";

        public static AccountSettings Load()
        {
            try
            {
                var raw = ApplicationData.Current.LocalSettings.Values[SettingsKey] as string;
                if (string.IsNullOrEmpty(raw)) return null;
                return JsonConvert.DeserializeObject<AccountSettings>(raw);
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static void Save(AccountSettings settings)
        {
            ApplicationData.Current.LocalSettings.Values[SettingsKey] = JsonConvert.SerializeObject(settings);
        }

        public static string LoadPassword(string email)
        {
            if (string.IsNullOrEmpty(email)) return null;
            try
            {
                var vault = new PasswordVault();
                var credential = vault.Retrieve(VaultResource, email);
                credential.RetrievePassword();
                return credential.Password;
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static void SavePassword(string email, string password)
        {
            var vault = new PasswordVault();
            try
            {
                var existing = vault.Retrieve(VaultResource, email);
                vault.Remove(existing);
            }
            catch (Exception)
            {
                // nothing stored yet
            }
            vault.Add(new PasswordCredential(VaultResource, email, password));
        }

        public static void ClearPassword(string email)
        {
            if (string.IsNullOrEmpty(email)) return;
            try
            {
                var vault = new PasswordVault();
                vault.Remove(vault.Retrieve(VaultResource, email));
            }
            catch (Exception)
            {
                // not stored
            }
        }
    }
}
