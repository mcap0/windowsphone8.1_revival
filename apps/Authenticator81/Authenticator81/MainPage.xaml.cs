using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using Authenticator.Core;
using Windows.Storage;
using Windows.UI.Popups;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace Authenticator
{
    public sealed class AccountItem : INotifyPropertyChanged
    {
        public OtpAccount Account { get; private set; }

        public AccountItem(OtpAccount a)
        {
            Account = a;
        }

        public string Title
        {
            get
            {
                return string.IsNullOrEmpty(Account.Issuer) ? Account.Label
                    : string.IsNullOrEmpty(Account.Label) ? Account.Issuer
                    : Account.Issuer + " · " + Account.Label;
            }
        }

        string code = "";
        public string Code
        {
            get { return code; }
            private set
            {
                if (code != value)
                {
                    code = value;
                    Changed("Code");
                }
            }
        }

        string hint = "";
        public string Hint
        {
            get { return hint; }
            private set
            {
                if (hint != value)
                {
                    hint = value;
                    Changed("Hint");
                }
            }
        }

        public void Refresh(long now)
        {
            string c = Account.Type == OtpType.Totp
                ? Otp.Totp(AccountStore.Hmac, Account, now)
                : Otp.Hotp(AccountStore.Hmac, Account.Algorithm, Account.Secret, Account.Counter, Account.Digits);
            Code = Format(c);
            Hint = Account.Type == OtpType.Totp
                ? Otp.SecondsLeft(now, Account.Period) + " s · tocca per il codice"
                : "contatore " + Account.Counter + " · tocca per il codice successivo";
        }

        public static string Format(string c)
        {
            return c.Length == 6 ? c.Substring(0, 3) + " " + c.Substring(3) : c;
        }

        public event PropertyChangedEventHandler PropertyChanged;

        void Changed(string propertyName)
        {
            var handler = PropertyChanged;
            if (handler != null)
                handler(this, new PropertyChangedEventArgs(propertyName));
        }
    }

    public sealed partial class MainPage : Page
    {
        readonly ObservableCollection<AccountItem> items = new ObservableCollection<AccountItem>();
        readonly DispatcherTimer timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };

        public MainPage()
        {
            InitializeComponent();
            NavigationCacheMode = NavigationCacheMode.Required;
            List.ItemsSource = items;
            timer.Tick += (s, e) => Tick();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            ShowCrash();
            Reload();
            timer.Start();
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            timer.Stop();
        }

        void ShowCrash()
        {
            string crash = null;
            try { crash = ApplicationData.Current.LocalSettings.Values[App.CrashKey] as string; }
            catch (Exception) { }
            CrashPanel.Visibility = string.IsNullOrEmpty(crash) ? Visibility.Collapsed : Visibility.Visible;
            CrashText.Text = crash ?? "";
        }

        void OnClearCrash(object sender, RoutedEventArgs e)
        {
            try { ApplicationData.Current.LocalSettings.Values.Remove(App.CrashKey); }
            catch (Exception) { }
            ShowCrash();
        }

        void Reload()
        {
            items.Clear();
            foreach (var account in App.Store.Load())
                items.Add(new AccountItem(account));

            EmptyText.Visibility = items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            Tick();
        }

        void Tick()
        {
            long now = App.UnixNow();
            Countdown.Value = Otp.SecondsLeft(now, 30);
            foreach (var item in items)
                item.Refresh(now);
        }

        void OnAdd(object sender, RoutedEventArgs e)
        {
            Frame.Navigate(typeof(AddPage));
        }

        void OnScan(object sender, RoutedEventArgs e)
        {
            Frame.Navigate(typeof(ScanPage));
        }

        void OnBackup(object sender, RoutedEventArgs e)
        {
            Frame.Navigate(typeof(BackupPage));
        }

        void OnItemClick(object sender, ItemClickEventArgs e)
        {
            var item = (AccountItem)e.ClickedItem;
            Frame.Navigate(typeof(CodePage), item.Account.Id);
        }

        void OnHolding(object sender, HoldingRoutedEventArgs e)
        {
            if (e.HoldingState == Windows.UI.Input.HoldingState.Started)
                ShowMenu(e.OriginalSource as FrameworkElement);
        }

        void ShowMenu(FrameworkElement source)
        {
            var item = source == null ? null : source.DataContext as AccountItem;
            if (item == null)
                return;

            var menu = new MenuFlyout();
            var up = new MenuFlyoutItem { Text = "sposta in alto" };
            up.Click += (s, e) => Move(item);

            var rename = new MenuFlyoutItem { Text = "rinomina" };
            rename.Click += (s, e) => Frame.Navigate(typeof(EditPage), item.Account.Id);

            var delete = new MenuFlyoutItem { Text = "elimina" };
            delete.Click += async (s, e) =>
            {
                var dialog = new MessageDialog("Eliminare " + item.Title + "? Se non hai un'altra copia del segreto perderai l'accesso al codice.", "Elimina account");
                dialog.Commands.Add(new UICommand("elimina"));
                dialog.Commands.Add(new UICommand("annulla"));
                dialog.CancelCommandIndex = 1;
                if ((await dialog.ShowAsync()).Label != "elimina")
                    return;

                items.Remove(item);
                App.Store.Delete(item.Account, items.Select(i => i.Account));
                EmptyText.Visibility = items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            };

            menu.Items.Add(up);
            menu.Items.Add(rename);
            menu.Items.Add(delete);
            menu.ShowAt(source);
        }

        void Move(AccountItem item)
        {
            int index = items.IndexOf(item);
            if (index <= 0)
                return;

            items.Move(index, index - 1);
            App.Store.Save(item.Account, items.Select(x => x.Account));
        }
    }
}
