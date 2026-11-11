using System;
using System.Threading;
using System.Threading.Tasks;
using Posta.Core.Logic;
using Posta.Core.Mail;
using Posta.Core.Model;
using Posta.Services;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Posta.Pages
{
    public sealed partial class AccountPage : Page
    {
        private AccountSettings _existing;
        private bool _loading;

        public AccountPage()
        {
            InitializeComponent();
            NavigationCacheMode = NavigationCacheMode.Disabled;
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            _existing = AccountStore.Load();
            var s = _existing ?? AccountSettings.GmailDefaults("");
            _loading = true;
            try
            {
                EmailBox.Text = s.Email;
                ImapHostBox.Text = s.ImapHost;
                ImapPortBox.Text = s.ImapPort.ToString();
                SmtpHostBox.Text = s.SmtpHost;
                SmtpPortBox.Text = s.SmtpPort.ToString();
                UsernameBox.Text = s.Username;
            }
            finally
            {
                _loading = false;
            }
            PasswordBox.Password = "";
        }

        private void EmailBox_TextChanged(object sender, TextChangedEventArgs e)
        {
            // Typing a Gmail address fills in the Gmail server defaults; other providers are left as they are.
            if (_loading) return;
            if (EmailBox.Text.Trim().EndsWith("@gmail.com", StringComparison.OrdinalIgnoreCase))
            {
                ImapHostBox.Text = "imap.gmail.com";
                ImapPortBox.Text = "993";
                SmtpHostBox.Text = "smtp.gmail.com";
                SmtpPortBox.Text = "465";
            }
        }

        private AccountSettings ReadForm()
        {
            int imapPort, smtpPort;
            if (!int.TryParse(ImapPortBox.Text.Trim(), out imapPort)) imapPort = -1;
            if (!int.TryParse(SmtpPortBox.Text.Trim(), out smtpPort)) smtpPort = -1;
            return new AccountSettings
            {
                Email = EmailBox.Text.Trim(),
                ImapHost = ImapHostBox.Text.Trim(),
                ImapPort = imapPort,
                SmtpHost = SmtpHostBox.Text.Trim(),
                SmtpPort = smtpPort,
                Username = UsernameBox.Text.Trim(),
            };
        }

        /// <summary>The typed password, or the stored one when the field was left empty for the same account.</summary>
        private string ResolvePassword(AccountSettings s)
        {
            if (!string.IsNullOrEmpty(PasswordBox.Password)) return PasswordBox.Password;
            if (_existing != null && string.Equals(_existing.Email, s.Email, StringComparison.OrdinalIgnoreCase))
                return AccountStore.LoadPassword(s.Email);
            return null;
        }

        private async void TestButton_Click(object sender, Windows.UI.Xaml.RoutedEventArgs e)
        {
            var s = ReadForm();
            var pwd = ResolvePassword(s);
            var errors = AccountValidator.Validate(s, pwd, true);
            if (errors.Length > 0)
            {
                StatusText.Text = string.Join(" ", errors);
                return;
            }
            SetBusy(true);
            StatusText.Text = "Verifica in corso...";
            try
            {
                using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(60)))
                {
                    var summary = await new MailService(s, pwd).TestConnectionAsync(cts.Token);
                    StatusText.Text = summary;
                }
            }
            catch (Exception ex)
            {
                StatusText.Text = MailService.Describe(ex);
            }
            finally
            {
                SetBusy(false);
            }
        }

        private async void SaveButton_Click(object sender, RoutedEventArgs e)
        {
            var s = ReadForm();
            var typed = PasswordBox.Password;
            var pwd = ResolvePassword(s);
            var errors = AccountValidator.Validate(s, pwd, true);
            if (errors.Length > 0)
            {
                StatusText.Text = string.Join(" ", errors);
                return;
            }

            bool accountChanged = _existing == null ||
                !string.Equals(_existing.Email, s.Email, StringComparison.OrdinalIgnoreCase);
            if (accountChanged && _existing != null)
            {
                AccountStore.ClearPassword(_existing.Email);
                await CacheStore.ClearAsync();
            }

            AccountStore.Save(s);
            if (!string.IsNullOrEmpty(typed)) AccountStore.SavePassword(s.Email, typed);
            _existing = s;

            // Back to the inbox with a fresh stack, whatever screen we came from.
            Frame.Navigate(typeof(InboxPage));
            Frame.BackStack.Clear();
        }

        private void SetBusy(bool busy)
        {
            Busy.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
            Busy.IsIndeterminate = busy;
            TestButton.IsEnabled = !busy;
            SaveButton.IsEnabled = !busy;
        }
    }
}
