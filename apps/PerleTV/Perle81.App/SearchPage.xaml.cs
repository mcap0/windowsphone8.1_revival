/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Search of videos on YouTube (the "Videos" filter), results open in the player.
 */

using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Perle;
using Perle.Extractor;
using Perle.Search;
using Windows.Storage;
using Windows.System;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace Perle81.App
{
    public sealed partial class SearchPage : Page
    {
        readonly ObservableCollection<VideoItem> results = new ObservableCollection<VideoItem>();
        readonly ObservableCollection<ChannelResult> channels = new ObservableCollection<ChannelResult>();
        CancellationTokenSource cts;

        public SearchPage()
        {
            InitializeComponent();
            List.ItemsSource = results;
            Channels.ItemsSource = channels;
            // Back from a video returns to these results instead of an empty search
            NavigationCacheMode = NavigationCacheMode.Required;
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            // the app was closed by the system meanwhile: the page is new, run the last search again
            if (e.NavigationMode == NavigationMode.Back && results.Count == 0 && cts == null)
            {
                string last = ApplicationData.Current.LocalSettings.Values[LastQueryKey] as string;
                if (!string.IsNullOrEmpty(last))
                {
                    Query.Text = last;
                    Run();
                }
            }
        }

        const string LastQueryKey = "lastSearch";

        void OnKeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == VirtualKey.Enter) Run();
        }

        void OnSearchClick(object sender, RoutedEventArgs e) => Run();

        async void Run()
        {
            string query = (Query.Text ?? "").Trim();
            if (query.Length == 0) return;
            ApplicationData.Current.LocalSettings.Values[LastQueryKey] = query;
            if (cts != null) cts.Cancel();
            var mine = cts = new CancellationTokenSource();
            results.Clear();
            channels.Clear();
            Busy.Visibility = Visibility.Visible;
            Status.Text = "Ricerca in corso…";
            try
            {
                var channelsSearch = SearchChannelsAsync(query, mine.Token);
                var found = await YoutubeSearch.SearchVideosAsync(query, mine.Token);
                if (mine.IsCancellationRequested) return;
                foreach (var c in (await channelsSearch).Take(3)) channels.Add(c);
                foreach (var v in found) results.Add(new VideoItem(PerleVideo.From(v)));
                Status.Text = results.Count == 0 ? "Nessun risultato." : results.Count + " risultati";
            }
            catch (OperationCanceledException) { }
            catch (Exception ex)
            {
                if (!mine.IsCancellationRequested) Status.Text = "Errore nella ricerca: " + ex.Message;
            }
            finally
            {
                if (cts == mine) Busy.Visibility = Visibility.Collapsed;
            }
        }

        // the channels are an extra: when their search fails the videos are still shown
        static async Task<List<ChannelResult>> SearchChannelsAsync(string query, CancellationToken ct)
        {
            try
            {
                return await YoutubeSearch.SearchChannelsAsync(query, ct);
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception) { return new List<ChannelResult>(); }
        }

        void OnHolding(object sender, HoldingRoutedEventArgs e)
        {
            if (e.HoldingState == Windows.UI.Input.HoldingState.Started) ShowMenu(e.OriginalSource as FrameworkElement);
        }

        // on touch a long press already raised Holding: the RightTapped on release would open the menu a second time
        void OnRightTapped(object sender, RightTappedRoutedEventArgs e)
        {
            if (e.PointerDeviceType != Windows.Devices.Input.PointerDeviceType.Touch) ShowMenu(e.OriginalSource as FrameworkElement);
        }

        async void ShowMenu(FrameworkElement source)
        {
            var item = source == null ? null : source.DataContext as VideoItem;
            if (item == null) return;
            var menu = new MenuFlyout();
            menu.Items.Add(await WatchLater.MenuItemAsync(item, null));
            var download = new MenuFlyoutItem { Text = "Scarica audio" };
            download.Click += (s, a) => DownloadService.Enqueue(item.Video.Url, item.Title, item.Uploader);
            menu.Items.Add(download);
            if (item.Video.UploaderUrl != null)
            {
                var channel = new MenuFlyoutItem { Text = "Vai al canale" };
                channel.Click += (s, a) => Frame.Navigate(typeof(ChannelPage), item.Video.UploaderUrl);
                menu.Items.Add(channel);
            }
            if (PerleCast.CastUi.IsActive)
            {
                var tv = new MenuFlyoutItem { Text = "Aggiungi alla coda TV" };
                tv.Click += async (s, a) => await PlayerPage.AddToTvQueueAsync(item.Video.Url);
                menu.Items.Add(tv);
            }
            menu.ShowAt(source);
        }

        void OnVideoClick(object sender, ItemClickEventArgs e)
        {
            var item = e.ClickedItem as VideoItem;
            if (item != null) Frame.Navigate(typeof(PlayerPage), item.Video.Url);
        }

        void OnChannelClick(object sender, ItemClickEventArgs e)
        {
            var channel = e.ClickedItem as ChannelResult;
            if (channel != null) Frame.Navigate(typeof(ChannelPage), channel.Url);
        }
    }
}
