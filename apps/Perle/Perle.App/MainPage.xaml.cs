/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Port of PerleFragment and PerleViewModel.
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Windows.Storage;
using Windows.Storage.Pickers;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media.Imaging;
using Windows.UI.Xaml.Navigation;

namespace Perle.App
{
    public sealed class VideoItem
    {
        public PerleVideo Video { get; }
        public VideoItem(PerleVideo v) { Video = v; }

        public string Title => Video.Title;
        public string Uploader => Video.Uploader;
        public BitmapImage Thumbnail => Video.ThumbnailUrl == null ? null
            : new BitmapImage(new Uri(Video.ThumbnailUrl)) { DecodePixelWidth = 160 };

        public string DurationText
        {
            get
            {
                var t = TimeSpan.FromSeconds(Math.Max(0, Video.Duration));
                return t.TotalHours >= 1 ? ((int)t.TotalHours) + t.ToString(@"\:mm\:ss") : t.ToString(@"m\:ss");
            }
        }

        public string Details
        {
            get
            {
                var parts = new List<string>();
                if (Video.ViewCount.HasValue) parts.Add(Format.Views(Video.ViewCount.Value));
                if (!string.IsNullOrEmpty(Video.TextualUploadDate)) parts.Add(Video.TextualUploadDate);
                return string.Join(" · ", parts);
            }
        }
    }

    public static class Format
    {
        public static string Views(long v) =>
            v >= 1000000000 ? (v / 1e9).ToString("0.#") + " Mrd visualizzazioni"
            : v >= 1000000 ? (v / 1e6).ToString("0.#") + " Mln visualizzazioni"
            : v >= 1000 ? (v / 1e3).ToString("0.#") + "K visualizzazioni"
            : v + " visualizzazioni";

        /// <summary>Localization.getDurationString: 1:02:05, 4:07.</summary>
        public static string Duration(long seconds)
        {
            if (seconds < 0) seconds = 0;
            long h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
            return h > 0 ? h + ":" + m.ToString("00") + ":" + s.ToString("00") : m + ":" + s.ToString("00");
        }
    }

    /// <summary>State shared by the pages: subscriptions and the Perle repository.</summary>
    public static class AppState
    {
        public static readonly LocalStorage Storage = new LocalStorage();
        public static readonly PerleRepository Repository = new PerleRepository(Storage);
        public static SubscriptionsData Subscriptions;
        const string SubscriptionsFile = "subscriptions.json";

        public static async Task<SubscriptionsData> LoadSubscriptionsAsync()
        {
            if (Subscriptions != null) return Subscriptions;
            var text = await Storage.ReadTextAsync(SubscriptionsFile);
            if (text == null)
            {
                // first start: the export bundled with this build, if any
                try
                {
                    var bundled = await StorageFile.GetFileFromApplicationUriAsync(new Uri("ms-appx:///Assets/subscriptions.json"));
                    text = await FileIO.ReadTextAsync(bundled);
                    await Storage.WriteTextAsync(SubscriptionsFile, text);
                }
                catch (Exception) { text = "{}"; }
            }
            Subscriptions = SubscriptionsData.Parse(text);
            return Subscriptions;
        }

        public static async Task ImportAsync(string json)
        {
            var data = SubscriptionsData.Parse(json); // throws on invalid files, before overwriting anything
            await Storage.WriteTextAsync(SubscriptionsFile, json);
            Subscriptions = data;
        }
    }

    public sealed class GroupChoice
    {
        public long Id { get; set; }
        public string Name { get; set; }
    }

    public sealed partial class MainPage : Page
    {
        const string GroupKey = "perle_group";

        readonly ObservableCollection<VideoItem> items = new ObservableCollection<VideoItem>();
        PerleCache cache;
        long groupId = SubscriptionsData.GroupAllId;
        int loadGeneration;
        CancellationTokenSource loadCts;
        bool loaded;
        bool settingGroups;

        // last hidden video, for "Annulla"
        VideoItem lastHidden;
        int lastHiddenPosition;
        DispatcherTimer snackbarTimer;

        public MainPage()
        {
            InitializeComponent();
            NavigationCacheMode = NavigationCacheMode.Required;
            List.ItemsSource = items;
        }

