using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using Posta.Core.Cache;
using Posta.Core.Mail;
using Posta.Core.Model;
using Posta.Models;
using Posta.Services;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Posta.Pages
{
    public sealed partial class InboxPage : Page
    {
        private static readonly string[] FolderKeys =
        {
            MailFolderKeys.Inbox,
            MailFolderKeys.Sent,
            MailFolderKeys.Drafts,
            MailFolderKeys.Trash,
            MailFolderKeys.Spam,
            MailFolderKeys.All
        };

        private static readonly string[] FolderNames =
        {
            "Posta in arrivo",
            "Inviati",
            "Bozze",
            "Cestino",
            "Spam",
            "Tutti"
        };

        private readonly ObservableCollection<MailListItem> _items = new ObservableCollection<MailListItem>();
        private readonly List<MailSummary> _summaries = new List<MailSummary>();
        private AccountSettings _account;
        private string _password;
        private string _folderKey = MailFolderKeys.Inbox;
        private int _total;
        private bool _busy;
        private CancellationTokenSource _networkCancellation;

        public InboxPage()
        {
            InitializeComponent();
            InboxList.ItemsSource = _items;
            SentList.ItemsSource = _items;
            DraftsList.ItemsSource = _items;
            TrashList.ItemsSource = _items;
            SpamList.ItemsSource = _items;
            AllList.ItemsSource = _items;
            FolderPivot.SelectedIndex = 0;
            NavigationCacheMode = NavigationCacheMode.Disabled;
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            _account = AccountStore.Load();
            _password = _account == null ? null : AccountStore.LoadPassword(_account.Email);
            if (_account == null || string.IsNullOrEmpty(_password))
            {
                Frame.Navigate(typeof(AccountPage));
                return;
            }

            var selectedIndex = FolderPivot.SelectedIndex;
            if (selectedIndex < 0 || selectedIndex >= FolderKeys.Length)
                selectedIndex = 0;

            _folderKey = FolderKeys[selectedIndex];

            if (_folderKey == MailFolderKeys.Inbox)
            {
                var cached = await CacheStore.LoadAsync();
                if (cached != null &&
                    string.Equals(cached.Account, _account.Email, StringComparison.OrdinalIgnoreCase))
                {
                    _total = cached.Total;
                    SetSummaries(cached.Messages);
                    StatusText.Text = "Dalla cache. Aggiornamento in corso...";
                }
            }

            await RefreshAsync();
        }

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            if (_networkCancellation != null)
                _networkCancellation.Cancel();

            base.OnNavigatedFrom(e);
        }

        private async Task RefreshAsync()
        {
            if (_account == null)
                return;

            await RunNetworkAsync(async ct =>
            {
                var page = await new MailService(_account, _password)
                    .FetchFolderPageAsync(_folderKey, 0, ct);

                _total = page.Total;
                SetSummaries(page.Messages);
                StatusText.Text = page.Messages.Count == 0
                    ? "La cartella è vuota."
                    : "Aggiornato: " + _total + " messaggi.";
                await SaveCacheAsync();
            });
        }

        private async Task LoadMoreAsync()
        {
            if (_account == null || _summaries.Count >= _total)
                return;

            var pageIndex = _summaries.Count / MailService.PageSize;
            await RunNetworkAsync(async ct =>
            {
                var result = await new MailService(_account, _password)
                    .FetchFolderPageAsync(_folderKey, pageIndex, ct);

                _total = result.Total;
                foreach (var message in result.Messages)
                    _summaries.Add(message);

                Rebuild();
                StatusText.Text = "Mostrati " + _summaries.Count + " di " + _total + " messaggi.";
                await SaveCacheAsync();
            });
        }

        private async Task RunNetworkAsync(Func<CancellationToken, Task> operation)
        {
            if (_busy)
                return;

            _busy = true;
            SetBusy(true);
            using (var cts = new CancellationTokenSource(TimeSpan.FromSeconds(90)))
            {
                _networkCancellation = cts;
                try
                {
                    await operation(cts.Token);
                }
                catch (OperationCanceledException)
                {
                    StatusText.Text = "Operazione annullata.";
                }
                catch (Exception ex)
                {
                    StatusText.Text = MailService.Describe(ex);
                }
                finally
                {
                    if (_networkCancellation == cts)
                        _networkCancellation = null;
                    _busy = false;
                    SetBusy(false);
                }
            }
        }

        private void SetSummaries(List<MailSummary> messages)
        {
            _summaries.Clear();
            if (messages != null)
                _summaries.AddRange(messages);
            Rebuild();
        }

        private void Rebuild()
        {
            _items.Clear();
            foreach (var message in _summaries)
                _items.Add(MailListItem.Create(message));

            LoadMoreButton.Visibility = _summaries.Count < _total
                ? Visibility.Visible
                : Visibility.Collapsed;
        }

        private Task SaveCacheAsync()
        {
            if (_folderKey != MailFolderKeys.Inbox || _account == null)
                return Task.FromResult(0);

            return CacheStore.SaveAsync(new InboxCacheDocument
            {
                Account = _account.Email,
                Total = _total,
                Messages = new List<MailSummary>(_summaries)
            });
        }

        private void SetBusy(bool busy)
        {
            Busy.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
            Busy.IsIndeterminate = busy;
            RefreshButton.IsEnabled = !busy;
            LoadMoreButton.IsEnabled = !busy;
        }

        private async void FolderPivot_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            var index = FolderPivot.SelectedIndex;
            if (_account == null || index < 0 || index >= FolderKeys.Length)
                return;

            _folderKey = FolderKeys[index];
            _total = 0;
            _summaries.Clear();
            Rebuild();
            StatusText.Text = "Caricamento...";
            await RefreshAsync();
        }

        private async void RefreshButton_Click(object sender, RoutedEventArgs e)
        {
            await RefreshAsync();
        }

        private async void LoadMoreButton_Click(object sender, RoutedEventArgs e)
        {
            await LoadMoreAsync();
        }

        private void MailList_ItemClick(object sender, ItemClickEventArgs e)
        {
            var item = e.ClickedItem as MailListItem;
            if (item == null)
                return;

            var key = _folderKey + "|" + item.Uid.ToString();
            Frame.Navigate(typeof(MessagePage), key);
        }

        private void ComposeButton_Click(object sender, RoutedEventArgs e)
        {
            ComposeRequest.StartNew();
            Frame.Navigate(typeof(ComposePage));
        }

        private void AccountButton_Click(object sender, RoutedEventArgs e)
        {
            Frame.Navigate(typeof(AccountPage));
        }
    }
}
