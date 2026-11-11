using System;
using System.Linq;
using Authenticator.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Authenticator
{
    public sealed partial class EditPage : Page
    {
        OtpAccount account;

        public EditPage() { InitializeComponent(); }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            string id = e.Parameter as string;
            account = App.Store.Load().FirstOrDefault(x => x.Id == id);
            if (account == null) { ErrorText.Text = "Account non trovato."; return; }
            IssuerBox.Text = account.Issuer;
            LabelBox.Text = account.Label;
        }

        void OnSave(object sender, RoutedEventArgs e)
        {
            ErrorText.Text = "";
            if (account == null) return;
            try
            {
                account.Issuer = IssuerBox.Text.Trim();
                account.Label = LabelBox.Text.Trim();
                App.Store.Save(account, App.Store.Load());
                Frame.GoBack();
            }
            catch (Exception ex) { ErrorText.Text = ex.Message; }
        }
    }
}
