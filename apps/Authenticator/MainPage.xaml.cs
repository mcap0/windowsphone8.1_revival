using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using Authenticator.Core;
using Windows.ApplicationModel.DataTransfer;
using Windows.UI.Popups;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace Authenticator
{
    public sealed class AccountItem : INotifyPropertyChanged
    {
        public OtpAccount Account { get; }
        public AccountItem(OtpAccount a) { Account = a; }

        public string Title => string.IsNullOrEmpty(Account.Issuer) ? Account.Label
            : string.IsNullOrEmpty(Account.Label) ? Account.Issuer : Account.Issuer + " · " + Account.Label;

        string code = "";
        public string Code { get => code; private set { if (code != value) { code = value; Changed(nameof(Code)); } } }

        string hint = "";
        public string Hint { get => hint; private set { if (hint != value) { hint = value; Changed(nameof(Hint)); } } }

        public void Refresh(long now)
        {
            string c = Account.Type == OtpType.Totp
                ? Otp.Totp(AccountStore.Hmac, Account, now)
                : Otp.Hotp(AccountStore.Hmac, Account.Algorithm, Account.Secret, Account.Counter, Account.Digits);
            // "123 456" is easier to read and type than "123456"
            Code = c.Length == 6 ? c.Substring(0, 3) + " " + c.Substring(3) : c;
            Hint = Account.Type == OtpType.Totp
                ? Otp.SecondsLeft(now, Account.Period) + " s · tocca per copiare"
                : "contatore " + Account.Counter + " · tocca per il codice successivo";
        }

        public string RawCode => Code.Replace(" ", "");

        public event PropertyChangedEventHandler PropertyChanged;
        void Changed(string p) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(p));
    }

    public sealed partial class MainPage : Page
    {
        public static readonly AccountStore Store = new AccountStore();
        readonly ObservableCollection<AccountItem> items = new ObservableCollection<AccountItem>();
        readonly DispatcherTimer timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };

        public MainPage()
        {
            InitializeComponent();
            NavigationCacheMode = NavigationCacheMode.Required;
            List.ItemsSource = items;
            timer.Tick += (s, e) => Tick();
        }

        static long Now() => DateTimeOffset.UtcNow.ToUnixTimeSeconds();

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            items.Clear();
            foreach (var a in Store.Load()) items.Add(new AccountItem(a));
            EmptyText.Visibility = items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            Tick();
            timer.Start();
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e) => timer.Stop();

        void Tick()
        {
            long now = Now();
            Countdown.Value = Otp.SecondsLeft(now, 30);
            foreach (var i in items) i.Refresh(now);
        }

        void OnAdd(object sender, RoutedEventArgs e) => Frame.Navigate(typeof(AddPage));

        void OnItemClick(object sender, ItemClickEventArgs e)
        {
            var item = (AccountItem)e.ClickedItem;
            if (item.Account.Type == OtpType.Hotp)
            {
                item.Account.Counter++;
                Store.Save(item.Account, items.Select(i => i.Account));
                item.Refresh(Now());
            }
            var data = new DataPackage();
            data.SetText(item.RawCode);
            Clipboard.SetContent(data);
        }

        void OnHolding(object sender, HoldingRoutedEventArgs e)
        {
            if (e.HoldingState == Windows.UI.Input.HoldingState.Started) ShowMenu(e.OriginalSource as FrameworkElement);
        }

        void OnRightTapped(object sender, RightTappedRoutedEventArgs e) => ShowMenu(e.OriginalSource as FrameworkElement);

        void ShowMenu(FrameworkElement source)
        {
            if (!(source?.DataContext is AccountItem item)) return;
            var menu = new MenuFlyout();
            var up = new MenuFlyoutItem { Text = "sposta in alto" };
            up.Click += (s, a) => Move(item);
            var del = new MenuFlyoutItem { Text = "elimina" };
            del.Click += async (s, a) =>
            {
                var dialog = new MessageDialog("Eliminare " + item.Title + "? Se non hai un'altra copia del segreto perderai l'accesso al codice.", "Elimina account");
                dialog.Commands.Add(new UICommand("elimina"));
                dialog.Commands.Add(new UICommand("annulla"));
                dialog.CancelCommandIndex = 1;
                if ((await dialog.ShowAsync()).Label != "elimina") return;
                items.Remove(item);
                Store.Delete(item.Account, items.Select(i => i.Account));
                EmptyText.Visibility = items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            };
            menu.Items.Add(up);
            menu.Items.Add(del);
            menu.ShowAt(source);
        }

        void Move(AccountItem item)
        {
            int i = items.IndexOf(item);
            if (i <= 0) return;
            items.Move(i, i - 1);
            Store.Save(item.Account, items.Select(x => x.Account));
        }
    }
}
