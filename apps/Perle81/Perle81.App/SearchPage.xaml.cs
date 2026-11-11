/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Search of videos on YouTube (the "Videos" filter), results open in the player.
 */

using System;
using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using Perle;
using Perle.Extractor;
using Perle.Search;
using Windows.System;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;

namespace Perle81.App
{
    public sealed partial class SearchPage : Page
    {
        readonly ObservableCollection<VideoItem> results = new ObservableCollection<VideoItem>();
        CancellationTokenSource cts;

        public SearchPage()
        {
            InitializeComponent();
            List.ItemsSource = results;
        }

        void OnKeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == VirtualKey.Enter) Run();
        }

        void OnSearchClick(object sender, RoutedEventArgs e) => Run();

        async void Run()
        {
            string query = (Query.Text ?? "").Trim();
            if (query.Length == 0) return;
            if (cts != null) cts.Cancel();
            var mine = cts = new CancellationTokenSource();
            results.Clear();
            Busy.Visibility = Visibility.Visible;
            Status.Text = "Ricerca in corso…";
            try
            {
                var found = await YoutubeSearch.SearchVideosAsync(query, mine.Token);
                if (mine.IsCancellationRequested) return;
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
            download.Click += (s, a) => DownloadService.Enqueue(item.Video.Url, item.Title, item.Uploader);
            menu.Items.Add(download);
            menu.ShowAt(source);
        }

        void OnVideoClick(object sender, ItemClickEventArgs e)
        {
            var item = e.ClickedItem as VideoItem;
            if (item != null) Frame.Navigate(typeof(PlayerPage), item.Video.Url);
        }
    }
}
