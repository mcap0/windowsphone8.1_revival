// 
// This is the source code of Telegram for Windows Phone v. 3.x.x.
// It is licensed under GNU GPL v. 2 or later.
// You should have received a copy of the license in this archive (see LICENSE).
// 
// Copyright Evgeny Nadymov, 2013-present.
// 
using System;
using Telegram.Api;
#if WP81
using Windows.Security.Credentials;
using Windows.Storage;
#endif

namespace TelegramClient.Helpers
{
    /// <summary>
    /// Stores the user's api_id (LocalSettings) and api_hash (PasswordVault) and pushes them into ApiConfig.
    /// </summary>
    public static class ApiCredentials
    {
        private const string ApiIdKey = "ApiCredentials.ApiId";
        private const string VaultResource = "TelegramNative.ApiCredentials";
        private const string VaultUserName = "api_hash";

        public static bool IsConfigured
        {
            get { return ApiConfig.IsConfigured; }
        }

        public static void Load()
        {
            var apiId = 0;
            string apiHash = null;
#if WP81
            try
            {
                object value;
                if (ApplicationData.Current.LocalSettings.Values.TryGetValue(ApiIdKey, out value) && value is int)
                {
                    apiId = (int)value;
                }
            }
            catch (Exception)
            {
                apiId = 0;
            }

            try
            {
                var credential = new PasswordVault().Retrieve(VaultResource, VaultUserName);
                credential.RetrievePassword();
                apiHash = credential.Password;
            }
            catch (Exception)
            {
                apiHash = null;
            }
#endif
            ApiConfig.ApiId = apiId;
            ApiConfig.ApiHash = apiHash;
        }

        public static void Save(int apiId, string apiHash)
        {
#if WP81
            ApplicationData.Current.LocalSettings.Values[ApiIdKey] = apiId;

            var vault = new PasswordVault();
            try
            {
                foreach (var existing in vault.FindAllByResource(VaultResource))
                {
                    vault.Remove(existing);
                }
            }
            catch (Exception)
            {
                // nothing stored yet
            }
            vault.Add(new PasswordCredential(VaultResource, VaultUserName, apiHash));
#endif
            ApiConfig.ApiId = apiId;
            ApiConfig.ApiHash = apiHash;
        }
    }
}
