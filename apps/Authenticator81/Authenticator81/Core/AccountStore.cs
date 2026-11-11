using System;
using System.Collections.Generic;
using System.Linq;
using Windows.Security.Credentials;
using Windows.Security.Cryptography;
using Windows.Security.Cryptography.Core;
using Windows.Storage;
using Windows.Storage.Streams;

namespace Authenticator.Core
{
    /// <summary>
    /// Keeps the accounts in the PasswordVault (one credential per account, password = otpauth:// URI);
    /// the order of the list is kept in the local settings.
    /// </summary>
    public sealed class AccountStore
    {
        const string Resource = "DumbPhone.Authenticator";
        const string OrderKey = "order";

        readonly PasswordVault vault = new PasswordVault();

        public List<OtpAccount> Load()
        {
            var byId = new Dictionary<string, OtpAccount>();
            IReadOnlyList<PasswordCredential> creds;
            try { creds = vault.FindAllByResource(Resource); }
            catch (Exception) { creds = new List<PasswordCredential>(); } // FindAllByResource throws when empty
            foreach (var c in creds)
            {
                try
                {
                    c.RetrievePassword();
                    var a = OtpUri.Parse(c.Password);
                    a.Id = c.UserName;
                    byId[a.Id] = a;
                }
                catch (Exception) { /* skip unreadable entries */ }
            }
            var order = (ApplicationData.Current.LocalSettings.Values[OrderKey] as string ?? "")
                .Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries);
            var result = order.Where(byId.ContainsKey).Select(id => byId[id]).ToList();
            result.AddRange(byId.Values.Where(a => !order.Contains(a.Id)));
            return result;
        }

        /// <summary>Adds or replaces the account (PasswordVault.Add throws on duplicates, so remove first).</summary>
        public void Save(OtpAccount a, IEnumerable<OtpAccount> all)
        {
            RemoveCredential(a.Id);
            vault.Add(new PasswordCredential(Resource, a.Id, OtpUri.Build(a)));
            SaveOrder(all);
        }

        public void Delete(OtpAccount a, IEnumerable<OtpAccount> all)
        {
            RemoveCredential(a.Id);
            SaveOrder(all);
        }

        void RemoveCredential(string id)
        {
            try { vault.Remove(vault.Retrieve(Resource, id)); } catch (Exception) { } // Retrieve throws when absent
        }

        static void SaveOrder(IEnumerable<OtpAccount> all) =>
            ApplicationData.Current.LocalSettings.Values[OrderKey] = string.Join(",", all.Select(x => x.Id));

        /// <summary>HMAC via the WinRT MacAlgorithmProvider (System.Security.Cryptography HMAC is not in the 8.1 profile).</summary>
        public static byte[] Hmac(OtpAlgorithm alg, byte[] key, byte[] msg)
        {
            string name = alg == OtpAlgorithm.Sha256 ? MacAlgorithmNames.HmacSha256
                        : alg == OtpAlgorithm.Sha512 ? MacAlgorithmNames.HmacSha512
                        : MacAlgorithmNames.HmacSha1;
            var provider = MacAlgorithmProvider.OpenAlgorithm(name);
            CryptographicKey k = provider.CreateKey(CryptographicBuffer.CreateFromByteArray(key));
            IBuffer mac = CryptographicEngine.Sign(k, CryptographicBuffer.CreateFromByteArray(msg));
            byte[] result;
            CryptographicBuffer.CopyToByteArray(mac, out result);
            return result;
        }
    }
}
