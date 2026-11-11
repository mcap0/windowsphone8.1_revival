using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Authenticator.Core;
using Windows.UI.Popups;

namespace Authenticator
{
    public static class AccountImport
    {
        /// <summary>Returns -1 when the user cancels, otherwise the number of newly imported accounts.</summary>
        public static async Task<int> ImportAsync(OtpImportResult result)
        {
            if (result == null || result.Accounts == null)
                throw new ArgumentNullException("result");

            var existing = App.Store.Load();
            var candidates = new List<OtpAccount>();

            foreach (OtpAccount account in result.Accounts)
            {
                if (account == null)
                    continue;

                bool duplicate = existing.Any(item => SameAccount(item, account));
                if (!duplicate)
                {
                    candidates.Add(account);
                    existing.Add(account);
                }
            }

            if (candidates.Count == 0)
            {
                var alreadyThere = new MessageDialog("Tutti gli account del QR sono già presenti.", "Importazione");
                await alreadyThere.ShowAsync();
                return 0;
            }

            var content = new StringBuilder();
            int shown = 0;
            foreach (OtpAccount account in candidates)
            {
                if (shown == 8)
                {
                    content.AppendLine("…");
                    break;
                }

                if (shown > 0)
                    content.AppendLine();

                string title = string.IsNullOrEmpty(account.Issuer)
                    ? account.Label
                    : string.IsNullOrEmpty(account.Label)
                        ? account.Issuer
                        : account.Issuer + " · " + account.Label;

                content.Append(title);
                shown++;
            }

            var dialog = new MessageDialog(content.ToString(), "Importa " + candidates.Count + " account?");
            dialog.Commands.Add(new UICommand("importa"));
            dialog.Commands.Add(new UICommand("annulla"));
            dialog.CancelCommandIndex = 1;

            IUICommand choice = await dialog.ShowAsync();
            if (choice == null || choice.Label != "importa")
                return -1;

            var all = App.Store.Load();
            foreach (OtpAccount account in candidates)
            {
                all.Add(account);
                App.Store.Save(account, all);
            }

            return candidates.Count;
        }

        static bool SameAccount(OtpAccount left, OtpAccount right)
        {
            return string.Equals(left.Issuer ?? "", right.Issuer ?? "", StringComparison.OrdinalIgnoreCase)
                && string.Equals(left.Label ?? "", right.Label ?? "", StringComparison.OrdinalIgnoreCase)
                && left.Secret != null
                && right.Secret != null
                && left.Secret.SequenceEqual(right.Secret);
        }
    }
}