        protected override async void OnNavigatedTo(NavigationEventArgs e)
        {
            Perle.Cast.CastService.StateChanged += OnCastStateChanged;
            RenderCastBar(Perle.Cast.CastService.State);
            if (loaded) return;
            loaded = true;
            if (ApplicationData.Current.LocalSettings.Values[GroupKey] is long saved) groupId = saved;
            await FillGroupsAsync();
            await LoadAsync(false);
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
                    result = await Task.Run(() => AppState.Repository.FetchAsync(subs, loadedGroupId,
                        (done, total) => PostProgress(generation, done, total), cts.Token));
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
                InfoText.Text = "Errore: " + ex.Message;
            }
        }

        void PostProgress(int generation, int done, int total)
        {
            var _ = Dispatcher.RunAsync(CoreDispatcherPriority.Normal, () =>
            {
                if (generation == loadGeneration && Progress.Visibility == Visibility.Visible) ShowLoading(done, total);
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
            string when = DateTimeOffset.FromUnixTimeMilliseconds(c.Timestamp).ToLocalTime().ToString("d MMM, HH:mm");
            InfoText.Text = c.FailedChannels > 0
                ? string.Format("Aggiornato {0} · canali saltati per errori: {1}", when, c.FailedChannels)
                : string.Format("Aggiornato {0}", when);
            EmptyText.Visibility = items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            if (items.Count > 0) List.ScrollIntoView(items[0]);
        }

        async void OnGroupChanged(object sender, SelectionChangedEventArgs e)
        {
            if (settingGroups || !(GroupBox.SelectedItem is GroupChoice choice) || choice.Id == groupId) return;
            groupId = choice.Id;
            ApplicationData.Current.LocalSettings.Values[GroupKey] = groupId;
            cache = null;
            await LoadAsync(false);
        }

        /// <summary>Mixes the already downloaded videos again, without downloading anything.</summary>
        async void OnShuffle(object sender, RoutedEventArgs e)
        {
            if (cache != null && Progress.Visibility != Visibility.Visible) await ShowAsync(cache);
        }

        async void OnRefresh(object sender, RoutedEventArgs e) => await LoadAsync(true);

        async void OnShowHidden(object sender, RoutedEventArgs e)
        {
            await AppState.Repository.ClearHiddenAsync();
            if (cache != null && Progress.Visibility != Visibility.Visible) await ShowAsync(cache);
        }

        async void OnImport(object sender, RoutedEventArgs e)
        {
            var picker = new FileOpenPicker();
            picker.FileTypeFilter.Add(".json");
            var file = await picker.PickSingleFileAsync();
            if (file == null) return;
            try
            {
                await AppState.ImportAsync(await FileIO.ReadTextAsync(file));
                await FillGroupsAsync();
                await LoadAsync(true);
            }
            catch (Exception ex) { InfoText.Text = "File non valido: " + ex.Message; }
        }

        void OnVideoClick(object sender, ItemClickEventArgs e)
        {
            var item = (VideoItem)e.ClickedItem;
            Frame.Navigate(typeof(PlayerPage), item.Video.Url);
        }

        void OnHolding(object sender, HoldingRoutedEventArgs e)
        {
            if (e.HoldingState == Windows.UI.Input.HoldingState.Started) ShowMenu(e.OriginalSource as FrameworkElement);
        }

        void OnRightTapped(object sender, RightTappedRoutedEventArgs e) => ShowMenu(e.OriginalSource as FrameworkElement);

        void ShowMenu(FrameworkElement source)
        {
            if (!(source?.DataContext is VideoItem item)) return;
            var menu = new MenuFlyout();
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

        protected override void OnNavigatedFrom(NavigationEventArgs e)
        {
            Perle.Cast.CastService.StateChanged -= OnCastStateChanged;
        }

        void OnCastStateChanged(Perle.Cast.CastState state) =>
            _ = Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Normal, () => RenderCastBar(state));

        /// <summary>CastBar.bind</summary>
        void RenderCastBar(Perle.Cast.CastState state)
        {
            CastBar.Visibility = state.Active ? Visibility.Visible : Visibility.Collapsed;
            if (!state.Active) return;
            CastBarTitle.Text = state.Title ?? "";
            CastBarDevice.Text = string.Format(Perle.Cast.CastStrings.NotificationText, Perle.Cast.CastStatuses.Text(state.Status), state.DeviceName);
            CastBarIcon.Symbol = state.Status == Perle.Cast.CastStatus.Playing ? Symbol.Pause : Symbol.Play;
        }

        void OnCastBarTapped(object sender, TappedRoutedEventArgs e) => CastLauncher.OpenController(Frame);

        void OnCastBarPlayPause(object sender, RoutedEventArgs e) => Perle.Cast.CastService.PlayPause();

        void OnSettings(object sender, RoutedEventArgs e) => Frame.Navigate(typeof(SettingsPage));

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
