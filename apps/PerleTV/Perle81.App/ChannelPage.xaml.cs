/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Videos of a YouTube channel (its Videos tab), with "altri video" for the following pages.
 */

using System;
using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using Perle;
using Perle.Extractor;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Navigation;

namespace Perle81.App
{
    public sealed partial class ChannelPage : Page
    {
        readonly ObservableCollection<VideoItem> results = new ObservableCollection<VideoItem>();
        ChannelTabPage tab;
        CancellationTokenSource cts;
        string channelUrl;
        bool subscribed;

        public ChannelPage()
        {
            InitializeComponent();
            List.ItemsSource = results;
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);
            if (tab == null && cts == null) Load(e.Parameter as string);
        }

        async void Load(string url)
        {
            if (string.IsNullOrEmpty(url)) return;
            var mine = cts = new CancellationTokenSource();
            Busy.Visibility = Visibility.Visible;
            Status.Text = "Caricamento…";
            try
            {
                tab = await YoutubeChannel.GetVideosTabAsync(url, mine.Token);
                if (mine.IsCancellationRequested) return;
                Header.Text = tab.ChannelName ?? "Canale";
                channelUrl = tab.ChannelUrl ?? url;
                await ShowSubscribedAsync();
                AddPage(tab);
                Status.Text = results.Count == 0 ? "Nessun video." : "";
            }
            catch (OperationCanceledException) { }
            catch (Exception ex)
            {
                if (!mine.IsCancellationRequested) Status.Text = "Errore nel caricamento del canale: " + ex.Message;
            }
            finally
            {
                if (cts == mine) Busy.Visibility = Visibility.Collapsed;
            }
        }

        async Task ShowSubscribedAsync()
        {
            subscribed = await AppState.IsSubscribedAsync(channelUrl);
            SubscribeButton.Content = subscribed ? "Iscritto ✓" : "Iscriviti";
            SubscribeButton.Visibility = Visibility.Visible;
        }

        async void OnSubscribeClick(object sender, RoutedEventArgs e)
        {
            if (string.IsNullOrEmpty(channelUrl)) return;
            SubscribeButton.IsEnabled = false;
            try
            {
                await AppState.SetSubscribedAsync(channelUrl, tab == null ? null : tab.ChannelName, !subscribed);
                await ShowSubscribedAsync();
            }
            catch (Exception ex) { Status.Text = "Iscrizione non salvata: " + ex.Message; }
            finally { SubscribeButton.IsEnabled = true; }
        }

        void AddPage(ChannelTabPage page)
        {
            foreach (var v in page.Items) results.Add(new VideoItem(PerleVideo.From(v)));
            MoreButton.Visibility = page.NextPage == null ? Visibility.Collapsed : Visibility.Visible;
        }

        async void OnMoreClick(object sender, RoutedEventArgs e)
        {
            if (tab == null || tab.NextPage == null) return;
            MoreButton.IsEnabled = false;
            Busy.Visibility = Visibility.Visible;
            try
            {
                tab = await YoutubeChannel.GetMoreItemsAsync(tab);
                AddPage(tab);
            }
            catch (Exception ex)
            {
                Status.Text = "Errore nel caricamento dei video: " + ex.Message;
            }
            finally
            {
                MoreButton.IsEnabled = true;
                Busy.Visibility = Visibility.Collapsed;
            }
        }

        void OnVideoClick(object sender, ItemClickEventArgs e)
        {
            var item = e.ClickedItem as VideoItem;
            if (item != null) Frame.Navigate(typeof(PlayerPage), item.Video.Url);
        }
    }
}
