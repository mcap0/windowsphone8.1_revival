using System;
using System.Linq;
using Authenticator.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Authenticator
{
    public sealed partial class CodePage : Page
    {
        OtpAccount account;
        readonly DispatcherTimer timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };

        public CodePage()
        {
            InitializeComponent();
            timer.Tick += (s, e) => Show();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            string id = e.Parameter as string;
            var all = App.Store.Load();
            account = all.FirstOrDefault(x => x.Id == id);
            if (account == null)
            {
                TitleText.Text = "Account non trovato";
                CodeBox.Visibility = Visibility.Collapsed;
                return;
            }
            if (account.Type == OtpType.Hotp)
            {
                account.Counter++; // each tap on an HOTP account yields the next code
                App.Store.Save(account, all);
            }
            TitleText.Text = Title(account);
            Show();
            timer.Start();
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e) => timer.Stop();

        static string Title(OtpAccount a) =>
            string.IsNullOrEmpty(a.Issuer) ? a.Label : string.IsNullOrEmpty(a.Label) ? a.Issuer : a.Issuer + " · " + a.Label;

        void OnLoaded(object sender, RoutedEventArgs e)
        {
            CodeBox.Focus(FocusState.Keyboard);
            CodeBox.SelectAll();
        }

        // Runs every second; the text is only replaced when the code changes, so the selection survives.
        void Show()
        {
            if (account == null) return;
            long now = App.UnixNow();
            string c = account.Type == OtpType.Totp
                ? Otp.Totp(AccountStore.Hmac, account, now)
                : Otp.Hotp(AccountStore.Hmac, account.Algorithm, account.Secret, account.Counter, account.Digits);
            string shown = AccountItem.Format(c);
            if (CodeBox.Text != shown)
            {
                CodeBox.Text = shown;
                CodeBox.SelectAll();
            }
            HintText.Text = account.Type == OtpType.Totp
                ? "valido ancora " + Otp.SecondsLeft(now, account.Period) + " s"
                : "contatore " + account.Counter;
        }
    }
}
