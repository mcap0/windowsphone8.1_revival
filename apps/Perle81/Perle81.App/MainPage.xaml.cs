/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of PerleFragment and PerleViewModel (Perle list, folders, shuffle, hide/undo, refresh, import).
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Perle;
using Perle.Extractor;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace Perle81.App
{
    public sealed partial class MainPage : Page
    {
        const string GroupKey = "perle_group";

        readonly ObservableCollection<VideoItem> items = new ObservableCollection<VideoItem>();
        readonly ObservableCollection<VideoItem> latestItems = new ObservableCollection<VideoItem>();
        int latestGeneration;
        PerleCache cache;
        long groupId = SubscriptionsData.GroupAllId;
        int loadGeneration;
        CancellationTokenSource loadCts;
        bool loaded;
        bool settingGroups;
        int shownSubscriptionsVersion;

        // last hidden video, for "Annulla"
        VideoItem lastHidden;
        int lastHiddenPosition;
        DispatcherTimer snackbarTimer;

        public MainPage()
        {
            InitializeComponent();
            NavigationCacheMode = NavigationCacheMode.Required;
            List.ItemsSource = items;
            Latest.ItemsSource = latestItems;
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            // coming back from search, player or history: keep the list as it is
            if (loaded && shownSubscriptionsVersion == AppState.SubscriptionsVersion) return;
            bool imported = shownSubscriptionsVersion != AppState.SubscriptionsVersion; // a new import: download again
            loaded = true;
            shownSubscriptionsVersion = AppState.SubscriptionsVersion;
            object saved = ApplicationData.Current.LocalSettings.Values[GroupKey];
            if (saved is long) groupId = (long)saved;
            ShowCrashOnce();
            await FillGroupsAsync();
            await LoadAsync(imported);
            await LoadLatestAsync(imported);
        }

        void ShowCrashOnce()
        {
            try
            {
                object crash = ApplicationData.Current.LocalSettings.Values[App.CrashKey];
                if (crash == null) return;
                ApplicationData.Current.LocalSettings.Values.Remove(App.CrashKey);
                InfoText.Text = "Ultimo errore: " + crash;
            }
            catch (Exception) { }
        }

        async Task FillGroupsAsync()
        {
            var data = await AppState.LoadSubscriptionsAsync();
            var choices = new List<GroupChoice> { new GroupChoice { Id = SubscriptionsData.GroupAllId, Name = "Tutte le iscrizioni" } };
            choices.AddRange(data.FeedGroups.Select(g => new GroupChoice { Id = g.Id, Name = g.Name }));
            settingGroups = true;
            GroupBox.ItemsSource = choices;
            var selected = choices.FirstOrDefault(c => c.Id == groupId) ?? choices[0];
            groupId = selected.Id;
            GroupBox.SelectedItem = selected;
            settingGroups = false;
        }

        /// <summary>Shows the cached videos if they are less than 7 days old, otherwise downloads them again.</summary>
        async Task LoadAsync(bool forceRefresh)
        {
            loadCts?.Cancel();
            var cts = loadCts = new CancellationTokenSource();
            int generation = ++loadGeneration;
            long loadedGroupId = groupId;
            ShowLoading(0, 0);
            try
            {
                var cached = await AppState.Repository.ReadCacheAsync(loadedGroupId);
                PerleCache result;
                if (!forceRefresh && cached != null && AppState.Repository.IsFresh(cached)) result = cached;
                else
                {
                    var subs = (await AppState.LoadSubscriptionsAsync()).SubscriptionsFor(loadedGroupId);
                    result = await AppState.Repository.FetchAsync(subs, loadedGroupId,
                        (done, total) => PostProgress(generation, done, total), cts.Token,
                        partial => PostPartial(generation, partial));
                }
                if (generation != loadGeneration) return;
                cache = result;
                await ShowAsync(result);
            }
            catch (OperationCanceledException) { }
            catch (Exception ex)
            {
                if (generation != loadGeneration) return;
                Progress.Visibility = Visibility.Collapsed;
                ShuffleButton.IsEnabled = true;
                InfoText.Text = "Errore: " + ex.Message;
            }
        }

        void PostProgress(int generation, int done, int total)
        {
            var ignored = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                if (generation == loadGeneration && Progress.Visibility == Visibility.Visible) ShowLoading(done, total);
            });
        }

        void PostPartial(int generation, PerleCache partial)
        {
            var ignored = Dispatcher.RunAsync(CoreDispatcherPriority.Low, async () =>
            {
                if (generation != loadGeneration) return;
                var list = await AppState.Repository.BuildListAsync(partial);
                if (generation != loadGeneration || list.Count == 0) return;
                items.Clear();
                foreach (var v in list) items.Add(new VideoItem(v));
                EmptyText.Visibility = Visibility.Collapsed;
            });
        }

        void ShowLoading(int done, int total)
        {
            Progress.Visibility = Visibility.Visible;
            Progress.IsIndeterminate = total == 0;
            Progress.Maximum = Math.Max(1, total);
            Progress.Value = done;
            InfoText.Text = total == 0 ? "Caricamento canali…" : string.Format("Canali caricati: {0}/{1}", done, total);
            ShuffleButton.IsEnabled = false;
        }

        async Task ShowAsync(PerleCache c)
        {
            var list = await AppState.Repository.BuildListAsync(c);
            items.Clear();
            foreach (var v in list) items.Add(new VideoItem(v));
            Progress.Visibility = Visibility.Collapsed;
            ShuffleButton.IsEnabled = true;
            string when = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddMilliseconds(c.Timestamp).ToLocalTime().ToString("d MMM, HH:mm");
            InfoText.Text = c.FailedChannels > 0
                ? string.Format("Aggiornato {0} · canali saltati per errori: {1}", when, c.FailedChannels)
                : string.Format("Aggiornato {0}", when);
            EmptyText.Visibility = items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            if (items.Count > 0) List.ScrollIntoView(items[0]);
        }

        async void OnGroupChanged(object sender, SelectionChangedEventArgs e)
        {
            var choice = GroupBox.SelectedItem as GroupChoice;
            if (settingGroups || choice == null || choice.Id == groupId) return;
            groupId = choice.Id;
            ApplicationData.Current.LocalSettings.Values[GroupKey] = groupId;
            cache = null;
            await LoadAsync(false);
            await LoadLatestAsync(false);
        }

        /// <summary>Mixes the already downloaded videos again, without downloading anything.</summary>
        async void OnShuffle(object sender, RoutedEventArgs e)
        {
            if (cache != null && Progress.Visibility != Visibility.Visible) await ShowAsync(cache);
        }

        async void OnRefresh(object sender, RoutedEventArgs e)
        {
            await LoadAsync(true);
            await LoadLatestAsync(true);
        }

        /// <summary>The "Ultimi" tab: from the channel feeds, cached for an hour unless refreshed.</summary>
        async Task LoadLatestAsync(bool force)
        {
            int generation = ++latestGeneration;
            long gid = groupId;
            var cached = await LatestStore.ReadAsync(gid);
            if (!force && LatestStore.IsFresh(cached))
            {
                ShowLatest(cached);
                return;
            }
            LatestInfo.Text = "Ultimi video: aggiornamento…";
            try
            {
                var fresh = await LatestStore.RefreshAsync(gid, (done, total) =>
                {
                    var ignored = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
                    {
                        if (generation == latestGeneration) LatestInfo.Text = string.Format("Ultimi video: canali {0}/{1}", done, total);
                    });
                });
                if (generation == latestGeneration && gid == groupId) ShowLatest(fresh);
            }
            catch (Exception ex)
            {
                if (generation == latestGeneration) LatestInfo.Text = "Ultimi video, errore: " + ex.Message;
            }
        }

        void ShowLatest(LatestCache c)
        {
            latestItems.Clear();
            foreach (var e in c.Items) latestItems.Add(new VideoItem(LatestStore.ToVideo(e)));
            LatestEmpty.Visibility = latestItems.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            string when = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddMilliseconds(c.Timestamp).ToLocalTime().ToString("d MMM, HH:mm");
            LatestInfo.Text = c.FailedChannels > 0
                ? string.Format("Aggiornato {0} · canali saltati: {1} (solo ID canale)", when, c.FailedChannels)
                : string.Format("Aggiornato {0}", when);
        }

        async void OnShowHidden(object sender, RoutedEventArgs e)
        {
            await AppState.Repository.ClearHiddenAsync();
            if (cache != null && Progress.Visibility != Visibility.Visible) await ShowAsync(cache);
        }

        void OnSearch(object sender, RoutedEventArgs e) => Frame.Navigate(typeof(SearchPage));

        void OnHistory(object sender, RoutedEventArgs e) => Frame.Navigate(typeof(HistoryPage));

        void OnDownloads(object sender, RoutedEventArgs e) => Frame.Navigate(typeof(DownloadPage));

        void OnImport(object sender, RoutedEventArgs e)
        {
            // the app may be terminated while the picker is open: ContinuationManager brings it back to ImportPage
            var picker = new FileOpenPicker();
            picker.FileTypeFilter.Add(".json");
            picker.PickSingleFileAndContinue();
        }

        void OnVideoClick(object sender, ItemClickEventArgs e)
        {
            var item = e.ClickedItem as VideoItem;
            if (item != null) Frame.Navigate(typeof(PlayerPage), item.Video.Url);
        }

        void OnHolding(object sender, HoldingRoutedEventArgs e)
        {
            if (e.HoldingState == Windows.UI.Input.HoldingState.Started) ShowMenu(e.OriginalSource as FrameworkElement);
        }

        void OnRightTapped(object sender, RightTappedRoutedEventArgs e) => ShowMenu(e.OriginalSource as FrameworkElement);

        void ShowMenu(FrameworkElement source)
        {
            var item = source == null ? null : source.DataContext as VideoItem;
            if (item == null) return;
            var menu = new MenuFlyout();
            var download = new MenuFlyoutItem { Text = "Scarica audio" };
            download.Click += (s, a) => { DownloadService.Enqueue(item.Video.Url, item.Title, item.Uploader); InfoText.Text = "Aggiunto alla coda: " + item.Title; };
            menu.Items.Add(download);
            var hide = new MenuFlyoutItem { Text = "Non mostrare" };
            hide.Click += async (s, a) => await HideAsync(item);
            var channel = new MenuFlyoutItem { Text = "Apri nel browser" };
            channel.Click += async (s, a) => await Windows.System.Launcher.LaunchUriAsync(new Uri(item.Video.Url));
            menu.Items.Add(hide);
            menu.Items.Add(channel);
            menu.ShowAt(source);
        }

        /// <summary>Hides the video now and every time Perle is opened again.</summary>
        async Task HideAsync(VideoItem item)
        {
            int position = items.IndexOf(item);
            if (position < 0) return;
            items.RemoveAt(position);
            lastHidden = item;
            lastHiddenPosition = position;
            Snackbar.Visibility = Visibility.Visible;
            snackbarTimer?.Stop();
            snackbarTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(5) };
            snackbarTimer.Tick += (s, a) => { snackbarTimer.Stop(); Snackbar.Visibility = Visibility.Collapsed; };
            snackbarTimer.Start();
            await AppState.Repository.SetHiddenAsync(item.Video.Url, true);
        }

        /// <summary>Undoes the last hide, putting the video back where it was.</summary>
        async void OnUndo(object sender, RoutedEventArgs e)
        {
            Snackbar.Visibility = Visibility.Collapsed;
            if (lastHidden == null) return;
            var item = lastHidden;
            lastHidden = null;
            if (!items.Contains(item)) items.Insert(Math.Min(Math.Max(0, lastHiddenPosition), items.Count), item);
            await AppState.Repository.SetHiddenAsync(item.Video.Url, false);
        }
    }
}
