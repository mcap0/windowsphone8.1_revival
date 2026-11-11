using System;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using Windows.Security.Credentials;
using Windows.Storage;

namespace Authenticator.Core
{
    /// <summary>
    /// Keeps the accounts in the Windows credential locker (PasswordVault), one entry per account
    /// holding its otpauth:// URI; the order of the list is kept in the local settings.
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

        public void Save(OtpAccount a, IEnumerable<OtpAccount> all)
        {
            vault.Add(new PasswordCredential(Resource, a.Id, OtpUri.Build(a)));
            SaveOrder(all);
        }

        public void Delete(OtpAccount a, IEnumerable<OtpAccount> all)
        {
            try { vault.Remove(vault.Retrieve(Resource, a.Id)); } catch (Exception) { }
            SaveOrder(all);
        }

        static void SaveOrder(IEnumerable<OtpAccount> all) =>
            ApplicationData.Current.LocalSettings.Values[OrderKey] = string.Join(",", all.Select(x => x.Id));

        public static byte[] Hmac(OtpAlgorithm alg, byte[] key, byte[] msg)
        {
            using (HMAC h = alg == OtpAlgorithm.Sha256 ? new HMACSHA256(key)
                          : alg == OtpAlgorithm.Sha512 ? (HMAC)new HMACSHA512(key)
                          : new HMACSHA1(key))
                return h.ComputeHash(msg);
        }
    }
}
